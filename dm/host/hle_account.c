// nn::account：真机经 acc:u0 服务管理用户；宿主固定一个本地用户。
// 联网账号（网络服务账号 / 任天堂账号授权 / 令牌）属排除范围，统一回 RES_FAIL，异步上下文的完成事件立即触发。
// 环境开关：DM_NICK=昵称（默认 Player）。
// nn::account: the real device manages users via the acc:u0 service; the host has one fixed local user.
// Online accounts (network service account / Nintendo Account authorization / tokens) are out of scope and uniformly return RES_FAIL; the async context's completion event fires immediately.
// Environment switch: DM_NICK=nickname (default Player).
#include "hle.h"

typedef struct { uint64_t v[2]; } Uid;                 // nn::account::Uid，16 字节 | nn::account::Uid, 16 bytes
typedef struct { uint64_t v[3]; } UserHandle;          // nn::account::UserHandle，24 字节 | nn::account::UserHandle, 24 bytes
_Static_assert(sizeof(Uid) == 16, "Uid");
static const Uid k_uid = {{0x78626c0000000001ull, 0x53574e4154495645ull}};   // 非零即有效 | Non-zero means valid
#define HANDLE_MAGIC 0x55534552484e444cull                                    // "USERHNDL"

HLE_FORCE("_ZN2nn7account10InitializeEv", "连 acc:u0 账号服务；宿主固定本地用户", void, h_acc_Init, void) {}

HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "已打开用户列表；恒为本地用户 1 个", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    *n = 0;
    if (max > 0) { out[0] = k_uid; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account16ShowUserSelectorEPNS0_3UidERKNS0_21UserSelectionSettingsE", "用户选择小程序；宿主直接选本地用户", Result, h_acc_ShowSel, Uid *out, const void *settings) {
    (void)settings; *out = k_uid; return RES_OK;
}
// 取走系统代应用打开的用户（马力欧 1.2.1 起先问它，0 个再走 TryOpenPreselectedUser；游戏只看个数、不看返回值）：
// 宿主没有代开的用户，给 0 个，游戏照 v0 的路自己打开预选用户
// Take the users the system opened on the application's behalf (Mario 1.2.1 asks this first and falls back to TryOpenPreselectedUser on 0; the game only reads the count, not the return value):
// the host has no such users and returns 0, so the game opens the preselected user itself, as in v0
HLE_FORCE("_ZN2nn7account12PopOpenUsersEPiPNS0_10UserHandleEi", "系统代开的用户：无（游戏改走预选用户）", void, h_acc_PopOpen, int *n, UserHandle *out, int max) { (void)out; (void)max; *n = 0; }
HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "启动时预选用户；恒为本地用户（返回 bool）", unsigned, h_acc_TryOpenPre, UserHandle *h) {
    h->v[0] = k_uid.v[0]; h->v[1] = k_uid.v[1]; h->v[2] = HANDLE_MAGIC; return 1;
}
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "打开用户；句柄内直接存 Uid", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) {
    h->v[0] = uid->v[0]; h->v[1] = uid->v[1]; h->v[2] = HANDLE_MAGIC; return RES_OK;
}
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "从句柄取 Uid", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    if (h->v[2] != HANDLE_MAGIC) die("account::GetUserId 句柄不是宿主发的（%#llx）", h->v[2]);
    out->v[0] = h->v[0]; out->v[1] = h->v[1]; return RES_OK;
}
// nn::account::Nickname = struct { char name[33]; }，UTF-8
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "用户昵称；按 DM_NICK 给值", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    (void)uid;
    const char *e = getenv("DM_NICK");
    memset(nick, 0, 33); strncpy(nick, e ? e : "Player", 32);
    return RES_OK;
}

