// 较新 SDK（16.x）游戏用到、d2r-mobile 那一作没用到的系统服务接口：oe / time / pl（共享字体）/ vi 垂直同步 / diag。
// 真机上都经 Horizon 系统服务（IPC）；宿主直接给值或做成「不可用」。
// System service interfaces used by games on newer SDKs (16.x) that the d2r-mobile title did not use: oe / time / pl (shared font) / vi vsync / diag.
// On the real device all of these go through Horizon system services (IPC); the host gives values directly or makes them "unavailable".
#include "hle.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------- oe ----------
HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "CPU 加速档；宿主不调频", void, h_oe_CpuBoost, int m) { (void)m; }
// DisplayVersion = char[16]（应用的显示版本号）：按 main 的构建号取该版控制数据（NACP）里的值（游戏档案的 build 行），认不出的构建给 1.0.0
// DisplayVersion = char[16] (the application's display version): the value from that version's control data (NACP), chosen by main's build ID (the game profile's build rows); unknown builds get 1.0.0
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "显示版本号（按 main 构建号）", void, h_oe_DispVer, char *v) {
    static const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
    const char *ver = "1.0.0";
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (unsigned j = 0; j < sizeof k / sizeof *k; j++) if (!strcmp(g_mod[i].build_id, k[j].build)) ver = k[j].ver;
    memset(v, 0, 16); strlcpy(v, ver, 16);
}
HLE_FORCE("_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_", "系统 / 应用音量平衡；宿主恒 1:1", void, h_oe_GetVolBal, float *sys, float *app) { *sys = 1.0f; *app = 1.0f; }
HLE_FORCE("_ZN2nn2oe24SetExpectedVolumeBalanceEff", "音量平衡；宿主忽略", void, h_oe_SetVolBal, float sys, float app) { (void)sys; (void)app; }
// 程序累计运行时长（nn::TimeSpan，纳秒）：取宿主启动以来的单调时间
// Program total active time (nn::TimeSpan, nanoseconds): the host's monotonic time since startup
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "累计运行时长 → 宿主启动以来", int64_t, h_oe_ActiveTime, void) { return (int64_t)(now_ns() - g_t0_ns); }
HLE_FORCE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", "退出请求临界区；宿主的退出走 SIGTERM / 生命周期", void, h_oe_EnterExit, void) {}
HLE_FORCE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", "退出请求临界区；宿主忽略", void, h_oe_LeaveExit, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "自动休眠延长；宿主忽略", void, h_oe_Inactivity, unsigned on) { (void)on; }

// ---------- time ----------
// CalendarTime = { int16 年; int8 月, 日, 时, 分, 秒 }，按值返回（8 字节内，走 x0）
// CalendarTime = { int16 year; int8 month, day, hour, minute, second }, returned by value (within 8 bytes, in x0)
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "绝对秒 → UTC 日历（宿主 gmtime_r）", uint64_t, h_time_ToCalUtc, const int64_t *t) {
    time_t v = (time_t)*t; struct tm tm; gmtime_r(&v, &tm);
    return (uint64_t)(uint16_t)(tm.tm_year + 1900) | (uint64_t)(uint8_t)(tm.tm_mon + 1) << 16 | (uint64_t)(uint8_t)tm.tm_mday << 24 |
           (uint64_t)(uint8_t)tm.tm_hour << 32 | (uint64_t)(uint8_t)tm.tm_min << 40 | (uint64_t)(uint8_t)tm.tm_sec << 48;
}
// LocationName = char[0x24]（时区名）：跟宿主系统时区
// LocationName = char[0x24] (time zone name): follows the host's system time zone
HLE_FORCE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", "设备时区名 → 宿主时区", Result, h_time_LocName, char *out) {
    memset(out, 0, 0x24); tzset(); strlcpy(out, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24); return RES_OK;
}
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "网络时钟（离线，不可用）", Result, h_time_NetNow, int64_t *out) { (void)out; return RES_FAIL; }
HLE_FORCE("_ZN2nn4time8FinalizeEv", "与 Initialize 配对；宿主无会话", Result, h_time_Finalize, void) { return RES_OK; }
// 「确保网络时钟可用」异步任务：不发请求，完成事件立即触发、结果失败。对象本身不动（构造 / 析构空操作），事件放宿主静态区
// "Ensure network clock availability" async task: no request is sent; the finish event fires immediately and the result is failure. The object itself is untouched (constructor / destructor no-ops), the event lives in host static storage
static _Alignas(16) uint8_t g_ev_netclock[0x100];
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "网络时钟任务构造；宿主不建会话", void, h_time_NcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "网络时钟任务析构；无会话可关", void, h_time_NcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "网络时钟任务初始化（离线）", Result, h_time_NcInit, void *self, int mode) { (void)self; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "网络时钟任务开跑：立即以失败结束", Result, h_time_NcStart, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "网络时钟任务完成事件：已触发", void *, h_time_NcEvent, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return g_ev_netclock; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "网络时钟任务结果：离线，失败", Result, h_time_NcResult, void *self) { (void)self; return RES_FAIL; }

