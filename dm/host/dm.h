// Switch 原生宿主公共头（多模块）。
// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // 当前游戏档案的常数（dm/games/<名>/game.tsv 生成，Makefile 的 GAME 选） | constants of the current game profile (generated from dm/games/<id>/game.tsv, chosen by the Makefile's GAME)

#define MAX_MOD 8
#define LOG(...) do { fprintf(stderr, "[dm] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
typedef struct { uint8_t *d; size_t n; } dm_blob;   // 整份读进内存的一个原版存档文件（dm_pages.c dm_save_blobs） | One save file of the original game read whole into memory (dm_pages.c dm_save_blobs)
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)            // 符号绑定：0 = STB_LOCAL | symbol binding: 0 = STB_LOCAL
#define ELF64_ST_VISIBILITY(o) ((o) & 3)       // 可见性：0 = STV_DEFAULT | visibility: 0 = STV_DEFAULT
enum { STB_LOCAL = 0, STV_DEFAULT = 0 };

// 一个 NSO 模块的运行时状态
// Run-time state of one NSO module
typedef struct {
    char name[16];
    uint8_t *base;             // 映像（含补丁区）在宿主地址空间的首址 | Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, patch_off, key_page_off, key_page_size;   // 来自改写器 fixups 头 | From the rewriter fixups header
    char build_id[33];         // NSO 构建号前 16 字节（十六进制）；旧产物没有则为空 | First 16 bytes of the NSO build ID (hex); empty for older build outputs
    unsigned key_idx[8];
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // 已定义符号的名字哈希表（开放寻址，存符号下标；同名只留下标最小者 = 线性扫描的首个命中） | Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT（模块 _init：登记 DSO 并跑本模块 init_array） | init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
} module_t;

extern module_t g_mod[MAX_MOD];
extern int g_nmod;
// main 模块的构建号（没有则空串）。DM_D2R_BUILD = d2r-mobile 带来的 D2R 构建：写死 D2R 地址或改 D2R 存档的代码只在它上面开
// main's build ID (empty if none). DM_D2R_BUILD = the D2R build that the code inherited from d2r-mobile belongs to: code with fixed D2R addresses or that edits D2R saves runs only on it
#define DM_D2R_BUILD "db0507f36b84200ac05f75d76ed5026f"
static inline const char *dm_main_build(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id; return ""; }
// 四个线程本地槽：K18 = Switch 的 x18、KTLS = tpidr_el0、KTLR = tpidrro_el0（ThreadLocalRegion）、KGATE = 宿主门入口；实际键号由装载器写入键页
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
extern pthread_key_t g_keyres[2], g_keyaspect, g_keyaspectui;

