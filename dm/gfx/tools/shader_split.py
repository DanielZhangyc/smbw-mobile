#!/usr/bin/env python3
# 马力欧全部着色器 → dmsh_build.py 认的「包目录」+ 着色器映射表 + 翻译查询作答表（计算着色器的线程组 / 共享内存）。
# 来源（全量扫，不靠文件名）：RomFS 下每个文件（.zs 先 zstd 解压，SARC 逐成员展开）与 ExeFS main（NSO，lz4 解压）里的
#   · BNSH（'BNSH\0\0\0\0'，bfsha / 粒子特效 esetb / UI 布局 blarc / main 内嵌各自带）：
#       +0x60 grsc 块；+0x7c 变体数 u32；+0x80 变体表指针（指针都相对 BNSH 起点）。变体 0x40 字节，取 +0x10 二进制程序；
#       二进制程序 +0x08 起 6 个阶段指针：顶点、外壳、域、几何、像素、计算；阶段代码描述 +0x08 控制段指针、+0x10 代码区指针、+0x18 代码区长、+0x1c 控制段长。
#   · BAHS（agl 的 sharcb：agl / gsys 资源包、Secred 包里的 GameAglShader）：文件头 +0x1c 代码区起点、+0x18 代码区长；
#       表区里每条二进制项 16 字节 {代码偏移（相对代码区，0x100 对齐）, 0, 控制段长 0x880 / 0x88c, 控制段相对本项地址的有符号偏移}。
#       控制段在多个程序间共用，所以按项配对，不能按控制段找代码；项靠「控制段魔数 + 代码区魔数 + 长度自洽」三重核对认出。
#   · Unity（UnityFS 资源包、Data 下的序列化文件，读法见 unityfs.py）：每个 Shader 对象的 Switch 平台子程序，程序代码 = 0x60 头 + 各阶段块。
#       头是 4 个 i32 ×6 数组，下标按 NVN 阶段（0 顶点 1 像素 2 几何 3 外壳 4 域 5 计算）：[0] 未用（0 或暂存大小）、[1] 块偏移（相对 +0x60，−1 = 无此阶段）、[2] 控制段长、[3] 代码区长；
#       块 = 控制段 + 代码区。一个程序的顶点 / 像素阶段即一对。游戏本体的序列化文件（data.unity3d、unity default resources）不带类型树，借资源包里同哈希的 Shader 类型树（第二遍）
# 代码区：0x30 头 + （图形）0x50 SPH + 指令；头魔数图形 0x12345678、计算 0x12345679。控制段 0x98761234 开头，+0x704 = 代码区长，
#   +0x700 = c1 常数段在代码区内的起点；计算着色器 +0x748 线程组 xyz（u32 ×3）、+0x754 共享内存字节（D2R 22 个计算着色器与 PC 反射逐条核对）。
# 阶段：BNSH 取阶段槽；BAHS 取 SPH 第 0 字 bits 10..13（1 顶点 2 外壳 3 域 4 几何 5 像素），计算头的为计算。
# 键 = sha256(控制段前 0x880 字节 + 代码区)[:16]，与宿主 prog_get 一致：本作控制段相同、代码不同的情况很多
#   （SecredUber 16600 个阶段着色器只有 3079 种控制段，其中 6367 处同控制段而代码不同），只哈希控制段会拿错库。
# 输出：<包目录>/<阶段>_<来源组>.bin，每条记录 = 'BTCDBTCD' + 头（+0x28 控制段长、+0x2c 代码区长）补到 0x38 + 代码区 + 控制段，同键只出一份；
#   映射表行与 D2R 的 shader_map.tsv 同列（第 5 列记首次出现的位置）；作答表每个计算着色器一行 {"local", "shared"}（共享内存按 16 字节向上取整，同 D2R 口径）。
# 用法：shader_split.py <RomFS 目录> <ExeFS main> <输出包目录> <输出映射表> <输出作答表> [<输出配对表>]   （解 main 要 lz4：用 dm 的 venv python 跑）   环境 J=并发（缺省 8）
#   带更新包：RomFS 与 main 都写成逗号分隔的列表（基础版在前，如 data/v0/RomFS,data/v327680/RomFS），输出两版的并集（见 main()）
#   Unity 的类型树字符串要 Unity 的公共字符串表，取自第一个 main
# 配对表：BNSH 每个变体（Unity 每个程序）的（顶点键, 像素键），dmsh_build 据此给顶点着色器补上像素着色器读而它没写的输出（见 dmsh_build.py vs_pad）
# All of Mario's shaders → the "package directory" dmsh_build.py understands + the shader map + the translation query answer table (thread group / shared memory of compute shaders).
# Sources (a full scan, not relying on file names): every file under RomFS (.zs is zstd-decompressed first, SARC is expanded member by member) and ExeFS main (NSO, lz4-decompressed), looking for
#   · BNSH ('BNSH\0\0\0\0', carried by bfsha / particle effect esetb / UI layout blarc / embedded in main):
#       +0x60 grsc block; +0x7c variation count u32; +0x80 variation table pointer (pointers are relative to the BNSH start). A variation is 0x40 bytes; the binary program is at +0x10;
#       the binary program has 6 stage pointers from +0x08: vertex, hull, domain, geometry, pixel, compute; the stage code descriptor has +0x08 control pointer, +0x10 code area pointer, +0x18 code area length, +0x1c control length.
#   · BAHS (agl's sharcb: the agl / gsys resource packs, GameAglShader in the Secred pack): file header +0x1c code region start, +0x18 code region length;
#       each binary entry in the table area is 16 bytes {code offset (relative to the code region, 0x100-aligned), 0, control length 0x880 / 0x88c, signed offset of the control relative to this entry's address}.
#       Control sections are shared between programs, so pairing goes by entry, never by finding code from a control; entries are recognized by a triple check of "control magic + code magic + consistent lengths".
#   · Unity (UnityFS bundles and the serialized files under Data, read by unityfs.py): the Switch subprograms of each Shader object; the program code = a 0x60 header + one block per stage.
#       The header is 4 i32 ×6 arrays indexed by NVN stage (0 vertex 1 pixel 2 geometry 3 hull 4 domain 5 compute): [0] unused (0 or a scratch size), [1] block offset (from +0x60, -1 = no such stage), [2] control length, [3] code length;
#       block = control section + code area. A program's vertex / pixel stages form a pair. The game's own serialized files (data.unity3d, unity default resources) carry no type trees and borrow the bundles' Shader type tree of the same hash (second pass)
# Code area: 0x30 header + (graphics) 0x50 SPH + instructions; header magic 0x12345678 for graphics, 0x12345679 for compute. The control section starts with 0x98761234, +0x704 = code area length,
#   +0x700 = start of the c1 constant segment within the code area; for compute +0x748 thread group xyz (u32 ×3), +0x754 shared memory bytes (checked one by one against PC reflection on D2R's 22 compute shaders).
# Stage: BNSH uses the stage slot; BAHS uses bits 10..13 of SPH word 0 (1 vertex 2 hull 3 domain 4 geometry 5 pixel), and a compute header means compute.
# Key = sha256(first 0x880 bytes of the control section + code area)[:16], the same as the host's prog_get: in this game many shaders share a control section but differ in code
#   (SecredUber's 16600 stage shaders have only 3079 distinct control sections, with 6367 cases of same control, different code), so hashing the control alone picks the wrong library.
# Output: <package dir>/<stage>_<source group>.bin, each record = 'BTCDBTCD' + header (+0x28 control length, +0x2c code length) padded to 0x38 + code area + control section, one copy per key;
#   map rows have the same columns as D2R's shader_map.tsv (column 5 records where the key first appeared); the answer table has one row per compute shader {"local", "shared"} (shared memory rounded up to 16 bytes, as D2R did).
# Usage: shader_split.py <RomFS dir> <ExeFS main> <output package dir> <output map> <output answer table> [<output pair table>]   (decompressing main needs lz4: run with dm's venv python)   Environment J=concurrency (default 8)
#   With an update: give RomFS and main as comma-separated lists (base game first, e.g. data/v0/RomFS,data/v327680/RomFS); the output is the union of both versions (see main())
#   Unity type trees need Unity's common string table, which is taken from the first main
# Pair table: (vertex key, pixel key) of every BNSH variation (every Unity program); dmsh_build uses it to give vertex shaders the outputs their pixel shaders read but they do not write (see dmsh_build.py vs_pad)
import collections, concurrent.futures as cf, hashlib, json, os, re, struct, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../games')); import game as _game
GAME = _game.load()   # 当前游戏档案（DM_GAME，缺省 smbw）：来源组规则与表头说明 | the current game profile (DM_GAME, default smbw): source group rules and the table header note
GROUPS, NOTE = GAME.pairs('shader_groups'), GAME.get('shader_note')

