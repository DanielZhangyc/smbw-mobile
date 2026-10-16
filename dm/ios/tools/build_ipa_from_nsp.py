#!/usr/bin/env python3
# 从 NSP 一路出未签名 IPA：认游戏（标题号 → dm/games/<名>/ 游戏档案）→ 解包（nx_extract）→ 改写代码模块（patch_nso.py）→ 拆出着色器并与档案里的表核对（shader_split.py）
#   → iOS 着色器库（dmsh_build.py）→ build_ipa.sh。没有对应档案的游戏只做粗查（<NSP 文件夹>/<标题号>_survey/survey.md），不出 IPA。
# 在 Spyder 里打开本文件按 F5 运行（也可 python3 build_ipa_from_nsp.py）；下面「设置」留空的两项运行时会问。
# 只用三样东西：本仓库、放一个游戏 NSP 的文件夹（基础版 + 档案 shader_versions 列的更新都要；DLC 跳过，本移植不读追加内容）、prod.keys。
# 所有中间产物都在 NSP 文件夹里的 <名>_build/：extract/（解出的游戏文件，出包后默认删掉）、tools/、patched_*/、shader/（着色器包与缓存，留着下次出包快）、logs/（每步的完整输出）、tmp/、ipa/。
# 着色器翻译器用仓内的 dm/gfx/tools/dmsh_xlate_bin（与 dmsh_chain.lock 指纹一致的那份；重编出的 dll 指纹不同，dmsh_build 会拒绝）。
# 运行本文件的 Python 要有 lz4、capstone、fonttools；另需 Xcode、xcodegen、.NET SDK 10、zstd、spirv-cross。马力欧首次约 35 分钟（大头是着色器库），再跑约 10 分钟（着色器库走缓存）。密钥值一律不打印。
# Unsigned IPA straight from the NSPs: identify the game (title ID → game profile dm/games/<id>/) → extract (nx_extract) → rewrite the code modules (patch_nso.py) → split out the shaders and check them against the profile's tables (shader_split.py)
#   → iOS shader libraries (dmsh_build.py) → build_ipa.sh. A game with no matching profile is only surveyed (<NSP folder>/<title ID>_survey/survey.md); no IPA.
# Open this file in Spyder and press F5 (or run python3 build_ipa_from_nsp.py); the two settings below that are left empty are asked for when it runs.
# Uses only three things: this repository, a folder holding one game's NSPs (the base game plus the updates listed in the profile's shader_versions are required; DLC is skipped, this port does not load add-on content), and prod.keys.
# Every intermediate file is under <id>_build/ in the NSP folder: extract/ (the extracted game files, deleted after the IPA is built by default), tools/, patched_*/, shader/ (shader package and caches, kept so the next build is fast), logs/ (each step's full output), tmp/, ipa/.
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin (the build whose fingerprints match dmsh_chain.lock; a rebuilt dll has different fingerprints and dmsh_build refuses it).
# The Python running this file needs lz4, capstone and fonttools; also needed: Xcode, xcodegen, .NET SDK 10, zstd, spirv-cross. For Mario the first run takes about 35 minutes (mostly the shader libraries); later runs take about 10 (the shader libraries come from the cache). Key values are never printed.
import collections, ctypes, getpass, os, re, shutil, struct, subprocess, sys, threading, time, zlib
from pathlib import Path

# ---- 设置 | Settings ----
NSP_DIR = ''                            # 放 NSP 的文件夹（留空 = 运行时问） | folder holding the NSPs (empty = ask at run time)
PROD_KEYS = ''                          # prod.keys 的路径（留空 = 运行时问） | path of prod.keys (empty = ask at run time)
PACKAGE = ''                            # 打进 IPA 的版本（留空 = 游戏档案的 package；马力欧 'v327680' 即 1.2.1，或 'v0'） | version packed into the IPA (empty = the profile's package; Mario 'v327680' i.e. 1.2.1, or 'v0')
OUT_IPA = ''                            # 输出的 IPA（留空 = ~/Desktop/<档案的 ipa>） | output IPA (empty = ~/Desktop/<the profile's ipa>)
APP_NAME = ''                           # 主屏上的 App 名（留空 = 基础版 NSP 里的游戏名） | app name on the home screen (empty = the game name in the base NSP)
KEEP_EXTRACTED = False                  # 出包后保留解出的游戏文件（下次省去解包；与游戏文件一样大） | keep the extracted game files after the IPA is built (saves extraction next time; as big as the game files)
DM_ENV_FILE = ''                        # 可选：诊断包的环境开关文件（每行 KEY=VALUE） | optional: environment switches for a diagnostic build (one KEY=VALUE per line)