// ---------- pl：共享字体 ----------
// 系统共享字体（0 标准 / 1 简中 / 2 简中扩展 / 3 繁中 / 4 韩文 / 5 任天堂扩展）宿主没有原版，用替身：
//   任天堂扩展 = DM_SHARED_FONT_EXT 或 $DM_FONT_DIR/nintendo_ext.ttf（dm/tools/make_ext_font.py 生成：按键图标在原版私用区码位，如 U+E0E0 = A 键）；
//   其余类型共用一份 = DM_SHARED_FONT 或 $DM_FONT_DIR/standard.ttf，再缺省 macOS 的 Arial Unicode（iOS 没有这个路径，App 里自带 dm/font/）。
//   扩展字体打不开时退回标准字体（按键图标画成「?」，其余照常）。
// 游戏自己的文字用 RomFS 里的字体，共享字体只在系统名字、按键图标等少数地方用。
// 格式：原版共享内存里是 BFTTF——8 字节头 + 按字异或混淆的 TTF；游戏拿到地址后先调 nn::fontll::ScalableFontEngineHelper::Decode(地址, 大小)
//   （SMBW sdk +0x3093c0）就地解混淆并返回地址 + 8。Decode 先看头一个字：已是「解过」标记（小端 0x18029a7f）就直接返回地址 + 8、不动数据。
//   所以宿主给「解过」标记 + 原样 TTF；第二个字照原版写混淆后的长度（键 0x49621806：存的是 bswap(长度 ^ 键)）。给裸 TTF 会被当密文异或成乱码，
//   字体引擎随后用野指针崩（SMBW main +0x107dbc0）。
// The host has none of the original system shared fonts (0 standard / 1 Simplified Chinese / 2 Simplified Chinese extension / 3 Traditional Chinese / 4 Korean / 5 Nintendo extension) and uses stand-ins:
//   Nintendo extension = DM_SHARED_FONT_EXT or $DM_FONT_DIR/nintendo_ext.ttf (generated by dm/tools/make_ext_font.py: button icons at the original private-use code points, e.g. U+E0E0 = A button);
//   all other types share one = DM_SHARED_FONT or $DM_FONT_DIR/standard.ttf, then macOS's Arial Unicode (that path does not exist on iOS; the App ships dm/font/).
//   If the extension font cannot be opened it falls back to the standard font (button icons draw as "?", everything else works).
// The game's own text uses the fonts in its RomFS; shared fonts are only used in a few places such as system names and button icons.
// Format: the original shared memory holds BFTTF - an 8-byte header + a TTF obfuscated by per-word XOR; after getting the address the game calls nn::fontll::ScalableFontEngineHelper::Decode(address, size)
//   (SMBW sdk +0x3093c0), which deobfuscates in place and returns address + 8. Decode first checks the first word: if it is already the "decoded" marker (little-endian 0x18029a7f) it returns address + 8 without touching the data.
//   So the host provides the "decoded" marker + the TTF as is; the second word holds the obfuscated length as in the original (key 0x49621806: stored as bswap(length ^ key)). A raw TTF would be XORed into garbage as if it were ciphertext,
//   and the font engine then crashes on a wild pointer (SMBW main +0x107dbc0).
typedef struct { uint8_t *p; size_t n; } font_t;
static font_t font_map(const char *p) {
    font_t f = {0}; int fd = open(p, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return f; }
    size_t n = (size_t)st.st_size;
    uint8_t *m = mmap(NULL, n + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) { LOG("【红】共享字体内存分配失败（%zu 字节）", n + 8); close(fd); return f; }
    size_t got = 0; while (got < n) { ssize_t r = pread(fd, m + 8 + got, n - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) { LOG("【红】共享字体读不全：%s（%zu / %zu 字节）", p, got, n); munmap(m, n + 8); return f; }
    uint32_t done = 0x18029a7fu, len = __builtin_bswap32((uint32_t)n ^ 0x49621806u);
    memcpy(m, &done, 4); memcpy(m + 4, &len, 4);
    f.p = m; f.n = n + 8; return f;
}
static font_t font_first(const char *env, const char *file, const char *fallback) {
    char buf[1024]; const char *dir = getenv("DM_FONT_DIR"), *c[3] = {getenv(env), NULL, fallback};
    if (dir && *dir) { snprintf(buf, sizeof buf, "%s/%s", dir, file); c[1] = buf; }
    for (int i = 0; i < 3; i++) {
        if (!c[i] || !*c[i]) continue;
        font_t f = font_map(c[i]); if (f.p) { LOG("共享字体（%s）：%s（%zu 字节，BFTTF「已解」头 + 原样 TTF）", file, c[i], f.n - 8); return f; }
    }
    return (font_t){0};
}
static font_t font_get(int type) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static font_t std_f, ext_f; static int std_done, ext_done;
    pthread_mutex_lock(&mu);
    if (!std_done) { std_done = 1; std_f = font_first("DM_SHARED_FONT", "standard.ttf", "/System/Library/Fonts/Supplemental/Arial Unicode.ttf");
                     if (!std_f.p) LOG("【红】标准共享字体打不开（DM_SHARED_FONT 或 DM_FONT_DIR/standard.ttf 指一个 TTF）"); }
    if (type == 5 && !ext_done) { ext_done = 1; ext_f = font_first("DM_SHARED_FONT_EXT", "nintendo_ext.ttf", NULL);
                                  if (!ext_f.p) LOG("【黄】任天堂扩展共享字体缺（dm/tools/make_ext_font.py 生成 nintendo_ext.ttf）：按键图标会画成「?」"); }
    font_t f = type == 5 && ext_f.p ? ext_f : std_f;
    pthread_mutex_unlock(&mu);
    return f;
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "共享字体装载状态：已装（1）", int, h_pl_LoadState, int type) { return font_get(type).p ? 1 : 0; }
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "共享字体地址 → BFTTF 头（宿主替身字体）", const void *, h_pl_Addr, int type) { return font_get(type).p; }
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "共享字体大小 → 头 8 字节 + 替身字体文件大小", size_t, h_pl_Size, int type) { return font_get(type).n; }
// 是否跑在 Switch 2（代号 Ounce）上（返回 bool）：宿主按 Switch 1 报假。马力欧 1.2.1 据此决定要不要数追加内容（升级包）、提示升级到 Switch 2 版
// Whether running on a Switch 2 (codename Ounce) (returns bool): the host reports false, as a Switch 1. Mario 1.2.1 uses it to decide whether to count add-on content (the upgrade pack) and suggest the Switch 2 Edition
HLE_FORCE("_ZN2nn2pl16IsRunningOnOunceEv", "是否 Switch 2：否", unsigned, h_pl_IsOunce, void) { return 0; }