CTL_MAGIC, GFX_MAGIC, CS_MAGIC = bytes.fromhex('34127698'), bytes.fromhex('78563412'), bytes.fromhex('79563412')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}
ZH = {'vs': '顶点', 'hs': '外壳', 'ds': '域', 'gs': '几何', 'ps': '像素', 'cs': '计算'}

def red(msg): sys.exit('【红】' + msg)

def check(where, st, ctl, code):
    """控制段与代码区自洽；不自洽返回原因 | control section and code area consistent; returns the reason if not"""
    if ctl[:4] != CTL_MAGIC or len(ctl) < 0x880: return '控制段异常'
    if code[:4] != (CS_MAGIC if st == 'cs' else GFX_MAGIC): return '代码区头魔数与阶段不符'
    if struct.unpack_from('<I', ctl, 0x704)[0] != len(code): return '控制段 +0x704 与代码区长不符'
    return None

def bnsh_shaders(d, base, label):
    if d[base + 0x60:base + 0x64] != b'grsc': red(f'{label} BNSH @{base:#x} 没有 grsc 块')
    nvar, pvar = struct.unpack_from('<IQ', d, base + 0x7c)
    out = []
    for v in range(nvar):
        prog = struct.unpack_from('<Q', d, base + pvar + 0x40 * v + 0x10)[0]
        if not prog: continue
        for si, st in enumerate(STAGES):
            sc = struct.unpack_from('<Q', d, base + prog + 8 + 8 * si)[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            why = check(label, st, ctl, code)
            if why: red(f'{label} BNSH @{base:#x} 变体 {v} {st}：{why}')
            out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
    return out

def bahs_shaders(d, label):
    region, rsz = struct.unpack_from('<I', d, 0x1c)[0], struct.unpack_from('<I', d, 0x18)[0]
    if region + rsz > len(d): red(f'{label} BAHS 代码区越界')
    out = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        if z or csz not in (0x880, 0x88c) or off % 0x100: continue
        c = a + rel
        if c < 0 or c + csz > region or d[c:c + 4] != CTL_MAGIC: continue
        n = struct.unpack_from('<I', d, c + 0x704)[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = 'cs' if code[:4] == CS_MAGIC else SPH_STAGE.get((struct.unpack_from('<I', code, 0x30)[0] >> 10) & 0xf) if code[:4] == GFX_MAGIC else None
        if not st or check(label, st, ctl, code): continue
        out.append((f'{label}#BAHS@{a:#x}', st, ctl, code))
    return out

def sarc_members(d):
    hl, bom, size, doff = struct.unpack_from('<HHII', d, 4)
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        nm = d[names + (attr & 0xffffff) * 4:].split(b'\0')[0].decode('utf-8', 'replace') if attr >> 24 else f'#{i}'
        yield nm, d[doff + s:doff + e]

NV_STAGES = ['vs', 'ps', 'gs', 'hs', 'ds', 'cs']   # NVN 阶段序 | NVN stage order
COMMON, TREES = None, {}   # Unity 公共字符串表、借用的类型树（进程池初始化时给） | Unity common strings and borrowed type trees (given when the pool starts)

def _pool_init(common, trees):
    global COMMON, TREES
    COMMON, TREES = common, trees

def unity_program(code, where):
    """Unity 的 Switch 程序代码 → [(阶段, 控制段, 代码区)] | Unity's Switch program code → [(stage, control, code)]"""
    h = struct.unpack_from('<24i', code, 0); out = []; end = 0x60
    for i, st in enumerate(NV_STAGES):
        off, nctl, ncode = h[6 + i], h[12 + i], h[18 + i]
        if off == -1: continue
        a = 0x60 + off; ctl, cd = code[a:a + nctl], code[a + nctl:a + nctl + ncode]
        why = check(where, st, ctl, cd) if a + nctl + ncode <= len(code) else '块越界'
        if why: red(f'{where} {st}：{why}')
        out.append((st, ctl, cd)); end = max(end, a + nctl + ncode)
    if end != len(code): red(f'{where}：程序代码 {len(code):#x} 字节，各阶段块只到 {end:#x}')
    return out

def unity_shaders(d, label, out, trees):
    """UnityFS 包或序列化文件里的 Shader → out；没有类型树可用时抛 NoTree | Shaders in a UnityFS bundle or serialized file → out; raises NoTree when no type tree is available"""
    import unityfs
    if COMMON is None: red(f'{label}：是 Unity 资源，但 main 里没有 Unity 公共字符串表')
    sers = unityfs.bundle_files(d) if d[:8] == b'UnityFS\0' else [('', d)]
    for node, x in sers:
        if not unityfs.is_serialized(x): continue
        for cid, pid, o in unityfs.serialized_objects(x, COMMON, {unityfs.CLASS_SHADER}, trees):
            name = o['m_ParsedForm']['m_Name'] if 'm_ParsedForm' in o else o.get('m_Name', '?')
            for plat, k, sp in unityfs.shader_programs(o):
                where = f'{label}{":" + node if node else ""}#{name}#var{plat}.{k}'
                code = unityfs.subprogram(sp)[4]
                out.extend((where, st, ctl, cd) for st, ctl, cd in unity_program(code, where))

def scan_blob(d, label, out):
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d): scan_blob(m, f'{label}/{nm}', out)
        return
    if d[:4] == b'BAHS': out.extend(bahs_shaders(d, label)); return
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc': out.extend(bnsh_shaders(d, m.start(), label))

def group(label):
    """来源组（包文件名的后半） | source group (second half of the package file name)"""
    if label.startswith('main'): return 'main'
    for k, g in GROUPS:   # 游戏档案 shader_groups，按顺序取第一个 | the profile's shader_groups, first match wins
        if k in label: return g
    return 'other'

def var_pairs(found):
    """BNSH 同一变体里的（顶点键, 像素键） | (vertex key, pixel key) within the same BNSH variation"""
    var = collections.defaultdict(dict)
    for where, st, ctl, code in found:
        if '#var' in where: var[where][st] = hashlib.sha256(ctl[:0x880] + code).hexdigest()[:16]
    return {(v['vs'], v['ps']) for v in var.values() if 'vs' in v and 'ps' in v}

def scan_file(args):
    """→ (相对路径, {键: …}, 配对, 额外)；额外 = 本文件的 Unity 类型树，或 'defer'（缺类型树，第二遍再扫） | → (relative path, {key: …}, pairs, extra); extra = this file's Unity type trees, or 'defer' (no type tree, scanned again in the second pass)"""
    path, rel = args
    d = subprocess.run(['zstd', '-d', '-c', '-q', path], capture_output=True, check=True).stdout if path.endswith('.zs') else open(path, 'rb').read()
    found = []; trees = {}
    if d[:8] == b'UnityFS\0' or (d[8:11] == b'\0\0\0' and __import__('unityfs').is_serialized(d)):
        import unityfs
        try: unity_shaders(d, '/' + rel, found, trees if not TREES else dict(TREES))
        except unityfs.NoTree: return rel, {}, set(), 'defer'
    elif b'BNSH\0\0\0\0' not in d and b'BAHS' not in d: return rel, {}, set(), {}
    else: scan_blob(d, '/' + rel, found)
    uniq = {}
    for where, st, ctl, code in found:
        key = hashlib.sha256(ctl[:0x880] + code).hexdigest()[:16]
        if key in uniq and (uniq[key][2] != ctl or uniq[key][3] != code): red(f'键 {key} 相同而控制段或代码区不同（{where}）')
        uniq.setdefault(key, (where, st, ctl, code))
    return rel, uniq, var_pairs(found), trees

def nso_image(path):
    import lz4.block
    d = open(path, 'rb').read()
    flags = struct.unpack_from('<I', d, 0xc)[0]; segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img)

