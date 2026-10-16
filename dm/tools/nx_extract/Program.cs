// NSP 解包：nx_extract <prod.keys> <基础 NSP> <更新 NSP 或 -> <输出目录> [比对用的基础 RomFS 目录] [--manifest]
// 输出 <输出目录>/ExeFS 与 <输出目录>/RomFS（有更新包时是合并后的 RomFS）；给了基础 RomFS 目录时，内容与之相同的文件做成 APFS 克隆（不占新空间）。
// --manifest：RomFS 不写出，只写清单 <输出目录>/romfs_manifest.tsv（路径、大小、头 32 字节），给新游戏粗查用（大作的 RomFS 十几 GB）。
// 标题密钥取自 NSP 里的通用票据（.tik），只需 prod.keys；密钥值一律不打印。
// NSP extraction: nx_extract <prod.keys> <base NSP> <update NSP or -> <output dir> [base RomFS dir to compare against] [--manifest]
// Writes <output dir>/ExeFS and <output dir>/RomFS (the merged RomFS when there is an update); with a base RomFS dir, files whose content matches it become APFS clones (no new space).
// --manifest: the RomFS is not written, only a list <output dir>/romfs_manifest.tsv (path, size, first 32 bytes), for surveying a new game (a large game's RomFS is well over 10 GB).
// Title keys come from the common tickets (.tik) inside the NSPs, so only prod.keys is needed; key values are never printed.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using LibHac.Common;
using LibHac.Common.Keys;
using LibHac.Fs;
using LibHac.Fs.Fsa;
using LibHac.FsSystem;
using LibHac.Spl;
using LibHac.Tools.Fs;
using LibHac.Tools.FsSystem;
using LibHac.Tools.FsSystem.NcaUtils;
using LibHac.Tools.FsSystem.RomFs;
using Path = System.IO.Path;

static class P {
    [DllImport("libc", SetLastError = true)] static extern int clonefile(string src, string dst, uint flags);