// ---------- 联网账号：剪 ----------
// ---------- Online accounts: cut ----------
HLE_FORCE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "网络服务账号，排除范围；剪口：恒不可用", Result, h_acc_EnsureNsa, const UserHandle *h) { (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "网络服务账号，排除范围；剪口", Result, h_acc_GetNsaId, void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "网络服务账号令牌，排除范围；剪口", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account40ShowLicenseRequirementsForNetworkServiceERKNS0_10UserHandleE", "联网会员提示小程序，排除范围；剪口", Result, h_acc_ShowLic, const UserHandle *h) { (void)h; return RES_FAIL; }

// AsyncContext / NintendoAccountAuthorizationRequestContext：对象内容只由下列接口读写，构造不写内存，完成事件立即触发、结果恒失败
// AsyncContext / NintendoAccountAuthorizationRequestContext: object contents are read and written only by the interfaces below; construction writes no memory, the completion event fires immediately, and the result is always failure
HLE_FORCE("_ZN2nn7account12AsyncContextC1Ev", "异步上下文构造；宿主不建 IPC 对象", void, h_acc_AcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "网络服务账号令牌，排除范围；剪口", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "异步完成事件；立即触发", Result, h_acc_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext9GetResultEv", "异步结果；联网已剪，恒失败", Result, h_acc_AcGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextC1Ev", "任天堂账号授权，排除范围；构造不写内存", void, h_acc_NaCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "任天堂账号授权，排除范围", void, h_acc_NaDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account41CreateNintendoAccountAuthorizationRequestEPNS0_42NintendoAccountAuthorizationRequestContextERKNS0_10UserHandleERKNS0_45NintendoAccountAuthorizationRequestParametersEPvm",
          "任天堂账号授权，排除范围；剪口", Result, h_acc_NaCreate, void *ctx, const UserHandle *h, const void *prm, void *buf, uint64_t sz) {
    (void)ctx; (void)h; (void)prm; (void)buf; (void)sz; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext14GetSystemEventEPNS_2os11SystemEventE", "授权完成事件；立即触发", Result, h_acc_NaGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext34GetResultWithInteractionIfRequiredEv", "授权结果；恒失败", Result, h_acc_NaGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContext10GetIdTokenEPmPcm", "授权令牌；恒失败", Result, h_acc_NaGetTok, void *self, uint64_t *n, char *buf, uint64_t sz) {
    (void)self; (void)buf; (void)sz; if (n) *n = 0; return RES_FAIL;
}

// ---------- 较新 SDK 用到的账号接口：用户枚举 / 存在性 / 头像，异步上下文的其余成员 ----------
// ---------- Account interfaces used by newer SDKs: user enumeration / existence / profile image, the remaining AsyncContext members ----------
HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "用户数；恒为本地用户 1 个", Result, h_acc_UserCount, int *n) { *n = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "可用用户列表；恒为本地用户 1 个", Result, h_acc_ListQual, int *n, Uid *out, int max) {
    int k = 0; if (max > 0 && out) { out[0] = k_uid; k = 1; } *n = k; return RES_OK;
}
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "用户是否存在；本地用户为真", Result, h_acc_Exists, uint8_t *out, const Uid *uid) {
    *out = uid->v[0] == k_uid.v[0] && uid->v[1] == k_uid.v[1]; return RES_OK;
}
HLE_FORCE("_ZN2nn7account15ShowUserCreatorEv", "新建用户小程序；宿主已有本地用户", Result, h_acc_ShowCreator, void) { return RES_OK; }
HLE_FORCE("_ZN2nn7account16LoadProfileImageEPmPvmRKNS0_3UidE", "用户头像；宿主没有（失败）", Result, h_acc_LoadImage, uint64_t *n, void *buf, uint64_t sz, const Uid *uid) {
    (void)buf; (void)sz; (void)uid; if (n) *n = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", "网络服务账号，排除范围；恒不可用", Result, h_acc_NsaAvail, uint8_t *out, const UserHandle *h) { (void)h; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "关闭用户；句柄里没有会话", void, h_acc_CloseUser, const UserHandle *h) { (void)h; }
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "异步上下文取消；无请求", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext7HasDoneEPb", "异步上下文：恒已完成", Result, h_acc_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "异步上下文析构；无会话可关", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "异步上下文移动赋值；对象里没有宿主状态", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }
