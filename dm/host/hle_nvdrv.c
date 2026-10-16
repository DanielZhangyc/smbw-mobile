// nvdrv（NVIDIA 驱动服务）：宿主把 NVN 整个接管了，用不到它；只有 SDK 影片库的硬件解码器（subsdk0 的 NVOMX → NvRmChannelOpen）直接开 /dev/nvhost-*。
// 原版 NvOsDrvOpen 先连 nvdrv 服务，宿主没有服务进程，SDK 当场中止（Engage 起机时 Unity 建解码器即中止）。
// 接管：开设备一律失败（fd = −1），ioctl / 映射 / 查事件一律回错，解码器走它自己的失败路径
// nvdrv (the NVIDIA driver service): the host takes over NVN entirely and has no use for it; only the SDK movie library's hardware decoder (subsdk0's NVOMX → NvRmChannelOpen) opens /dev/nvhost-* directly.
// The original NvOsDrvOpen first connects to the nvdrv service; the host has no service process and the SDK aborts on the spot (Engage aborted when Unity created a decoder at startup).
// Takeover: opening a device always fails (fd = -1), and ioctls / mappings / event queries all return an error, so the decoder takes its own failure path
#include "hle.h"

#define NV_ERR 3   // NvError：非零即失败（3 = 未初始化） | NvError: non-zero means failure (3 = not initialized)

HLE_FORCE("NvOsDrvOpen", "nvdrv 开设备：宿主没有 nvdrv，回 −1", int, h_nvos_Open, const char *path) {
    static int red; if (!red++) LOG("【红】nvdrv：%s 开不了（宿主没有 nvdrv；只有影片硬件解码用它）", path ? path : "(null)");
    return -1;
}
HLE_FORCE("NvOsDrvClose", "nvdrv 关设备；无事可做", void, h_nvos_Close, int fd) { (void)fd; }
HLE_FORCE("NvOsDrvIoctl", "nvdrv ioctl：回错", int, h_nvos_Ioctl, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvIoctl2", "nvdrv ioctl2：回错", int, h_nvos_Ioctl2, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvIoctl3", "nvdrv ioctl3：回错", int, h_nvos_Ioctl3, int fd, uint32_t req, void *arg, size_t n) { (void)fd; (void)req; (void)arg; (void)n; return NV_ERR; }
HLE_FORCE("NvOsDrvMapSharedMem", "nvdrv 共享内存映射：回错", int, h_nvos_MapShared, int fd) { (void)fd; return NV_ERR; }
HLE_FORCE("_Z17NvOsDrvQueryEventijPN2nn2os15SystemEventTypeE", "nvdrv 查事件：回错", int, h_nvos_QueryEvent, int fd, uint32_t id, void *ev) { (void)fd; (void)id; (void)ev; return NV_ERR; }