    static int Main(string[] a) {
        bool manifest = Array.IndexOf(a, "--manifest") >= 0; a = Array.FindAll(a, s => s != "--manifest");
        if (a.Length < 4) { Console.Error.WriteLine("用法 | usage: nx_extract <prod.keys> <base.nsp> <update.nsp|-> <out dir> [base RomFS dir] [--manifest]"); return 2; }
        KeySet ks = ExternalKeyReader.ReadKeyFile(a[0]);
        var basePkg = OpenNsp(ks, a[1]); var updPkg = a[2] == "-" ? null : OpenNsp(ks, a[2]);
        Nca baseProg = Pick(basePkg, NcaContentType.Program) ?? throw new Exception("基础 NSP 里没有程序 NCA | no Program NCA in the base NSP");
        Nca updProg = updPkg == null ? null : Pick(updPkg, NcaContentType.Program) ?? throw new Exception("更新 NSP 里没有程序 NCA | no Program NCA in the update NSP");
        Nca ctrl = Pick(updPkg ?? basePkg, NcaContentType.Control);
        Console.WriteLine($"标题 | title {baseProg.Header.TitleId:x16}" + (updProg != null ? $"，更新 | update {updProg.Header.TitleId:x16}" : ""));
        string outDir = a[3], cmpDir = a.Length > 4 ? a[4] : null;
        if (ctrl != null) {   // 控制数据（NACP）：显示版本与游戏名，原样存一份 <输出目录>/control.nacp 供出包取名 | control data (NACP): display version and game name; a raw copy goes to <output dir>/control.nacp for naming the app
            byte[] nacp = Nacp(ctrl);
            Console.WriteLine($"显示版本 | display version {System.Text.Encoding.ASCII.GetString(nacp, 0x3060, 0x10).TrimEnd('\0')}");
            Console.WriteLine($"名称 | name {TitleName(nacp)}");
            Directory.CreateDirectory(outDir); File.WriteAllBytes(Path.Combine(outDir, "control.nacp"), nacp);
        }
        // 代码区：更新包的 ExeFS 是完整的，叠在基础上打开也一样 | Code section: the update's ExeFS is complete; opening it layered over the base gives the same result
        IFileSystem exefs = updProg != null ? baseProg.OpenFileSystemWithPatch(updProg, NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenFileSystem(NcaSectionType.Code, IntegrityCheckLevel.ErrorOnInvalid);
        Copy(exefs, Path.Combine(outDir, "ExeFS"), null);
        string[] mods = Directory.GetFiles(Path.Combine(outDir, "ExeFS")); Array.Sort(mods, StringComparer.Ordinal);
        foreach (string p in mods) {   // 每个 NSO 模块（main、sdk、subsdk*、rtld…）的构建号 | build ID of every NSO module (main, sdk, subsdk*, rtld...)
            if (new FileInfo(p).Length < 0x50) continue;
            byte[] h = new byte[0x50]; using (var f = File.OpenRead(p)) f.ReadExactly(h);
            if (h[0] == 'N' && h[1] == 'S' && h[2] == 'O' && h[3] == '0') Console.WriteLine($"{Path.GetFileName(p)} 构建号 | build id {Convert.ToHexString(h, 0x40, 16).ToLowerInvariant()}");
        }
        IStorage romfs = updProg != null ? baseProg.OpenStorageWithPatch(updProg, NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid) : baseProg.OpenStorage(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        if (manifest) Manifest(new RomFsFileSystem(romfs), Path.Combine(outDir, "romfs_manifest.tsv"));
        else Copy(new RomFsFileSystem(romfs), Path.Combine(outDir, "RomFS"), cmpDir);
        return 0;
    }

    // 打开 NSP（PFS0），导入其中的通用票据，列出所有 NCA | Open an NSP (PFS0), import its common tickets, list all NCAs
    static List<Nca> OpenNsp(KeySet ks, string path) {
        var pfs = new PartitionFileSystem();
        pfs.Initialize(new LocalStorage(path, FileAccess.Read)).ThrowIfFailure();
        var ncas = new List<Nca>();
        foreach (DirectoryEntryEx e in pfs.EnumerateEntries("*", SearchOptions.Default)) {
            using var f = new UniqueRef<IFile>();
            pfs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            IStorage st = f.Release().AsStorage();
            if (e.Name.EndsWith(".tik", StringComparison.OrdinalIgnoreCase)) {
                byte[] t = new byte[0x2c0]; st.Read(0, t).ThrowIfFailure();
                if (t[0x281] != 0) { Console.Error.WriteLine($"【红】{e.Name} 不是通用票据，标题密钥要另给 | is not a common ticket; supply the title key separately"); continue; }
                ks.ExternalKeySet.Add(new RightsId(t.AsSpan(0x2a0, 0x10)), new AccessKey(t.AsSpan(0x180, 0x10))).ThrowIfFailure();
            } else if (e.Name.EndsWith(".nca", StringComparison.OrdinalIgnoreCase)) ncas.Add(new Nca(ks, st));
        }
        return ncas;
    }
    static Nca Pick(List<Nca> l, NcaContentType t) { foreach (Nca n in l) if (n.Header.ContentType == t) return n; return null; }

    static byte[] Nacp(Nca ctrl) {   // 整份 control.nacp（0x4000 字节） | the whole control.nacp (0x4000 bytes)
        IFileSystem fs = ctrl.OpenFileSystem(NcaSectionType.Data, IntegrityCheckLevel.ErrorOnInvalid);
        using var f = new UniqueRef<IFile>();
        fs.OpenFile(ref f.Ref, "/control.nacp".ToU8Span(), OpenMode.Read).ThrowIfFailure();
        f.Get.GetSize(out long n).ThrowIfFailure();
        byte[] v = new byte[n]; f.Get.Read(out _, 0, v, ReadOption.None).ThrowIfFailure();
        return v;
    }
    // 游戏名：NACP 开头 16 种语言各 0x300 字节（名称 0x200 + 发行商 0x100），取第一个非空的（0 = 美式英语）；DisplayVersion 在 0x3060
    //   新 SDK 的 NACP（如马力欧 1.2.1）把这 0x3000 字节压缩：u16 长度 + raw deflate，解开是 32 种语言 × 0x300。LibHac 0.19 不认，这里自己解
    // Game name: the NACP starts with 16 language entries of 0x300 bytes each (name 0x200 + publisher 0x100); take the first non-empty one (0 = American English); DisplayVersion is at 0x3060
    //   NACPs from newer SDKs (such as Mario 1.2.1) compress these 0x3000 bytes: a u16 size + raw deflate, inflating to 32 languages × 0x300. LibHac 0.19 does not know this, so it is decoded here
    static string TitleName(byte[] nacp) {
        byte[] t = nacp; int n = nacp[0] | nacp[1] << 8;
        if (n > 0 && n <= 0x2ffe) try {
            using var z = new System.IO.Compression.DeflateStream(new MemoryStream(nacp, 2, n), System.IO.Compression.CompressionMode.Decompress);
            var m = new MemoryStream(); z.CopyTo(m);
            if (m.Length == 0x6000) t = m.ToArray();   // 长度不对就是未压缩的名称碰巧能解 | a wrong length means an uncompressed name that happened to inflate
        } catch (InvalidDataException) { }   // 未压缩 | not compressed
        for (int i = 0; i < (t == nacp ? 16 : 32); i++) {
            int z = Array.IndexOf(t, (byte)0, i * 0x300, 0x200), len = (z < 0 ? i * 0x300 + 0x200 : z) - i * 0x300;
            if (len > 0) return System.Text.Encoding.UTF8.GetString(t, i * 0x300, len);
        }
        return "";
    }

    // 逐文件拷出；与比对目录同路径、同内容的文件改做 APFS 克隆 | Copy out file by file; files identical to the same path in the compare dir become APFS clones instead
    static void Copy(IFileSystem fs, string dst, string cmpDir) {
        long n = 0, cloned = 0, bytes = 0, written = 0; byte[] buf = new byte[4 << 20], cb = new byte[4 << 20];
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            string rel = e.FullPath.TrimStart('/'), op = Path.Combine(dst, rel);
            if (e.Type == DirectoryEntryType.Directory) { Directory.CreateDirectory(op); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(op)); n++; bytes += e.Size;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            string cp = cmpDir == null ? null : Path.Combine(cmpDir, rel);
            if (cp != null && File.Exists(cp) && new FileInfo(cp).Length == e.Size && Same(f.Get, cp, e.Size, buf, cb)) {
                if (File.Exists(op)) File.Delete(op);
                if (clonefile(cp, op, 0) != 0) throw new IOException($"clonefile {cp} → {op} 失败 | failed: errno {Marshal.GetLastWin32Error()}");
                cloned++; continue;
            }
            using var o = File.Create(op);
            for (long off = 0; off < e.Size;) {
                f.Get.Read(out long got, off, buf.AsSpan(0, (int)Math.Min(buf.Length, e.Size - off)), ReadOption.None).ThrowIfFailure();
                if (got <= 0) throw new IOException($"读不出 | cannot read {e.FullPath} @{off}");
                o.Write(buf, 0, (int)got); off += got;
            }
            written += e.Size;
        }
        Console.WriteLine($"{dst}：{n} 个文件 {bytes / 1048576.0:F1} MB，其中 {cloned} 个与比对目录相同（克隆），新写 {written / 1048576.0:F1} MB | {n} files {bytes / 1048576.0:F1} MB, {cloned} identical to the compare dir (cloned), {written / 1048576.0:F1} MB newly written");
    }
    // 只列 RomFS 清单（路径、大小、头 32 字节），不写游戏文件 | List the RomFS only (path, size, first 32 bytes) without writing game files
    static void Manifest(IFileSystem fs, string path) {
        long n = 0, bytes = 0; byte[] h = new byte[32];
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        using var o = new StreamWriter(path);
        o.WriteLine("# 路径\t字节\t头 32 字节（十六进制） | path\tbytes\tfirst 32 bytes (hex)");
        foreach (DirectoryEntryEx e in fs.EnumerateEntries("*", SearchOptions.RecurseSubdirectories)) {
            if (e.Type == DirectoryEntryType.Directory) continue;
            using var f = new UniqueRef<IFile>();
            fs.OpenFile(ref f.Ref, e.FullPath.ToU8Span(), OpenMode.Read).ThrowIfFailure();
            long got = 0; int want = (int)Math.Min(h.Length, e.Size);
            if (want > 0) f.Get.Read(out got, 0, h.AsSpan(0, want), ReadOption.None).ThrowIfFailure();
            o.WriteLine($"{e.FullPath}\t{e.Size}\t{Convert.ToHexString(h, 0, (int)got).ToLowerInvariant()}");
            n++; bytes += e.Size;
        }
        Console.WriteLine($"{path}：RomFS {n} 个文件 {bytes / 1048576.0:F1} MB（只列清单）| RomFS {n} files {bytes / 1048576.0:F1} MB (manifest only)");
    }
    static bool Same(IFile f, string path, long size, byte[] buf, byte[] cb) {
        using var c = File.OpenRead(path);
        for (long off = 0; off < size;) {
            int want = (int)Math.Min(buf.Length, size - off);
            f.Read(out long got, off, buf.AsSpan(0, want), ReadOption.None).ThrowIfFailure();
            c.ReadExactly(cb, 0, (int)got);
            if (!buf.AsSpan(0, (int)got).SequenceEqual(cb.AsSpan(0, (int)got))) return false;
            off += got;
        }
        return true;
    }
}
