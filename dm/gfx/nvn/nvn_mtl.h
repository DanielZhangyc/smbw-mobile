// NVN 空设备的 Metal 执行层（C 接口）：内存池 = 零拷贝 MTLBuffer，提交 = MTLCommandBuffer，同步 = MTLSharedEvent。
// 时序约定（防 x86 路线 GO_MAP_SHARE 花屏的覆辙）：一切「GPU 完成」的可见效果（同步对象就绪、事件值、计数器）
// 只在 Metal 命令缓冲真正完成之后才发生；CPU 侧等待只看 MTLSharedEvent 的已完成值。
// Metal execution layer of the NVN null device (C interface): memory pool = zero-copy MTLBuffer, submit = MTLCommandBuffer, sync = MTLSharedEvent.
// Ordering convention (to avoid repeating the GO_MAP_SHARE corrupted-picture mistake of the x86 route): every visible effect of "GPU completion" (sync object ready, event values, counters)
// happens only after the Metal command buffer has actually completed; CPU-side waits look only at the completed value of the MTLSharedEvent.
#pragma once
#include <stdint.h>
#include <stddef.h>

void mtl_init(void);
void bf_want_note(const char *key);

// 内存池：[mem, mem+size) 外扩到页边界建零拷贝缓冲；按 GPU 地址（= CPU 地址）反查缓冲与偏移
// Memory pool: [mem, mem+size) is expanded outward to page boundaries to build a zero-copy buffer; look up the buffer and offset by GPU address (= CPU address)
void mtl_pool_add(const void *mem, uint64_t size);
void mtl_pool_del(const void *mem);
// 池对象真正销毁时撤销该实例的贴图共享及描述符缓存；虚拟池也须调用，不能用存储地址替代对象身份。
// Retire texture sharing and descriptor caches when the pool instance is finalized; virtual pools also call this, using object identity rather than storage address.
void mtl_pool_tex_finalize(const void *pool);
// 游戏分配器的真实租借边界；归还只标记候选，命令引用消失后才按 GPU fence 回收。
// Actual guest allocator lease boundaries; return only marks candidates, reclaimed by GPU fence after recorded references disappear.
void mtl_storage_lease_begin(const void *texture);
void mtl_storage_lease_end(const void *pool, uint64_t offset, uint64_t size);
// 交换链存储由独立出图队列使用，不参与临时纹理回收。
// Swapchain storage is used by the independent present queue and excluded from temporary-texture reclamation.
void mtl_storage_preserve_pool(const void *pool);
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off);   // 成功 1；buf 是 (__bridge) id<MTLBuffer> | 1 on success; buf is a (__bridge) id<MTLBuffer>
// DM_BOOTPROF：全宿主地址查找累计计数（不是提交线程 CPU 时间）；按 present 快照取差。
// DM_BOOTPROF: cumulative address lookup counts across the host (not submit-thread CPU time); difference snapshots at present.
typedef struct { uint64_t lookups, pool_scans, cache_hits; } mtl_addr_stats;
void mtl_addr_stats_get(mtl_addr_stats *out);

// 普通 PSO 预测池：occupied/inflight 为当前槽位数，其余为累计次数；capacity_skips 计无法分配槽位的请求。
// Ordinary PSO prediction pool: occupied/inflight are current slot counts; the others are cumulative, with capacity_skips counting requests unable to reserve a slot.
typedef struct { uint64_t occupied, inflight, kicked, taken, evicted, capacity_skips; } mtl_pso_pending_stats;
void mtl_pso_pending_stats_get(mtl_pso_pending_stats *out);

// DM_NVN_PRESENT_TRACE：票号锁存的真实呈现诊断；时刻为 CACurrentMediaTime 基准秒。
// DM_NVN_PRESENT_TRACE: real presentation diagnostics latched by ticket; timestamps use CACurrentMediaTime seconds.
enum {
    MTL_PRESENT_TRACE_TARGET = 1u << 0, MTL_PRESENT_TRACE_DESIRED = 1u << 1,
    MTL_PRESENT_TRACE_COMMIT = 1u << 2, MTL_PRESENT_TRACE_CALL = 1u << 3,
    MTL_PRESENT_TRACE_RETURN = 1u << 4, MTL_PRESENT_TRACE_GPU_BEGIN = 1u << 5,
    MTL_PRESENT_TRACE_GPU_END = 1u << 6, MTL_PRESENT_TRACE_MIN_DURATION = 1u << 7,
    MTL_PRESENT_TRACE_GPU_RESULT = 1u << 8, MTL_PRESENT_TRACE_DEADLINE = 1u << 9,
    MTL_PRESENT_TRACE_ENQUEUE = 1u << 10
};
typedef struct {
    uint64_t ticket, guest_frame, present_index;
    uint32_t valid;
    int mode, gpu_error;   // mode：0 present、1 minimum duration、2 explicit time；error：0 成功、1 失败。
                          // mode: 0 present, 1 minimum duration, 2 explicit time; error: 0 success, 1 failure.
    double target_at, desired_at, commit_at, call_at, return_at, gpu_begin_at, gpu_end_at, min_duration, deadline_at, enqueue_at;
} mtl_present_trace;
int mtl_present_trace_enabled(void);
// 关闭或票号尚无记录返回 0；out 不改写。valid 未置位的字段属于缺测，不能填零代替。
// Returns 0 when disabled or the ticket has no record, leaving out unchanged; unset valid bits mean missing, not zero.
int mtl_present_trace_get(uint64_t ticket, mtl_present_trace *out);
// 传统出图可限制尚未呈现的帧数；1=目标已上屏/丢弃，0=未启用/失焦，-1=等待超时。
// Classic present can bound unpresented frames: 1=target presented/discarded, 0=disabled/unfocused, -1=timed out.
int mtl_present_throttle(uint64_t timeout_ns);

