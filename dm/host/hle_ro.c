// nn::ro（运行时装 NRO 模块）：真机上连 ldr:ro 服务。宿主没有这个服务，原样跑 SDK 的 Initialize 会在 InitializeWithPortName 里读空会话崩（Engage）。
// Engage（Unity）导入 8 个 ro 接口但 RomFS 里没有 .nro / .nrr：启动时只 Initialize，查符号走 LookupSymbol。
// 接管：Initialize 空操作；LookupSymbol 在已装模块（main / subsdk* / sdk）里按名字找定义；装卸 / 登记模块一律失败并报红一次（没有可装的东西）。
// 只看成败：游戏只判 IsSuccess，失败码取 ro 模块号 22 下的一个非零值。
// nn::ro (loading NRO modules at run time): on the real device it connects to the ldr:ro service. The host has no such service; running the SDK's Initialize as is reads a null session in InitializeWithPortName and crashes (Engage).
// Engage (Unity) imports 8 ro interfaces but its RomFS has no .nro / .nrr: at startup it only calls Initialize, and symbol lookups go through LookupSymbol.
// Takeover: Initialize is a no-op; LookupSymbol finds the definition by name in the loaded modules (main / subsdk* / sdk); loading / unloading / registering modules always fails and reports red once (there is nothing to load).
// Only success matters: the game checks IsSuccess only, so the failure code is a non-zero value under the ro module number 22.
#include "hle.h"

#define RO_FAIL ((Result)(22 | (1 << 9)))   // ro 模块 22 下的失败 | a failure under ro module 22

HLE_FORCE("_ZN2nn2ro10InitializeEv", "连 ldr:ro 服务；宿主没有，查符号直接查已装模块", Result, h_ro_Initialize, void) { LOG("init 接管：ro::Initialize（空操作）"); return 0; }

HLE_FORCE("_ZN2nn2ro12LookupSymbolEPmPKc", "按名字在已装模块里找定义", Result, h_ro_LookupSymbol, uintptr_t *out, const char *name) {
    void *p = name ? guest_sym(name) : NULL;
    static int n; if (n++ < 40) LOG("ro::LookupSymbol %s → %p", name ? name : "(null)", p);
    if (!p) { static int red; if (!red++) LOG("【红】ro::LookupSymbol 找不到 %s（首例）", name ? name : "(null)"); return RO_FAIL; }
    if (out) *out = (uintptr_t)p;
    return 0;
}

HLE_FORCE("_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", "没有装过 NRO：按模块查一律找不到", Result, h_ro_LookupModuleSymbol, uintptr_t *out, const void *m, const char *name) {
    (void)out; (void)m; static int red; if (!red++) LOG("【红】ro::LookupModuleSymbol %s：没有装过 NRO", name ? name : "(null)"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro13GetBufferSizeEPmPKv", "不装 NRO", Result, h_ro_GetBufferSize, size_t *out, const void *image) {
    (void)image; if (out) *out = 0; static int red; if (!red++) LOG("【红】ro::GetBufferSize：宿主不装 NRO"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", "不装 NRO", Result, h_ro_LoadModule, void *m, const void *image, void *buf, size_t size, int flag) {
    (void)m; (void)image; (void)buf; (void)flag; static int red; if (!red++) LOG("【红】ro::LoadModule：宿主不装 NRO（缓冲 %#zx）", size); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", "不装 NRO", void, h_ro_UnloadModule, void *m) { (void)m; }

HLE_FORCE("_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", "不登记 NRR", Result, h_ro_RegisterModuleInfo, void *info, const void *nrr) {
    (void)info; (void)nrr; static int red; if (!red++) LOG("【红】ro::RegisterModuleInfo：宿主不登记 NRR"); return RO_FAIL;
}

HLE_FORCE("_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", "不登记 NRR", void, h_ro_UnregisterModuleInfo, void *info) { (void)info; }