BEAT = 60                               # quiet 步骤的进度间隔（秒） | progress interval of quiet steps (seconds)
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', '/usr/local/share/dotnet', os.path.expanduser('~/.dotnet')]   # Spyder 等图形程序拿不到 shell 的 PATH | GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def say(msg):
    print(f'[{time.strftime("%H:%M:%S")}] {msg}', flush=True)


def ask(prompt):
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # 拖进来的路径带引号或转义空格 | dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, quiet=False):
    """跑一个子进程，全部输出写进 log；非零退出即停。quiet：控制台每分钟报一次进度，结束时只打末尾几行（着色器出包有上万行编译器警告）
    Run a subprocess with all its output written to log; stop on a non-zero exit. quiet: report progress to the console once a minute and print only the last lines at the end (the shader build emits tens of thousands of compiler warnings)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail, n, done = collections.deque(maxlen=40), [0], threading.Event()
    def beat():   # 子进程不出声时也报（走缓存的着色器出包几分钟没有输出） | report even while the subprocess is silent (a cached shader build prints nothing for minutes)
        t0 = time.time()
        while not done.wait(BEAT): say(f'……仍在进行，{(time.time() - t0) / 60:.0f} 分钟 | still running, {(time.time() - t0) / 60:.0f} min')
    p = subprocess.Popen(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace', bufsize=1)
    if quiet: threading.Thread(target=beat, daemon=True).start()
    try:
        with open(log, 'w') as f:
            for line in p.stdout:
                f.write(line); tail.append(line); n[0] += 1
                if not quiet: print(line, end='', flush=True)
        p.wait()
    except KeyboardInterrupt:
        p.terminate(); p.wait(); raise
    finally:
        done.set()
    if quiet: print(''.join(list(tail)[-40 if p.returncode else -12:]), end='', flush=True)
    if p.returncode:
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        raise BuildError(f'{tool} 退出码 | exit code {p.returncode}（完整输出 | full output: {log}）')


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def rmtree(p):
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p)


def swap_in(tmp, dst):   # 新产物整个换上（失败时旧的不受影响） | swap the new output in as a whole (the old one is untouched on failure)
    rmtree(dst); tmp.rename(dst)


def free_gb(p):
    while not p.exists(): p = p.parent
    return shutil.disk_usage(p).free / 2**30


# ---- NSP 识别 | NSP identification ----
def pfs0_names(path):   # NSP = PFS0：列出里面的文件名 | NSP = PFS0: list the file names inside
    with open(path, 'rb') as f:
        h = f.read(16)
        if h[:4] != b'PFS0': return []
        n, st = struct.unpack_from('<II', h, 4)
        ent, strs = f.read(n * 0x18), f.read(st)
    return [strs[o:strs.index(b'\0', o)].decode(errors='replace') for o in (struct.unpack_from('<I', ent, i * 0x18 + 0x10)[0] for i in range(n))]


def title_of(path):   # 文件名里的 [标题号]，没有就取票据名（权利号前 16 位 = 标题号） | [title ID] in the file name, else the ticket name (the first 16 digits of the rights ID = title ID)
    m = re.search(r'\[([0-9A-Fa-f]{16})\]', path.name)
    if m: return int(m.group(1), 16)
    for n in pfs0_names(path):
        if re.match(r'[0-9A-Fa-f]{32}\.tik$', n): return int(n[:16], 16)
    return None


def version_of(path):
    m = re.search(r'\[v(\d+)\]', path.name)
    return int(m.group(1)) if m else None


def scan_nsps(nsp_dir):
    """按游戏归组：{应用标题号: {'base': 路径, 'updates': [(版本, 路径)], 'dlc': [(标题号, 版本, 路径)]}}
    Group by game: {application title ID: {'base': path, 'updates': [(version, path)], 'dlc': [(title ID, version, path)]}}"""
    fams, unknown = {}, []
    for p in sorted(nsp_dir.glob('*.nsp')):
        tid = title_of(p)
        if tid is None: unknown.append(p); continue
        low = tid & 0xFFF   # 基础版 …000，更新 …800，DLC = 基础版 + 0x1000 + 序号 | base …000, update …800, DLC = base + 0x1000 + index
        app = tid if low == 0 else tid & ~0xFFF if low == 0x800 else (tid & ~0xFFF) - 0x1000
        f = fams.setdefault(app, {'base': None, 'updates': [], 'dlc': []})
        if low == 0: f['base'] = p
        elif low == 0x800: f['updates'].append((version_of(p), p))
        else: f['dlc'].append((tid, version_of(p), p))
    if len(fams) == 1 and len(unknown) == 1 and next(iter(fams.values()))['base'] is None:   # 卡带转出的基础版可能既无标签也无票据；构建号随后核对 | a base game converted from a cartridge may have neither tags nor a ticket; build IDs are checked afterwards
        next(iter(fams.values()))['base'] = unknown[0]
    if not fams: raise BuildError(f'{nsp_dir} 里没有认得出标题号的 NSP | no NSP with a readable title ID in {nsp_dir}')
    if len(fams) > 1: raise BuildError('文件夹里有不止一个游戏，一个文件夹放一个游戏 | more than one game in the folder, keep one game per folder: ' + ', '.join(f'{t:016X}' for t in fams))
    app, fam = next(iter(fams.items()))
    if fam['base'] is None: raise BuildError(f'{nsp_dir} 里没找到基础版 NSP（标题号 {app:016X}）| no base game NSP found (title ID {app:016X})')
    return app, fam


def pick_nsps(fam, versions):
    """每个要的版本对应的（基础版, 更新或 None）：v0 = 只基础版，v<N> = 基础版 + 版本号 N 的更新
    For each wanted version, (base game, update or None): v0 = the base game alone, v<N> = the base game + the update with version number N"""
    for tid, _, p in fam['dlc']: say(f'跳过 DLC（本移植不读追加内容）| skipping DLC (this port does not load add-on content): {p.name}')
    say(f'基础版 | base: {fam["base"].name}')
    out = {}
    for ver in versions:
        if ver == 'v0': out[ver] = (fam['base'], None); continue
        n = int(ver[1:])
        upd = [u for v, u in fam['updates'] if v == n] or [u for v, u in fam['updates'] if v is None and len(fam['updates']) == 1]   # 文件名没写版本号时只认唯一的那个，构建号随后核对 | without a version in the file name only a single update is accepted; build IDs are checked afterwards
        if not upd: raise BuildError(f'没找到更新 {ver} 的 NSP（游戏档案要这些版本：{", ".join(versions)}）| no NSP for update {ver} (the game profile needs these versions: {", ".join(versions)})')
        say(f'更新 {ver} | update {ver}: {upd[0].name}'); out[ver] = (fam['base'], upd[0])
    return out


def nacp_name(d):   # 解包目录里 control.nacp 的游戏名（nx_extract 存的；旧的解包没有则空；解不出像样的名字也空） | the game name from control.nacp in an extraction folder (saved by nx_extract; empty for older extractions, or when no sensible name decodes)
    p = d / 'control.nacp'
    if not p.exists(): return ''
    b = p.read_bytes(); t = b[:0x3000]
    try:   # 新 SDK 把名称区压缩：u16 长度 + raw deflate，解开是 32 种语言 × 0x300（马力欧 1.2.1 就是） | newer SDKs compress the title area: a u16 size + raw deflate, inflating to 32 languages × 0x300 (Mario 1.2.1 does)
        z = zlib.decompress(b[2:2 + struct.unpack_from('<H', b)[0]], -15)
        if len(z) == 0x6000: t = z
    except zlib.error: pass   # 未压缩 | not compressed
    for i in range(len(t) // 0x300):   # 每种语言 0x300 字节，名称占前 0x200；取第一个非空的（0 = 美式英语） | 0x300 bytes per language, the name in the first 0x200; take the first non-empty one (0 = American English)
        n = t[i * 0x300:i * 0x300 + 0x200].split(b'\0', 1)[0]
        if not n: continue
        try: n = n.decode('utf-8')
        except UnicodeDecodeError: return ''
        return n if n.isprintable() else ''
    return ''


def can_clone(d):   # 该卷能否做 APFS 克隆（nx_extract 用它省空间，不支持时整份写出） | whether this volume supports APFS clones (nx_extract uses them to save space; without them it writes full copies)
    a, b = d / '.clone_test', d / '.clone_test2'
    try:
        a.write_bytes(b'x'); rmtree(b)
        return ctypes.CDLL(None, use_errno=True).clonefile(str(a).encode(), str(b).encode(), 0) == 0
    finally:
        rmtree(a); rmtree(b)


# ---- 新游戏粗查（没有对应游戏档案时） | Survey of a new game (when no game profile matches) ----
MAGICS = [(b'UnityFS\0', 'UnityFS'), (b'UnityWeb', 'UnityWeb'), (b'UnityRaw', 'UnityRaw'), (b'BNSH', 'BNSH'), (b'BAHS', 'BAHS'), (b'SARC', 'SARC'), (b'Yaz0', 'Yaz0'),
          (bytes.fromhex('28b52ffd'), 'zstd'), (b'FRES', 'bfres'), (b'BNTX', 'BNTX'), (b'BWAV', 'BWAV'), (b'FSAR', 'BFSAR'), (b'CPK ', 'CRI CPK'), (b'CRID', 'CRI USM'),
          (b'@UTF', 'CRI UTF'), (b'AKPK', 'Wwise pck'), (b'BKHD', 'Wwise bnk'), (b'FSB5', 'FMOD'), (b'\x89PNG', 'PNG'), (b'RIFF', 'RIFF'), (b'OggS', 'Ogg'), (b'PK\x03\x04', 'zip'), (b'NSO0', 'NSO')]
MARKERS = [(b'nvnBootstrapLoader', 'NVN'), (b'vkCreateInstance', 'Vulkan'), (b'eglGetProcAddress', 'EGL / OpenGL'), (b'il2cpp_', 'Unity IL2CPP'), (b'UnityEngine', 'Unity'),
           (b'sead::', 'Nintendo sead'), (b'agl::', 'Nintendo agl'), (b'gsys::', 'Nintendo gsys'), (b'Unreal', 'Unreal'), (b'UE4', 'Unreal 4'), (b'Havok', 'Havok'), (b'CRIWARE', 'CRIWARE'), (b'Wwise', 'Wwise')]


def nso_image(path):   # 解出 NSO 的三段 | decompress an NSO's three segments
    import lz4.block
    d = path.read_bytes(); flags = struct.unpack_from('<I', d, 0xc)[0]
    segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]; csz = struct.unpack_from('<III', d, 0x60)
    img = bytearray(segs[2][1] + segs[2][2])
    for i, (fo, mo, sz) in enumerate(segs):
        raw = d[fo:fo + csz[i]]
        img[mo:mo + sz] = lz4.block.decompress(raw, uncompressed_size=sz) if flags >> i & 1 else raw
    return bytes(img), d[0x40:0x50].hex(), [sz for _, _, sz in segs]


def survey(nsp_dir, app, fam, keys, repo, py, env):
    work = nsp_dir / f'{app:016X}_survey'; logs, ex = work / 'logs', work / 'extract'
    say(f'没有 {app:016X} 的游戏档案：只做粗查，不出 IPA | no game profile for {app:016X}: survey only, no IPA')
    nx = work / 'tools/nx_extract/nx_extract'
    run(['dotnet', 'build', repo / 'dm/tools/nx_extract', '-c', 'Release', '-o', nx.parent, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
    ups = sorted(fam['updates'], key=lambda u: u[0] or 0)
    upd = ups[-1][1] if ups else None
    tmp = work / 'extract.partial'; rmtree(tmp)
    say(f'解出代码模块与 RomFS 清单（不写游戏文件）| extracting the code modules and a RomFS manifest (no game files written) → {ex}')
    run([nx, keys, fam['base'], upd or '-', tmp, '--manifest'], logs / 'extract.log')
    swap_in(tmp, ex)
    xlog = (logs / 'extract.log').read_text(errors='replace')
    dv = re.search(r'display version (\S+)', xlog)
    R = [f'# 粗查 | Survey: {nacp_name(ex) or "?"} ({app:016X})', '', f'- 显示版本 | display version: {dv.group(1) if dv else "?"}', f'- 基础版 | base: `{fam["base"].name}` ({fam["base"].stat().st_size / 2**30:.2f} GB)']
    R += [f'- 更新 | update: `{p.name}` (v{v}, {p.stat().st_size / 2**30:.2f} GB){"（粗查用的这一个 | used for this survey）" if p == upd else ""}' for v, p in ups]
    R += [f'- DLC: `{p.name}` ({p.stat().st_size / 1024:.0f} KB{"，只有授权、无内容 | licence only, no content" if p.stat().st_size < 1 << 20 else ""})' for _, _, p in fam['dlc']]

    # 代码模块 | Code modules
    R += ['', '## 代码模块 | Code modules', '', '| 模块 module | 构建号 build ID | text / ro / data (MB) |', '|---|---|---|']
    found, sdkver, comps, unity, mods = {}, set(), set(), set(), []
    for p in sorted((ex / 'ExeFS').iterdir()):
        if p.read_bytes()[:4] != b'NSO0': continue
        img, bid, szs = nso_image(p); mods.append(p)
        R.append(f'| {p.name} | `{bid}` | ' + ' / '.join(f'{z / 2**20:.1f}' for z in szs) + ' |')
        sdkver |= {m.decode() for m in re.findall(rb'(?i)NintendoSDK_nnSdk-([0-9_]+)', img)}   # SDK 12 写作 NintendoSdk_，16 写作 NintendoSDK_ | SDK 12 spells it NintendoSdk_, SDK 16 NintendoSDK_
        comps |= {m.decode() for m in re.findall(rb'SDK MW\+Nintendo\+([\w]+-[0-9_]+)', img)}
        unity |= {m.decode() for m in re.findall(rb'20[12][0-9]\.[0-9]+\.[0-9]+f[0-9]', img)} if p.name == 'main' else set()
        for pat, name in MARKERS:
            if pat in img: found.setdefault(name, []).append(p.name)
    npdm = ex / 'ExeFS/main.npdm'
    if npdm.exists():
        d = npdm.read_bytes(); a = d.find(b'ACI0')
        if a >= 0: R.append(f'\n- main.npdm 程序号 | program ID: `{struct.unpack_from("<Q", d, a + 0x10)[0]:016X}`')
    R.append(f'- SDK: {", ".join(sorted(sdkver)) or "?"}（马力欧 | Mario: 16_2_0 / 16_2_12；D2R ≈ 12）')
    R.append('- SDK 组件 | SDK components: ' + (', '.join(sorted(comps)) or '—'))
    if unity: R.append('- Unity: ' + ', '.join(sorted(unity)))
    R.append('- 引擎与接口标记 | engine and API markers: ' + ('; '.join(f'{k} ({", ".join(v)})' for k, v in found.items()) or '—'))
    extra = [m.name for m in mods if m.name not in ('main', 'sdk', 'rtld')]

    # RomFS 清单 | RomFS manifest
    tops, exts, mags, nfile, nbytes = collections.Counter(), collections.Counter(), collections.Counter(), 0, 0
    topb, magb = collections.Counter(), collections.Counter()
    for line in (ex / 'romfs_manifest.tsv').read_text(errors='replace').splitlines():
        if line.startswith('#'): continue
        path, size, head = line.split('\t'); size = int(size); h = bytes.fromhex(head)
        nfile += 1; nbytes += size
        parts = path.strip('/').split('/')
        top = '/'.join(parts[:2]) if len(parts) > 2 else parts[0] if len(parts) > 1 else '(根 | root)'   # 两层：Unity 全在 Data/ 下 | two levels: Unity keeps everything under Data/
        tops[top] += 1; topb[top] += size
        exts[os.path.splitext(path)[1].lower() or '(无 | none)'] += 1
        mag = next((n for m, n in MAGICS if h.startswith(m)), None)
        if mag: mags[mag] += 1; magb[mag] += size
    R += ['', f'## RomFS: {nfile} 个文件 | files, {nbytes / 2**30:.2f} GB', '', '| 顶层 top level | 文件 files | GB |', '|---|---|---|']
    R += [f'| {t} | {n} | {topb[t] / 2**30:.2f} |' for t, n in tops.most_common(20)]
    R += ['', '| 文件头 header magic | 文件 files | GB |', '|---|---|---|'] + [f'| {m} | {n} | {magb[m] / 2**30:.2f} |' for m, n in mags.most_common()]
    R += ['', '扩展名 | extensions: ' + ', '.join(f'`{e}` {n}' for e, n in exts.most_common(20))]

    # 改写点统计与 SDK 接口 | Rewrite-site census and SDK surface
    R += ['', '## 改写点统计 | Rewrite-site census (insn_census.py)', '', '```']
    for m in mods:
        if m.name == 'rtld': continue   # rtld 由宿主代替 | rtld is replaced by the host
        say(f'统计改写点 | counting rewrite sites: {m.name}')
        run([py, repo / 'dm/tools/insn_census.py', m], logs / f'census_{m.name}.log', env=env, quiet=True)
        R += (logs / f'census_{m.name}.log').read_text(errors='replace').rstrip().splitlines()
    R += ['```', '', '## 导入与宿主钩子 | Imports vs host hooks (nso_syms.py)', '', '```']
    run([py, repo / 'dm/tools/nso_syms.py', repo / 'dm/host', '-', ex / 'ExeFS'], logs / 'nso_syms.log', env=env, quiet=True)
    R += (logs / 'nso_syms.log').read_text(errors='replace').rstrip().splitlines() + ['```']

    # 对移植意味着什么 | What it means for a port
    notes = []
    if 'NVN' not in found: notes.append('没找到 NVN：宿主只实现了 NVN | no NVN found: the host implements only NVN')
    if extra: notes.append(f'除 main / sdk 外还有 {", ".join(extra)}：装载器能装最多 8 个模块，但打包与改写目前只处理 main + sdk | modules besides main / sdk: {", ".join(extra)}; the loader takes up to 8 modules, but packaging and rewriting handle only main + sdk today')
    if mags['UnityFS'] or 'Unity IL2CPP' in found: notes.append('Unity：着色器是 UnityFS 包与 data.unity3d 里的 Shader 对象，shader_split.py 经 unityfs.py 读出（按 Unity 2020.3 的 Switch 格式写成，别的 Unity 版本先核对） | Unity: the shaders are Shader objects in UnityFS bundles and data.unity3d, which shader_split.py reads through unityfs.py (written for Unity 2020.3\'s Switch format; check other Unity versions first)')
    if not (mags['BNSH'] or mags['BAHS']) and not mags['UnityFS']: notes.append('顶层没有 BNSH / BAHS：着色器在压缩包或别的容器里，要先找到 | no BNSH / BAHS at the top level: the shaders are inside compressed or other containers and must be located first')
    major = {v.split('_')[0] for v in sdkver}
    if not major: notes.append('没找到 SDK 版本串 | no SDK version string found')
    elif major != {'16'}: notes.append(f'SDK {"/".join(sorted(major))}，不是马力欧的 16：NVN 对象尺寸、API 版本与服务布局按 SDK 不同（宿主已从 D2R 的 SDK 12 改成 16 的尺寸），须逐项核对 | SDK {"/".join(sorted(major))}, not Mario\'s 16: NVN object sizes, API version and service layouts differ by SDK (the host was converted from D2R\'s SDK 12 to SDK 16 sizes); verify each against this build')
    if any(l.split()[:1] == ['nn::ro'] for l in R): notes.append('导入 nn::ro（运行时装 NRO 模块）：宿主没有 | imports nn::ro (loads NRO modules at run time): not in the host')
    if fam['dlc']: notes.append('有 DLC：宿主还没有 MountAddOnContent | DLC present: the host has no MountAddOnContent yet')
    R += ['', '## 对移植意味着什么 | What it means for a port', ''] + [f'- {n}' for n in notes or ['—']]
    R += ['', '下一步按 dm/games/README.md 添加档案，再核对模块装载、服务初始化和图形输出。 | Next: add a profile following dm/games/README.md, then verify module loading, service initialization and graphics output.']
    (work / 'survey.md').write_text('\n'.join(R) + '\n')
    for n in notes: say(n)
    say(f'粗查报告 | survey report: {work / "survey.md"}')


# ---- 各步 | Steps ----
def check_prereqs(env, xlate):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools') if not _importable(m)]
    if missing: raise BuildError(f'当前 Python（{sys.executable}）缺模块 | the current Python is missing modules: {", ".join(missing)}（pip install lz4 capstone fonttools）')
    tools = {'xcodebuild': 'Xcode', 'xcodegen': 'brew install xcodegen', 'dotnet': '.NET SDK 10', 'zstd': 'brew install zstd', 'spirv-cross': 'brew install spirv-cross', 'zip': 'zip', 'make': 'Xcode'}
    gone = [f'{t}（{how}）' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('找不到工具 | tools not found: ' + '，'.join(gone))
    if not xlate.exists(): raise BuildError(f'仓内没有着色器翻译器 | the shader translator is missing from the repository: {xlate}')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def extract(nx, keys, ex, name, base, upd, cmp_dir, logs, g):
    dst, marker = ex / name, g.get('romfs_marker')
    if (dst / 'ExeFS' / 'main').exists() and (dst / 'RomFS' / marker).exists():
        say(f'沿用已解出的 {name} | reusing the extracted {name}: {dst}')
    else:
        tmp = ex / (name + '.partial'); rmtree(tmp)
        say(f'解包 {name} | extracting {name} → {dst}')
        run([nx, keys, base, upd or '-', tmp] + ([cmp_dir] if cmp_dir else []), logs / f'extract_{name}.log')
        check_build(tmp, name, g); swap_in(tmp, dst)
    check_build(dst, name, g)
    return dst


def check_build(d, name, g):   # 每个模块的构建号要是档案 build 行登记的 | every module's build ID must be the one registered in the profile's build row
    want = g.builds()[name]
    for m, bid in want.items():
        if m == 'display': continue
        got = build_id(d / 'ExeFS' / m)
        if got != bid: raise BuildError(f'{m} 构建号 {got} 不是游戏档案 {g.id} 登记的 {name}（{bid}）| {m} build ID {got} is not the {name} registered in game profile {g.id} ({bid})')


def rewrite(py, repo, exe_dir, out, env, logs, g):
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True)
    for m in g.words('modules'):
        say(f'改写 {m} | rewriting {m}')
        run([py, repo / 'dm/tools/patch_nso.py', exe_dir / m, tmp, m], logs / f'rewrite_{m}.log', env=dict(env, DM_RULESET='native'))
    swap_in(tmp, out)


def split_shaders(py, repo, roms, mains, pkg, env, logs, g):
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    say('拆出着色器 | splitting out the shaders')
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names], logs / 'shader_split.log', env=env)
    # 表与档案里的逐字节相同 = 游戏文件与建表用的是同一份；不同就别往下出包（档案里的表不改） | Tables byte-identical to the profile's = the same game files the tables were built from; if not, stop (the profile's tables are left alone)
    diff = [n for n in names if (tmp / 'tables' / n).read_bytes() != g.table(n).read_bytes()]
    if diff: raise BuildError(f'拆出的着色器表与游戏档案 {g.id} 的不同 | the split shader tables differ from game profile {g.id}\'s: {", ".join(diff)}（游戏文件不是档案登记的版本？| game files not the versions the profile registers?）')
    swap_in(tmp / 'pkg', pkg); rmtree(tmp)


def build_shaders(py, repo, sh, xlate, env, logs, g):
    say('出 iOS 着色器库（马力欧首次约 30 分钟，之后走缓存约 7 分钟）| building the iOS shader libraries (Mario: about 30 minutes the first time, about 7 with the cache)')
    ios = sh / 'ios'
    run([py, repo / 'dm/gfx/tools/dmsh_build.py', sh / 'pkg', sh / 'cache'], logs / 'dmsh_build.log', env=dict(env, XLATE=str(xlate), IOSOUT=str(ios)), quiet=True)
    chain = re.search(r'#define DMSH_CHAIN "(.*)"', g.table('dmsh_chain.h').read_text()).group(1)
    libs = ios / f'b-{chain}'
    n = sum(1 for _ in libs.glob('*_b.metallib')) if libs.is_dir() else 0
    if n == 0: raise BuildError(f'没出 iOS 着色器库 | no iOS shader libraries in {libs}')
    say(f'iOS 着色器库 {n} 个 | {n} iOS shader libraries ({libs})')
    return ios


def main():
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    repo = here.parents[3]   # dm/ios/tools/本文件 | dm/ios/tools/this file
    if not (repo / 'dm/tools/nx_extract/Program.cs').exists(): raise BuildError(f'从 {here} 推不出仓库根目录 | cannot find the repository root from {here}')
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod   # 游戏档案 | game profiles
    nsp_dir = Path(os.path.expanduser(NSP_DIR) or ask('NSP 文件夹 | NSP folder: ')).resolve()
    keys = Path(os.path.expanduser(PROD_KEYS) or ask('prod.keys 路径 | path of prod.keys: ')).resolve()
    if not nsp_dir.is_dir(): raise BuildError(f'不是文件夹 | not a folder: {nsp_dir}')
    if not keys.is_file(): raise BuildError(f'没有这个文件 | no such file: {keys}')
    env = dict(os.environ, PATH=os.pathsep.join(TOOL_PATHS + [os.environ.get('PATH', '/usr/bin:/bin:/usr/sbin:/sbin')]))
    env.setdefault('USER', getpass.getuser())   # xcodegen 没有 USER 就失败（Couldn't find current username） | xcodegen fails without USER (Couldn't find current username)
    py = sys.executable
    app, fam = scan_nsps(nsp_dir)
    g = gamemod.by_title(app)
    if g is None:   # 没有游戏档案：只粗查 | no game profile: survey only
        missing = [m for m in ('lz4.block', 'capstone') if not _importable(m)]
        if missing: raise BuildError(f'当前 Python（{sys.executable}）缺模块 | the current Python is missing modules: {", ".join(missing)}')
        if not shutil.which('dotnet', path=env['PATH']): raise BuildError('找不到 dotnet（.NET SDK 10）| dotnet not found (.NET SDK 10)')
        return survey(nsp_dir, app, fam, keys, repo, py, env)
    say(f'游戏档案 | game profile: {g.id}')
    builds, need = g.builds(), g.words('shader_versions')
    pkg_ver = PACKAGE or g.get('package')
    if pkg_ver not in builds: raise BuildError(f"PACKAGE 只能是游戏档案登记的版本 | PACKAGE must be a version the game profile registers: {', '.join(builds)}")
    versions = need + ([pkg_ver] if pkg_ver not in need else [])
    out_ipa = Path(os.path.expanduser(OUT_IPA or f'~/Desktop/{g.get("ipa")}'))
    work = nsp_dir / f'{g.id}_build'
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_GAME 选档案；DM_ROOT / WORK 指到 <名>_build：各工具的缺省路径与临时目录都不出它 | DM_GAME picks the profile; DM_ROOT / WORK point at <id>_build: the tools' default paths and temporary directories all stay inside it
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate)
    picks = pick_nsps(fam, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = sum({p.stat().st_size for b, u in picks.values() for p in (b, u) if p}) / 2**30 + 6   # 解包 + 着色器缓存 + 余量 | extraction + shader cache + slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'{work} 所在的卷剩余不足 {gb:.0f} GB | less than {gb:.0f} GB free on the volume of {work}')
    nx = work / 'tools/nx_extract/nx_extract'
    say('编译 nx_extract | building nx_extract')
    run(['dotnet', 'build', repo / 'dm/tools/nx_extract', '-c', 'Release', '-o', nx.parent, '-v', 'quiet', '-nologo'], logs / 'nx_extract_build.log', env=env)
    dirs, clone = {}, None
    for ver in versions:   # 基础版先单独解，后面的更新拿它的 RomFS 做克隆比对 | the base game is extracted alone first; later updates clone against its RomFS
        b, u = picks[ver]
        dirs[ver] = extract(nx, keys, ex, ver, b, u, clone, logs, g)
        if u is None and clone is None and can_clone(ex): clone = dirs[ver] / 'RomFS'
    game = dirs[pkg_ver]
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])   # 基础版单独解出的目录 | the folder the base game was extracted to alone
    app_name = APP_NAME or nacp_name(base) or g.get('app_name')   # 设置 > 基础版 NSP 里的游戏名 > 档案 | setting > game name in the base NSP > profile
    say(f'App 名 | app name: {app_name}')

    patched = work / f'patched_{pkg_ver}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs, g)
    sh = work / 'shader'
    split_shaders(py, repo, [dirs[v] / 'RomFS' for v in need], [dirs[v] / 'ExeFS/main' for v in need], sh / 'pkg', env, logs, g)
    ios = build_shaders(py, repo, sh, xlate, env, logs, g)

    if free_gb(out_ipa.parent) < 5: raise BuildError(f'{out_ipa.parent} 所在的卷剩余不足 5 GB | less than 5 GB free on the volume of {out_ipa.parent}')
    say(f'出 IPA | building the IPA → {out_ipa}')
    ipa_env = dict(env, GAME=g.id, W=str(work / 'ipa'), PATCHED=str(patched), ROMFS=str(game / 'RomFS'), MSL_IOS=str(ios), FONT_PY=py, DM_DISPLAY_NAME=app_name)
    if DM_ENV_FILE: ipa_env['DM_ENV_FILE'] = os.path.expanduser(DM_ENV_FILE)
    run(['bash', repo / 'dm/ios/tools/build_ipa.sh', out_ipa], logs / 'build_ipa.log', env=ipa_env)
    if not out_ipa.is_file(): raise BuildError(f'没出 IPA | no IPA at {out_ipa}')
    rmtree(work / 'tmp')
    if not KEEP_EXTRACTED:
        say(f'删掉解出的游戏文件 | deleting the extracted game files: {ex}'); rmtree(ex)
    say(f'完成 | done: {out_ipa}（{out_ipa.stat().st_size / 2**30:.1f} GB）')


if __name__ == '__main__':
    try:
        main()
    except BuildError as e:
        print(f'【红】{e}', flush=True)
    except KeyboardInterrupt:
        print('【红】中断 | interrupted', flush=True)
