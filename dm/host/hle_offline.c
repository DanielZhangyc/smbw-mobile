// 联网与系统周边服务（排除范围：联机、本地无线、好友、遥测、SSL）在较新 SDK 上的接口：一律做成「不可用」，游戏走离线分支。
// 只接会走服务的入口；只读写游戏自己缓冲的库函数（prepo 的 Add / Array / Struct、err 的 ErrorResultVariant、friends 的取值函数、socket 的字节序转换）留在 SDK 原样跑。
// 返回约定同 hle_friends.c：Result 回 RES_FAIL（「已保存 / 已提交」类回 RES_OK 免得游戏重试）；bool 回假；事件给已绑定、永不触发的 SystemEvent。
// Networking and peripheral system services (out of scope: online play, local wireless, friends, telemetry, SSL) as seen through newer SDKs: all made "unavailable", the game takes its offline branch.
// Only entry points that go to a service are hooked; library functions that only read and write the game's own buffers (prepo Add / Array / Struct, err ErrorResultVariant, friends getters, socket byte-order conversion) keep running in the SDK.
// Return convention as in hle_friends.c: Result returns RES_FAIL ("saved / submitted" kinds return RES_OK so the game does not retry); bool returns false; events get a bound SystemEvent that never fires.
#include "hle.h"

static _Alignas(16) uint8_t g_ev_ldn[0x100];

// ---------- prepo：遥测报告。存 / 提交都报成功，什么也不发 ----------
// ---------- prepo: telemetry reports. Saving / submitting all report success; nothing is sent ----------
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "遥测报告保存（用户）；不发送，报成功", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "遥测报告保存；不发送，报成功", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10InitializeEv", "遥测初始化：原版连 prepo 服务（经 sm），宿主没有服务进程；空操作（Engage 显式调用）", Result, h_prepo_Init, void) { return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "遥测立即上传；不发送，报成功", Result, h_prepo_Tx, void) { return RES_OK; }