typedef struct {
    uint32_t pso, cps, shaders, dss;
    uint32_t view_nonnull, view_slots, live_tex, sview_nonnull;
    uint32_t pools;
    uint64_t pool_adds;
    uint32_t c1_bufs; uint64_t c1_alloc, c1_data;   // 着色器 c1 常数段：缓冲个数（4 MiB 块 + 独占 64 KiB）、分配字节、实际字节 | Shader c1 constant segment: buffer count (4 MiB blocks + dedicated 64 KiB), allocated bytes, actual bytes
} mtl_res_count;
void mtl_res_counts(uint32_t tick, const char *why);   // 打一行资源计数（宿主按拍 / 事件调用，见 dm.h dm_res_report_register） | Print one line of resource counts (the host calls it per tick / event, see dm_res_report_register in dm.h)
void mtl_res_snapshot(mtl_res_count *out);

// DM_BOOTPROF：提交线程的反馈累计 CPU 时间 / 次数；detect / prepare 仅计最外层，prepare 含嵌套准备及 pool、copy，entries 是当前池大小。关闭时返回 0。
// DM_BOOTPROF: cumulative feedback CPU time / counts on the submit thread; detect / prepare time only the outermost call, prepare includes nested preparation and pool / copy, entries is the current pool size. Returns 0 when disabled.
typedef struct {
    uint64_t detect_cpu_ns, prepare_cpu_ns, pool_cpu_ns, copy_cpu_ns;
    uint64_t pool_allocations, copy_calls, copy_subresources, snapshot_draws, pool_entries, domain_exclusions;
} mtl_feedback_profile;
int mtl_feedback_stats(mtl_feedback_profile *out);

// 一次提交：begin → 若干操作 → commit（返回本次提交完成时的栅栏值）
// One submit: begin -> several operations -> commit (returns the fence value at which this submit completes)
void mtl_sub_begin(void);
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n);               // GPU 时序上的缓冲拷贝 | Buffer copy in GPU ordering
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v);   // 完成后写事件值（0 写 / 1 加） | Write the event value after completion (0 write / 1 add)
void mtl_sub_counter(uint64_t addr, uint32_t type);                       // 完成后写计数器记录 | Write the counter record after completion
uint64_t mtl_sub_commit(void);

uint64_t mtl_last_fence(void);                       // 最近一次提交的栅栏值（0 = 从未提交） | Fence value of the most recent submit (0 = never submitted)
int mtl_wait(uint64_t fence, uint64_t timeout_ns);   // GPU 完成到 fence 返回 1，超时 0 | Returns 1 when the GPU has completed up to fence, 0 on timeout
void mtl_finish(void);                               // 等所有已提交的命令完成 | Wait for all submitted commands to complete
void mtl_drain(void);                                // 等队列上全部命令（含出图）完成：提交空命令缓冲并等它 | Wait for all commands on the queue (including presents) to complete: submit an empty command buffer and wait for it
void mtl_xv_window_tex(uint64_t serial);   // 登记窗口（交换链）贴图：跨提交有效性账本永不收它 | Register a window (swapchain) texture: the cross-submit validity ledger never collects it
void mtl_ui_ghost(int on, float px, float py, const float grid[4], float cx, float cy);   // 拖货跟手图：on=1 时把盖住屏幕点 (px,py)、压在货架格网 grid（左上角 x,y、一格宽高）上的那件货的图标在界面趟末尾平移到中心 (cx,cy) 再画一遍（归一化，左上原点）；任意线程调 | Drag-follow image for shop items: when on=1, the icon of the item that covers screen point (px,py) and sits on the shop shelf grid `grid` (top-left x,y, width and height of one cell) is translated to center (cx,cy) and drawn once more at the end of the UI pass (normalized, top-left origin); callable from any thread
int mtl_ui_ghost_rect(float r[4]);   // 上面认到的那件货的屏幕矩形（归一化）；还没认到返回 0 | Screen rectangle (normalized) of the item recognized above; returns 0 if not recognized yet