// ---------- ec：商店小程序（升级到 Switch 2 版的页面）----------
// 商店属排除范围：空操作。游戏不看返回值（两个重载都按无返回值处理）
// ---------- ec: shop applet (the upgrade-to-Switch-2-Edition page) ----------
// The shop is out of scope: no-op. The game does not look at the return value (both overloads are handled as returning nothing)
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionE", "商店升级页，排除范围；空操作", void, h_ec_ShowUpg, int ed) { LOG("ec::ShowShopApplicationInformationForUpgrade(%d)（剪）", ed); }
HLE_FORCE("_ZN2nn2ec40ShowShopApplicationInformationForUpgradeENS0_14UpgradeEditionERKNS_7account10UserHandleE", "商店升级页（指定用户），排除范围；空操作", void, h_ec_ShowUpgU, int ed, const void *user) { (void)user; LOG("ec::ShowShopApplicationInformationForUpgrade(%d, 用户)（剪）", ed); }

// ---------- nfp：amiibo ----------
// 马力欧 1.2.1 起启动即初始化 nfp（v0 不用）。真 SDK 要连 sm: 服务，宿主没有 → 中止，所以这一族全部接管。
// 宿主没有 NFC 读卡器：报一台 NFC 设备，挂在当前接着的手柄上（同 hle_hid.c），但找标签永远找不到——与真机手边没有 amiibo 时一样：
// 游戏的「请碰 amiibo」界面照常出现，取消即退出。要标签才能做的操作（挂载、读写应用区、amiibo 设置小程序）一律回「没找到标签」。
// 游戏不挂激活 / 失活事件，靠轮询 GetDeviceState 看有没有标签。
// ---------- nfp: amiibo ----------
// From Mario 1.2.1 the game initializes nfp at boot (v0 did not use it). The real SDK would connect to the sm: service, which the host lacks → abort, so the whole family is taken over.
// The host has no NFC reader: it reports one NFC device on the currently connected controller (same as hle_hid.c) whose detection never finds a tag -- the same as real hardware with no amiibo at hand:
// the game's "touch an amiibo" screen appears as usual and cancelling leaves it. Operations that need a tag (mount, application area read / write, amiibo settings applets) all return "tag not found".
// The game attaches no activate / deactivate events; it polls GetDeviceState to see whether a tag is there.
#define NFP_RES_TAG_NOT_FOUND (115u | 97u << 9)   // 模块 115（nfp）描述 97 | module 115 (nfp), description 97
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1 };   // nn::nfp::DeviceState：0 初始、1 找标签中（2 有标签，宿主不会出现） | 0 initialized, 1 searching for a tag (2 = tag found, never on the host)
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
static _Atomic int g_nfp_state;
uint32_t hle_hid_connected_npad(void);
static Result nfp_no_tag(const char *what) {
    static _Atomic int logged; if (!atomic_exchange(&logged, 1)) LOG("nfp：%s 等要 amiibo 标签的操作 → 没找到标签（宿主无 NFC）", what);
    return NFP_RES_TAG_NOT_FOUND;
}
HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo 库初始化；宿主一台 NFC 设备、永远找不到标签", void, h_nfp_Init, void) { LOG("nfp::Initialize：宿主一台 NFC 设备，找不到 amiibo 标签"); }
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC 可用性变化事件；绑内核事件、恒不触发", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC 设备列表：一台", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "NFC 设备所在手柄：当前接着的那个", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) { (void)h; *out = hle_hid_connected_npad(); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "开始找标签（永远找不到）", Result, h_nfp_Start, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_SEARCH); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "停止找标签", Result, h_nfp_Stop, const NfpDeviceHandle *h) { (void)h; atomic_store(&g_nfp_state, NFP_STATE_INIT); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "设备状态：初始 / 找标签中", int, h_nfp_State, const NfpDeviceHandle *h) { (void)h; return atomic_load(&g_nfp_state); }
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "标签信息：没有标签", Result, h_nfp_TagInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetTagInfo"); }
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "型号信息：没有标签", Result, h_nfp_ModelInfo, void *out, const NfpDeviceHandle *h) { (void)out; (void)h; return nfp_no_tag("GetModelInfo"); }
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "挂载标签：没有标签", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { (void)h; (void)model; (void)target; return nfp_no_tag("Mount"); }
HLE_FORCE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "写回标签：没有标签", Result, h_nfp_Flush, const NfpDeviceHandle *h) { (void)h; return nfp_no_tag("Flush"); }
HLE_FORCE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "打开应用区：没有标签", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) { (void)h; (void)id; return nfp_no_tag("OpenApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "读应用区：没有标签", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) { (void)buf; (void)h; (void)sz; if (n) *n = 0; return nfp_no_tag("GetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "写应用区：没有标签", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) { (void)h; (void)buf; (void)sz; return nfp_no_tag("SetApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "建应用区：没有标签", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("CreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "重建应用区：没有标签", Result, h_nfp_RecreateArea, const NfpDeviceHandle *h, const void *info) { (void)h; (void)info; return nfp_no_tag("RecreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 修复小程序：没有标签", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartRestorer"); }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 游戏数据删除小程序：没有标签", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_no_tag("StartGameDataEraser"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo 昵称与主人设置小程序：没有标签", Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag) { (void)h; (void)reg; (void)p; (void)tag; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", "amiibo 昵称与主人设置小程序（带原登记信息）：没有标签", Result, h_nfp_Nick2, NfpDeviceHandle *h, uint8_t *changed, void *reg, const void *p, const void *tag, const void *old) { (void)h; (void)reg; (void)p; (void)tag; (void)old; if (changed) *changed = 0; return nfp_no_tag("StartNicknameAndOwnerSettings"); }

// ---------- vi：垂直同步事件 ----------
// 游戏给一个 SystemEventType，宿主绑定后由一条宿主线程按 60 Hz 触发（DM_VSYNC_HZ 可改）
// ---------- vi: vsync event ----------
// The game passes a SystemEventType; after binding, a host thread fires it at 60 Hz (DM_VSYNC_HZ can change it)
static void *vsync_thread(void *ev) {
    const char *e = getenv("DM_VSYNC_HZ"); double hz = e ? atof(e) : 60.0; if (hz <= 0) hz = 60.0;
    uint64_t period = (uint64_t)(1e9 / hz), next = now_ns() + period;
    for (;;) {
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)}; nanosleep(&ts, NULL); }
        hle_signal_system_event(ev); next += period; if (now_ns() > next + 4 * period) next = now_ns() + period;   // 落后太多不追帧 | do not try to catch up when far behind
    }
    return NULL;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "垂直同步事件：宿主线程按 60 Hz 触发", Result, h_vi_VsyncEvent, void *ev, void *disp) {
    (void)disp; hle_bind_system_event(ev, 1);
    pthread_t th; if (pthread_create(&th, NULL, vsync_thread, ev)) die("垂直同步线程起不来"); pthread_detach(th);
    LOG("vi::GetDisplayVsyncEvent：%p 由宿主 60 Hz 触发", ev); return RES_OK;
}

// ---------- diag ----------
// LogMetaData 之后的文本（NN_LOG）直接进宿主日志；中止打出调用点后报红退出
// The text after LogMetaData (NN_LOG) goes straight into the host log; an abort prints the call site and exits with a failure
HLE_FORCE("_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", "游戏日志 → 宿主日志", void, h_diag_Put, const void *meta, const char *s, size_t n) {
    (void)meta; while (n && (s[n - 1] == '\n' || s[n - 1] == '\0')) n--; if (n) LOG("[game] %.*s", (int)n, s);
}
// 中止：报出调用点（模块 + 偏移）与客体回溯再退出
// Abort: report the call site (module + offset) and the guest backtrace, then exit
static void diag_abort(uint32_t res, void *lr) {
    module_t *m = module_of(lr);
    LOG("游戏调用 nn::diag 中止：Result %#x，调用点 %s+%#llx", res, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)lr - m->base) : (unsigned long long)(uintptr_t)lr);
    dm_backtrace((uint64_t)__builtin_frame_address(1));
    die("游戏中止");
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEv", "游戏中止", void, h_diag_Abort0, void) { diag_abort(0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", "游戏中止（带 Result）", void, h_diag_AbortR, const uint32_t *r) { diag_abort(r ? *r : 0, __builtin_return_address(0)); }
HLE_FORCE("_ZN2nn4diag12GetBacktraceEPmi", "回溯；宿主不提供（0 帧）", int, h_diag_Bt, uint64_t *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn4diag13GetSymbolNameEPcmm", "符号名；宿主不提供（空串）", uint64_t, h_diag_SymName, char *buf, size_t n, uint64_t addr) { (void)addr; if (buf && n) buf[0] = 0; return 0; }

// ---------- settings：语言枚举 ↔ 语言代码 ----------
// 真机 MakeLanguageCode 问 settings 服务；表是公开固定的（nn::settings::Language 顺序）。LanguageCode = char[8]
// ---------- settings: language enum <-> language code ----------
// On the real device MakeLanguageCode asks the settings service; the table is public and fixed (nn::settings::Language order). LanguageCode = char[8]
static const char *const k_lang_code[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
HLE_FORCE("_ZN2nn8settingseqERKNS0_8LanguageERKNS0_12LanguageCodeE", "语言枚举与语言代码比较（返回 bool）", unsigned, h_set_LangEq, const int32_t *lang, const char *code) {
    if ((unsigned)*lang >= sizeof k_lang_code / sizeof *k_lang_code) return 0;
    return strncmp(k_lang_code[*lang], code, 8) == 0;
}