// ---------- ldn：本地无线。初始化即失败，状态恒为「无」 ----------
// ---------- ldn: local wireless. Initialization fails, state is always "none" ----------
HLE_FORCE("_ZN2nn3ldn10InitializeEv", "本地无线（剪）：初始化失败", Result, h_ldn_Init, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn8FinalizeEv", "本地无线（剪）", void, h_ldn_Fini, void) {}
HLE_FORCE("_ZN2nn3ldn8GetStateEv", "本地无线（剪）：状态 0（无）", int, h_ldn_State, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", "本地无线状态事件：绑定、永不触发", void, h_ldn_Attach, void *ev) { hle_bind_system_event(ev, 0); (void)g_ev_ldn; }
HLE_FORCE("_ZN2nn3ldn11OpenStationEv", "本地无线（剪）", Result, h_ldn_OpenSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn12CloseStationEv", "本地无线（剪）", Result, h_ldn_CloseSta, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn15OpenAccessPointEv", "本地无线（剪）", Result, h_ldn_OpenAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16CloseAccessPointEv", "本地无线（剪）", Result, h_ldn_CloseAp, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn10DisconnectEv", "本地无线（剪）", Result, h_ldn_Disc, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14DestroyNetworkEv", "本地无线（剪）", Result, h_ldn_Destroy, void) { return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "本地无线扫描（剪）：0 个", Result, h_ldn_Scan, void *out, int *n, int max, const void *f, int ch) { (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", "本地无线扫描（剪）：0 个", Result, h_ldn_ScanP, void *out, int *n, int max, const void *f, int ch) { (void)out; (void)max; (void)f; (void)ch; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", "本地无线（剪）", Result, h_ldn_Create,
          const void *a, const void *b, const void *c) { (void)a; (void)b; (void)c; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE", "本地无线（剪）",
          Result, h_ldn_CreateP, const void *a, const void *b, const void *c, const void *d, int n, const void *e) { (void)a; (void)b; (void)c; (void)d; (void)n; (void)e; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE", "本地无线（剪）", Result, h_ldn_Connect,
          const void *a, const void *b, const void *c, int v, int o) { (void)a; (void)b; (void)c; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE", "本地无线（剪）",
          Result, h_ldn_ConnectP, const void *a, const void *b, const void *c, const void *d, int v, int o) { (void)a; (void)b; (void)c; (void)d; (void)v; (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", "本地无线（剪）", Result, h_ldn_NetInfo, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", "本地无线（剪）", Result, h_ldn_Ip, uint32_t *a, uint32_t *m) { if (a) *a = 0; if (m) *m = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", "本地无线（剪）", Result, h_ldn_SecPrm, void *o) { (void)o; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", "本地无线（剪）", Result, h_ldn_Adv, const void *d, uint64_t n) { (void)d; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn19GetDisconnectReasonEv", "本地无线（剪）：0", int, h_ldn_DiscReason, void) { return 0; }
HLE_FORCE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", "本地无线（剪）", Result, h_ldn_Filter, uint64_t mac) { (void)mac; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", "本地无线（剪）", Result, h_ldn_Policy, int p) { (void)p; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", "本地无线（剪）", Result, h_ldn_Reject, uint32_t a) { (void)a; return RES_FAIL; }

// ---------- socket：离线。初始化成功（网络库全局初始化依赖它），之后的操作一律失败、errno = ENETDOWN ----------
// ---------- socket: offline. Initialization succeeds (the network library's global init depends on it), every later operation fails with errno = ENETDOWN ----------
enum { SK_ENETDOWN = 50, SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "连 bsd 套接字服务；离线，不建会话（剪）", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "与 Initialize 配对（剪）", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "离线：ENETDOWN", int, h_sk_Errno, void) { return SK_ENETDOWN; }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "离线（剪）：-1", int, h_sk_Socket3i, int a, int b, int c) { (void)a; (void)b; (void)c; return -1; }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "离线（剪）", int, h_sk_Shutdown, int s, int h) { (void)s; (void)h; return -1; }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "离线（剪）", int, h_sk_Bind, int s, const void *a, unsigned n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "离线（剪）", int, h_sk_Poll, void *p, uint64_t n, int t) { (void)p; (void)n; (void)t; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "离线（剪）", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "离线（剪）", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "离线（剪）", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { (void)s; (void)b; (void)n; (void)f; (void)a; (void)al; return -1; }
HLE_FORCE("_ZN2nn6socket11GetAddrInfoEPKcS2_PK8addrinfoPPS3_", "域名解析（剪）：EAI_FAIL", int, h_sk_Gai2, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "与 GetAddrInfo 配对（剪）", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "解析错误文字", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }

// ---------- ssl：离线，建上下文即失败 ----------
// ---------- ssl: offline, creating a context fails ----------
HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL（剪）", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL（剪）", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL（剪）", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL（剪）", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

// ---------- nifm / nsd：离线 ----------
// ---------- nifm / nsd: offline ----------
HLE_FORCE("_ZN2nn4nifm19SetLocalNetworkModeEb", "离线：本地网络模式开关忽略", void, h_nifm_LocalMode, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "离线：网络请求不提交（立即返回，IsNetworkAvailable 恒假）", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", "离线：无 IP 配置", Result, h_nifm_IpCfg, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d, uint32_t *e) {
    uint32_t *p[5] = {a, b, c, d, e}; for (int i = 0; i < 5; i++) if (p[i]) *p[i] = 0; return RES_FAIL;
}
// EnvironmentIdentifier = char[8]：零售环境 "lp1"
// EnvironmentIdentifier = char[8]: retail environment "lp1"
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "服务器环境名：零售 lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }

// ---------- friends：较新 SDK 的重载 ----------
// ---------- friends: newer SDK overloads ----------
HLE_FORCE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", "好友库分配器；不建会话，忽略", void, h_fr_SetAlloc, void *a, void *f) { (void)a; (void)f; }
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "好友列表为空（剪）", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", "好友列表不可用（剪）", Result, h_fr_ListAvail, uint8_t *out, const void *uid) { (void)uid; *out = 0; return RES_OK; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "好友列表（剪）：请求即失败", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12AsyncContext7HasDoneEPb", "异步上下文：恒已完成", Result, h_fr_AcHasDone, void *self, uint8_t *done) { (void)self; *done = 1; return RES_OK; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "游玩记录不登记（剪）",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; return RES_FAIL; }

// ---------- ngc：较新 SDK 的重载（带 bool）。恒判无敏感词 ----------
// ---------- ngc: newer SDK overloads (with bool). Always judged as no profanity ----------
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "敏感词过滤初始化；宿主无词库", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "敏感词检查：全部无敏感词", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words; for (uint64_t i = 0; out && i < n; i++) out[i] = 0; return RES_OK;
}

// ---------- err：错误显示小程序。只记日志，不弹界面 ----------
// ---------- err: error display applet. Only logged, no UI is shown ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "错误显示小程序；宿主只记日志", void, h_err_ShowCode, uint64_t code) { LOG("游戏请求显示错误：%04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32)); }
HLE_FORCE("_ZN2nn3err9ShowErrorERKNS0_18ErrorResultVariantE", "错误显示小程序；宿主只记日志", void, h_err_ShowVar, const void *v) { (void)v; LOG("游戏请求显示错误（ErrorResultVariant）"); }

// ---------- C 接口（nnsocket* / nnssl*）：同样离线。参数都在寄存器里，宿主不看 ----------
// ---------- C interface (nnsocket* / nnssl*): offline as well. The arguments are all in registers and the host does not look at them ----------
HLE_FORCE("nnsocketAccept", "离线（剪）", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "离线（剪）", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "离线（剪）", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "离线（剪）", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "离线（剪）", int, h_c_sk_Shutdown, void) { return -1; }
HLE_FORCE("nnsocketGetAddrInfo", "域名解析（剪）：EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "与 GetAddrInfo 配对（剪）", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "解析错误文字", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