def main():
    romfs_l, main_l, pkg, mapout, factsout = sys.argv[1:6]
    pairsout = sys.argv[6] if len(sys.argv) > 6 else None
    # 多个版本（基础版 + 更新）：RomFS 与 main 各用逗号分隔、一一对应，输出取并集（键是内容哈希，各版共用的只出一份）。
    # 第一个版本的来源列不变；之后的版本以其工作区目录名（如 v327680）为前缀：/v327680/Shader/…、main@v327680，排在第一个版本之后，原有行的包内序号不变。
    # Several versions (base game + update): the RomFS and main arguments are comma-separated and paired one to one; the output is their union (keys are content hashes, shaders shared by versions appear once).
    # The first version's source column is unchanged; later versions are prefixed with their workspace directory name (e.g. v327680): /v327680/Shader/…, main@v327680. They sort after the first version, so existing rows keep their package indices.
    romfs_v, main_v = romfs_l.split(','), main_l.split(',')
    if len(romfs_v) != len(main_v): red(f'RomFS {len(romfs_v)} 个、main {len(main_v)} 个，要一一对应')
    files, results, common = [], [], None
    for i, (romfs, mainnso) in enumerate(zip(romfs_v, main_v)):
        tag = os.path.basename(os.path.dirname(os.path.abspath(romfs))) if i else ''
        files += sorted((os.path.join(dp, f), (tag + '/' if tag else '') + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
        img = nso_image(mainnso)
        if not i:
            import unityfs
            try: common = unityfs.common_strings(img)
            except ValueError: common = None   # 不是 Unity 游戏 | not a Unity game
        found = []; scan_blob(img, 'main' + ('@' + tag if tag else ''), found)
        results.append(('main', {hashlib.sha256(c[:0x880] + k).hexdigest()[:16]: (w, s, c, k) for w, s, c, k in found}, var_pairs(found), {}))
    keys = {}
    # 第一遍：全部文件；Unity 资源包带类型树，顺手收下 Shader 的树。第二遍：没带类型树的 Unity 序列化文件借这些树再扫
    # First pass: every file; Unity bundles carry type trees, and the Shader trees are collected on the way. Second pass: Unity serialized files without type trees are scanned again with the borrowed trees
    J = int(os.environ.get('J', '8'))
    with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, {})) as ex: results += list(ex.map(scan_file, files, chunksize=16))
    trees, defer = {}, []
    for rel, uniq, pr, extra in results:
        if extra == 'defer': defer.append(rel)
        elif extra: trees.update(extra)
    if defer:
        byrel = {r: p for p, r in files}
        with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(common, trees)) as ex: again = list(ex.map(scan_file, [(byrel[r], r) for r in defer]))
        for rel, uniq, pr, extra in again:
            if extra == 'defer': red(f'{rel}：Unity 序列化文件没有类型树，资源包里也没有同哈希的 Shader 类型树可借')
        results = [r for r in results if r[3] != 'defer'] + again
    per_src = collections.Counter(); pairs = set()
    for rel, uniq, pr, _ in results:
        pairs |= pr
        for key, v in uniq.items():
            if key in keys:
                if keys[key][2] != v[2] or keys[key][3] != v[3]: red(f'键 {key} 相同而控制段或代码区不同（{v[0]} / {keys[key][0]}）')
                continue
            keys[key] = v; per_src[group(v[0])] += 1
    os.makedirs(pkg, exist_ok=True)
    for f in os.listdir(pkg):
        if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
    recs = collections.defaultdict(list); rows = []; facts = {}; stat = collections.Counter()
    for key in sorted(keys, key=lambda k: (group(keys[k][0]), keys[k][0], k)):   # 同输入逐字节同样的输出 | the same input gives byte-identical output
        where, st, ctl, code = keys[key]
        name = f'{st}_{group(where)}.bin'
        rec = bytearray(b'BTCDBTCD' + bytes(0x30)); struct.pack_into('<2I', rec, 0x28, len(ctl), len(code)); rec += code + ctl
        if rec.find(CTL_MAGIC) != 0x38 + len(code): red(f'键 {key}：代码区里出现控制段魔数，dmsh_build 会切错')
        if rec.count(b'BTCDBTCD') != 1: red(f'键 {key}：记录内出现记录分隔符')
        rows.append((name, len(recs[name]), ZH[st], key, where)); recs[name].append(bytes(rec)); stat[st] += 1
        if st == 'cs':
            x, y, z = struct.unpack_from('<3I', ctl, 0x748); sh = struct.unpack_from('<I', ctl, 0x754)[0]
            if not x * y * z: red(f'计算着色器 {key} 控制段 +0x748 线程组为 0（{where}）')
            facts[key] = {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}
    for name, rr in recs.items():
        with open(os.path.join(pkg, name), 'wb') as f: f.write(b''.join(rr))
    with open(mapout, 'w') as f:
        kinds = ' / '.join(k for k, hit in (('BNSH / BAHS', any('#BNSH@' in r[4] or '#BAHS@' in r[4] for r in rows)), ('Unity Shader', any('#var' in r[4] and '#BNSH@' not in r[4] for r in rows))) if hit)
        f.write(f'# kind: evidence  {NOTE}（RomFS + main 里的 {kinds}）→ 控制段 + 代码区键（dm/gfx/tools/shader_split.py）\n')
        f.write('# 第 5 列记每个键首次出现的位置；本作没有 PC 版配对\n')
        f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    with open(factsout, 'w') as f:
        f.write('# 翻译查询的作答表（dm/gfx/tools/shader_split.py 生成，勿手改）：键 <TAB> 事实 JSON。本作没有 PC 版可配对：只有计算着色器的线程组 / 共享内存（取自控制段），其余查询按翻译器缺省\n')
        for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
    if pairsout:
        with open(pairsout, 'w') as f:
            f.write('# kind: evidence  BNSH 变体里的顶点 / 像素着色器配对（dm/gfx/tools/shader_split.py 生成，勿手改）：顶点键 <TAB> 像素键 | vertex / pixel shader pairs within BNSH variations (generated, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(pairs): f.write(f'{a}\t{b}\n')
    print(f'着色器 {len(rows)} 个（' + '，'.join(f'{ZH[s]} {n}' for s, n in sorted(stat.items())) + '）；来源 ' + '，'.join(f'{g} {n}' for g, n in per_src.most_common()) + f'；计算着色器作答 {len(facts)} 行' + (f'；变体配对 {len(pairs)} 对' if pairsout else ''))

if __name__ == '__main__': main()