void load_modules(const char *dir, const char **names, int n);
void run_module_inits(void);
void *module_sym(module_t *m, const char *name);
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// 门分派：改写后的 svc / 计时器读经公共入口进来，ret_addr = 该门块返回点，regs 指向保存的 x0..x30
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// 观测门（改写器 OBS 表；编号与 patch_nso.py 一致）：只读回调，挂上即在门命中时调用
// Observation gates (rewriter OBS table; numbering matches patch_nso.py): read-only callbacks, called on a gate hit once attached
enum { OBS_UIMSG = 1, OBS_TICK = 2, OBS_C2S = 3, OBS_C2S_SYS = 4, OBS_SETLANG = 5, OBS_TARGET_RANK = 7, OBS_UI_PROJECT = 8, OBS_FREEZE_WORLD = 10, OBS_FREEZE_CLIENT = 11, OBS_FREEZE_ANIM = 12, OBS_FREEZE_RENDER = 13, OBS_TEXTBOX = 14, OBS_EVENT_MSG = 15, OBS_LEVEL_WARP = 16, OBS_QUEST_KILL = 17, OBS_NOTICE_INPUT = 18, OBS_ALLOC_ENTRY = 19, OBS_ALLOC_RETURN = 20, OBS_ALLOC_BACKING_FAIL = 21, OBS_ALLOC_DESCRIPTOR_FAIL = 22, OBS_STORAGE_FREE = 23, OBS_STORAGE_FREE_INLINE_A = 24, OBS_STORAGE_FREE_INLINE_B = 25, OBS_STORAGE_ALLOC_RETURN = 26, OBS_STORAGE_LEASE_BEGIN = 27, OBS_STORAGE_LEASE_BEGIN_INLINE = 28, OBS_MAX = 29 };   // 6 = iOS 绑定读取（dm_touch_game_bridge.c） | 6 = iOS binding read (dm_touch_game_bridge.c)
void storage_trace_install(void);
void gate_obs_set(int id, void (*fn)(const uint64_t *regs));
int gate_obs_present(int id);
void dm_quiet_difficulty_msg(int on);   // 秘境局里拦原版「游戏难度设为 N」提示（观测门 15，diag.c） | In a rift run, block the original game's 「游戏难度设为 N」(Game difficulty set to N) message (observation gate 15, diag.c)
int gate_svc_present(int num);   // 产物里有没有这个 svc 号的门（剪口引入的宿主调用号用）   // 产物里有没有这个观测门（gates.tsv 登记过） | Whether the build output has a gate for this svc number (for host call numbers introduced by cuts)   // whether the build output has this observation gate (registered in gates.tsv)
uint64_t now_ns(void);
uint64_t guest_ns(void);              // 客体时钟（hle_os.c；DM_TICK_HOLD 冻结时不走） | Guest clock (hle_os.c; does not advance while DM_TICK_HOLD is frozen)
int dm_detclock_enabled(void);
void dm_guest_clock_advanced(void);
void guest_clock_freeze(int on);
void guest_clock_freeze_for(int reason, int on);   // 按原因冻结（1 后台，2 系统键盘）；见 hle_os.c | Freeze by reason (1 background, 2 system keyboard); see hle_os.c
void dm_guest_frame_tick(void);        // 每次出图调（hle_nvn.c）：DM_DETCLOCK 开时把客体时钟推进一个固定步长 | Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
int dm_nvn_ruler_truth(void);         // 视觉尺子真值阶段（DM_NVN_RULER=1 的前两张截图）为 1 | 1 during the ground-truth phase of the visual ruler (the first two screenshots with DM_NVN_RULER=1)
uint64_t dm_nvn_frame_count(void);
uint64_t dm_nvn_sub_cpu_ns(void);     // 提交（不含出图）累计 CPU 纳秒 | Cumulative CPU nanoseconds of submit (excluding present)
int dm_ingame_now(void);              // 是否在局内（diag.c） | Whether we are in a run (diag.c)
void dm_3d_scale_feed(double iv_ms);   // 出图线程每帧报实际上屏间隔（−1 丢弃 / 0 无读数），驱动动态 3D 比例 | The present thread reports the actual present interval every frame (−1 discard / 0 no reading), driving the dynamic 3D scale
float dm_3d_scale_cur(void);          // 当前生效的 3D 渲染比例（hle_oe.c scale3d_tick）     // 当前累计出图帧数（DM_TICKS 对齐观测） | The 3D render scale currently in effect (hle_oe.c scale3d_tick)     // current cumulative count of presented frames (for DM_TICKS alignment observation)
void dm_backtrace(uint64_t fp);
void crash_watch_progress(uint32_t tick, const char *stage);
void crash_watch_frame(void);
void crash_watch_pregame_done(void);
uint64_t crash_watch_progress_count(void);
void crash_dump_now(const char *why);   // 按需导出现场（线程 + 图形等待点），不退出 | Dump state on demand (threads + graphics wait points), without exiting
// 诊断开关：main 启动时从环境变量读一次（getenv 要拿 libc 全局环境锁，进热路径会让全部客体线程排队）
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // 宿主启动时刻（now_ns），日志相对时间用 | Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, movie, dbg, imports, bootprof; } g_flag;
// 启动时间线（DM_BOOTPROF=1，diag.c）：分段打点 + 计数器（纳秒 / 字节，全线程累加）；可用 PROF_GFX_* 埋点
// Boot timeline (DM_BOOTPROF=1, diag.c): segment marks + counters (nanoseconds / bytes, accumulated over all threads); PROF_GFX_* can be used for instrumentation
enum { PROF_FS_READ, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC, PROF_SLEEP, PROF_MAIN_WAIT,
       PROF_GFX_PRESENT, PROF_GFX_SHADER, PROF_TACT, PROF_N };   // PROF_TACT：TACT 块 zlib 解码（svc #0x80，宿主 libdeflate）耗时与输出字节 | PROF_TACT: time and output bytes of TACT block zlib decode (svc #0x80, host libdeflate)
void dm_prof_add(int id, uint64_t ns, uint64_t bytes);
void dm_prof_mark(const char *seg);
uint64_t dm_prof_ribbon_ns(void);   // 第一次出现 OpenPanel MainMenuButtonRibbon 的 now_ns 时刻（0 = 尚未；需 DM_BOOTPROF=1） | The now_ns time at which OpenPanel MainMenuButtonRibbon first appeared (0 = not yet; needs DM_BOOTPROF=1)
// DM_THREAD_WAIT_PROFILE=1：只读调用线程的等待 SVC 计数；超时预算不是实际睡眠，墙钟含内核锁竞争与恢复调度。
// DM_THREAD_WAIT_PROFILE=1: read only the caller's wait-SVC counters; timeout budgets are not sleep durations, and wall time includes kernel-lock contention and rescheduling.
enum { DM_WAIT_SLEEP, DM_WAIT_YIELD, DM_WAIT_LOCK, DM_WAIT_CV, DM_WAIT_ADDR, DM_WAIT_SYNC, DM_WAIT_COUNT };
typedef struct { uint64_t calls, requested_ns, infinite_calls, elapsed_ns; } dm_thread_wait_stat;
// 地址等待只留本采样窗口内最慢的一次；lr 是门返回点，caller_lr 是门桩保存的客体原始 x30。
// Keep only the slowest address wait in this sampling window; lr is the gate return point and caller_lr is the original guest x30 saved by the gate stub.
// wake_observed 为 1 才有 wake_to_return_ns；该间隔从 wake 的 signal 前到 SVC 返回边界，含重取 G 与调度，不是总等待或纯调度耗时。
// wake_to_return_ns is valid only when wake_observed is 1; it spans just before wake's signal to the SVC return boundary, including G reacquisition and scheduling, not total wait or pure scheduling time.
typedef struct { uint64_t elapsed_ns, address, lr, caller_lr, wake_observed, wake_to_return_ns; } dm_thread_wait_peak;
typedef struct { dm_thread_wait_stat kind[DM_WAIT_COUNT]; dm_thread_wait_peak addr_peak; } dm_thread_wait_profile;
// 仅 QueuePresentTexture 每帧调用；首帧或调用线程中断出图后只建基线；同线程连续帧返回 1 和差分，其余返回 0。
// Called once per frame only by QueuePresentTexture; first calls and gaps in a thread's presents establish a baseline, consecutive frames on the same thread return 1 with deltas, and other cases return 0.
// 每次采样都清峰值，包括只建基线的采样；累计计数不清零。
// Every sample clears the peak, including baseline-only samples; cumulative counters are not reset.
int dm_thread_wait_profile_frame(uint64_t frame, dm_thread_wait_profile *out);
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c：触发服务层 SystemEvent（任何线程） | hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // 客体线程上把客体 SystemEvent 绑到内核事件 | On a guest thread, bind a guest SystemEvent to a kernel event
// 缺页钩子 / 宿主写客体内存前钩子（crash.c；写保护脏页跟踪用）
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_res_report_register(void (*fn)(uint32_t tick, const char *why));   // 资源计数回调（图形层注册）：每 250 拍、读取画面已撤、记满退出时调 | Resource count callback (registered by the graphics layer): called every 250 ticks, when the loading screen has been dismissed, and on exit when the record is full
void (*dm_res_report_cb(void))(uint32_t tick, const char *why);   // 取已注册的资源计数回调（未注册为空） | Get the registered resource count callback (null if not registered)
void dm_guest_write_prepare(void *addr, size_t len);
// 图形生命周期（hle_nvn.c）：on=1 挂起（之后提交 / 出图阻塞，返回前 GPU 在途工作清空），on=0 恢复；iOS 前端在进出后台时调用
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_gfx_shot_at(long k, const char *path);
void dm_shot_written(const char *path, int ok);   // 图形侧截图写盘完成（diag.c：DM_TICK_HOLD=E:SHOT 据此解冻） | The graphics side finished writing a screenshot to disk (diag.c: DM_TICK_HOLD=E:SHOT unfreezes on this)
void dm_host_shutdown(const char *why);   // 优雅退出（crash.c）：停后台线程、音频、图形清空后 _exit(0)；SIGTERM 与 iOS App 终止回调用   // 图形侧：当前出图帧之后第 k 帧截图（nvn_mtl.m） | Graceful exit (crash.c): stop background threads and audio, drain graphics, then _exit(0); used by SIGTERM and the iOS App termination callback   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
