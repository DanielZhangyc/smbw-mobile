// Horizon 内核的线程与同步 SVC，原生实现：每个客体线程 = 一个宿主 pthread，跑在 SDK 给的客体栈上。
// SDK 的 nn::os 原样运行，只把它下面的「内核」换成宿主：
//   线程   0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D 优先级 0x0E/0x0F 核掩码
//          0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   锁     0x1A ArbitrateLock 0x1B ArbitrateUnlock（标签字 = 持有者句柄 | 0x40000000 有等待者）
//   条件   0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   地址   0x34 WaitForAddress 0x35 SignalToAddress
//   事件   0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// 语义按公开 Horizon 内核行为（Atmosphère mesosphère 的 KConditionVariable / KAddressArbiter / KSynchronization）。
// 所有等待挂在一把全局锁 G 上，每线程一个条件变量；先求正确，性能后量。
// Thread and synchronization SVCs of the Horizon kernel, implemented natively: each guest thread = one host pthread, running on the guest stack given by the SDK.
// The SDK's nn::os runs as is; only the "kernel" underneath it is replaced by the host:
//   Threads    0x08 CreateThread 0x09 StartThread 0x0A ExitThread 0x0B SleepThread 0x0C/0x0D priority 0x0E/0x0F core mask
//              0x10 GetCurrentProcessorNumber 0x25 GetThreadId 0x19 CancelSynchronization
//   Locks      0x1A ArbitrateLock 0x1B ArbitrateUnlock (tag word = owner handle | 0x40000000 has waiters)
//   Condition  0x1C WaitProcessWideKeyAtomic 0x1D SignalProcessWideKey
//   Address    0x34 WaitForAddress 0x35 SignalToAddress
//   Events     0x45 CreateEvent 0x11 SignalEvent 0x12 ClearEvent 0x17 ResetSignal 0x18 WaitSynchronization 0x16 CloseHandle
// Semantics follow the public Horizon kernel behavior (KConditionVariable / KAddressArbiter / KSynchronization of Atmosphère mesosphère).
// All waits hang on one global lock G, with one condition variable per thread; correctness first, performance to be measured later.
#include "dm.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <stdatomic.h>

#define R_TIMEOUT     0xEA01u   // 内核模块 1，描述 117 | Kernel module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define R_OUT_OF_RES  0xCE01u   // 103
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W };
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns; uint32_t born_tick, exit_tick;   // 建立 / 退出的时刻与当时的游戏拍（现场导出用：作业工人是不是在卡住前退的） | Time of creation / exit and the game tick at that time (for the state dump: did the job worker exit before the stall)
    int started, exited, cancel;
    pthread_t pt; pthread_cond_t cv;
    // 当前等待：kind 0=无 1=锁 2=条件 3=地址 4=同步对象
    // Current wait: kind 0=none 1=lock 2=condition 3=address 4=synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    // 地址等待的唤醒时刻：复位和跨线程写入均持 G；摘链后直到本线程下一次地址等待前保持不变。
    // Address-wait wake time: reset and cross-thread writes hold G; after unlinking it stays unchanged until this thread's next address wait.
    uint64_t addr_wake_ns;
    uint64_t wseq;             // 登记等待的先后序号：同优先级先到先醒（与 Horizon 内核 KThread 等待树同序） | Sequence number of wait registration: at equal priority first come, first woken (same order as the Horizon kernel's KThread wait tree)
    struct thr *next;          // 等待链 | Wait chain
    struct thr *all;           // 全部线程链（现场导出用） | All-threads chain (for the state dump)
    int last_svc; uint64_t last_lr;
    int closed;                // 句柄已关：与 exited 同时成立才释放对象（Horizon：线程对象随最后一个引用消失） | Handle closed: the object is freed only when this holds together with exited (Horizon: the thread object goes away with the last reference)
    uint64_t nsvc;             // SVC 次数（现场导出用） | SVC count (for the state dump)
    // 只有本线程读写；沿用已有线程对象，不为逐帧等待诊断分配内存或取得全局锁。
    // Read and written only by this thread; reuse its existing object without allocating memory or taking a global lock for per-frame wait diagnostics.
    dm_thread_wait_profile wait_profile, wait_previous;
    uint64_t wait_frame;
    int wait_baseline;
    mach_port_t mport;         // 宿主线程端口（取 CPU 时间） | Host thread port (to get CPU time)
    uint32_t qos;
    // 暂停（SetThreadActivity，IL2CPP 的垃圾回收停世界用）：pause_req 请求、paused 已停住；停在 SVC 里时现场取门帧 svc_regs（进 SVC 时登记），
    // 停在客体代码里时现场由信号处理存进 ctx_*；pcv 让请求方等「已停住」、被停线程等「恢复」
    // Pausing (SetThreadActivity, used by IL2CPP's garbage collector to stop the world): pause_req = requested, paused = stopped; when stopped inside an SVC the context comes from the gate frame svc_regs (registered on SVC entry),
    // when stopped in guest code the signal handler saves it into ctx_*; pcv lets the requester wait for "stopped" and the paused thread wait for "resume"
    _Atomic int pause_req; int paused;
    uint64_t *_Atomic svc_regs; uint64_t svc_pc, svc_caller_lr;
    int ctx_ok; uint64_t ctx_x[29], ctx_fp, ctx_lr, ctx_sp, ctx_pc; uint32_t ctx_pstate, ctx_fpcr, ctx_fpsr; __uint128_t ctx_v[32];
    pthread_cond_t pcv;
} thr_t;
static thr_t *g_all;

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;          // 全部在等的线程（锁 / 条件 / 地址 / 同步） | All waiting threads (lock / condition / address / synchronization)
static int g_wait_profile_enabled;

__attribute__((constructor)) static void wait_profile_init(void) {
    const char *e = getenv("DM_THREAD_WAIT_PROFILE");
    if (e && strcmp(e, "0") && strcmp(e, "1")) die("DM_THREAD_WAIT_PROFILE 必须为 0 或 1");
    g_wait_profile_enabled = e && !strcmp(e, "1");
}

int dm_thread_wait_profile_frame(uint64_t frame, dm_thread_wait_profile *out) {
    if (!g_wait_profile_enabled) return 0;
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) return 0;
    int consecutive = t->wait_baseline && t->wait_frame + 1 == frame;
    t->wait_frame = frame;
    if (consecutive) {
        for (int i = 0; i < DM_WAIT_COUNT; i++) {
            const dm_thread_wait_stat *a = &t->wait_profile.kind[i], *b = &t->wait_previous.kind[i];
            out->kind[i] = (dm_thread_wait_stat){a->calls - b->calls, a->requested_ns - b->requested_ns,
                a->infinite_calls - b->infinite_calls, a->elapsed_ns - b->elapsed_ns};
        }
        out->addr_peak = t->wait_profile.addr_peak;
    }
    // 峰值按窗口独立重置，避免首次采样、线程切换或下一帧重复带出旧的慢等待。
    // Reset the peak for every window so startup, thread switches and later frames cannot report an old slow wait again.
    t->wait_profile.addr_peak = (dm_thread_wait_peak){0};
    t->wait_previous = t->wait_profile;
    t->wait_baseline = 1;
    return consecutive;
}

static uint32_t h_new(int type, void *obj) {     // 调用方持 G | Caller holds G
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) { g_h[i].type = type; g_h[i].obj = obj; return 0x1000 + i; }
    die("句柄表满");
}
static void *h_peek(uint32_t h, int type) {      // 只查不报 | Lookup only, no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // 查不到即报（句柄错是真错误） | Reports if not found (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("句柄无效 %#x（要类型 %d）", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("非客体线程调用了线程 SVC");
    return t;
}

// ---------- 等待原语（持 G 调用）----------
// ---------- Wait primitives (called holding G) ----------
static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// 按内核顺序挑等待者：优先级数值小者优先，同优先级先登记者优先（持 G）。kind 1 按锁地址、2 按条件键、3 按地址匹配
// Pick a waiter in kernel order: numerically lower priority first, at equal priority the earlier registered first (holding G). kind 1 matches by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
static void wake(thr_t *t, int64_t res) {
    int profile_addr = g_wait_profile_enabled && t->wkind == 3;
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res;
    // 只记实际结束地址等待的 signal；时钟推进和虚假唤醒不结束等待，不算此事件。
    // Record only a signal that completes an address wait; clock advancement and spurious wakeups do not complete the wait and are not this event.
    if (profile_addr) t->addr_wake_ns = now_ns();
    pthread_cond_signal(&t->cv);
}
// 挂起直到被唤醒或超时（ns<0 无限、0 立即超时）；返回 wres 或 R_TIMEOUT
// Suspend until woken or timed out (ns<0 infinite, 0 immediate timeout); returns wres or R_TIMEOUT
static int64_t block_inner(thr_t *t, int kind, int64_t ns);
static int64_t block(thr_t *t, int kind, int64_t ns) {   // 启动时间线：按等待种类累计（全线程）+ 主线程单列 | Boot timeline: accumulated by wait kind (all threads) + the main thread listed separately
    if (!g_flag.bootprof) return block_inner(t, kind, ns);
    uint64_t t0 = now_ns(); int64_t r = block_inner(t, kind, ns); uint64_t d = now_ns() - t0;
    static const int id[] = {0, PROF_WAIT_LOCK, PROF_WAIT_CV, PROF_WAIT_ADDR, PROF_WAIT_SYNC};
    dm_prof_add(id[kind & 7], d, 0);
    if (t->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0);
    return r;
}
static int64_t block_inner(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    struct timespec dl;
    int det_wait = ns > 0 && dm_detclock_enabled();
    uint64_t guest_deadline = det_wait ? guest_ns() + (uint64_t)ns : 0;
    if (ns > 0 && !det_wait) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        int rc;
        if (det_wait) {
            uint64_t now = guest_ns();
            if (now >= guest_deadline) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
            (void)(guest_deadline - now);
            rc = pthread_cond_wait(&t->cv, &G);
        } else rc = ns > 0 ? pthread_cond_timedwait(&t->cv, &G, &dl) : pthread_cond_wait(&t->cv, &G);
        if (rc == ETIMEDOUT && !t->woke && (!det_wait || guest_ns() >= guest_deadline)) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

// ---------- 锁 ----------
// ---------- Locks ----------
static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// 释放 addr 处的锁：交给等在它上面的第一个线程，或清零（持 G）
// Release the lock at addr: hand it to the first thread waiting on it, or clear it to zero (holding G)
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// 让 t 去拿 addr 处的锁：空闲就直接拿到并唤醒，否则置等待者位并转为锁等待（持 G）
// Make t take the lock at addr: if free, it gets it directly and is woken, otherwise set the waiters bit and turn into a lock wait (holding G)
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;      // 仍在 g_wait 里，改为锁等待 | Still in g_wait, changed to a lock wait
    t->wseq = ++g_wseq;                                  // 内核把它新插进锁持有者的等待队列：同优先级排在已有锁等待者之后 | The kernel inserts it anew into the lock owner's wait queue: at equal priority it goes after the existing lock waiters
}

// ---------- 线程 ----------
// ---------- Threads ----------
extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);   // entry.S
uint32_t (*dm_thread_tick_fn)(void);   // 取当前游戏拍；diag.c 初始化时登记（单测不登记，拍号记 0） | Gets the current game tick; registered when diag.c initializes (unit tests do not register it, the tick number is recorded as 0)
void crash_thread_init(void);
void guest_thread_init(void);   // thread.c：每线程 TSD 槽（门、x18、TLS、TLR）唯一设置点 | thread.c: the single place that sets the per-thread TSD slots (gate, x18, TLS, TLR)
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self());
    crash_thread_init();
    guest_thread_init();
    pthread_mutex_lock(&G); while (!t->started) pthread_cond_wait(&t->cv, &G); pthread_mutex_unlock(&G);
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // 不返回：客体须经 ExitThread 退出 | Does not return: the guest must exit through ExitThread
    die("客体线程入口返回");
}
static void thr_free(thr_t *t) {   // 持 G；从全部线程链摘下并释放 | Holding G; unlink from the all-threads chain and free
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    pthread_cond_destroy(&t->cv); pthread_cond_destroy(&t->pcv); free(t);
}
static const char *thr_name(thr_t *t, char *buf, size_t n);
static void thread_exit(thr_t *t) {
    t->exit_ns = now_ns(); t->exit_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
    // 这里不打日志：退出发生在客体线程自己的栈上（有的只有十几 KiB），带浮点的 printf 会把栈撑爆（真机冒烟：LibcurlResolver 退出时 SIGBUS，回溯 fprintf → __dtoa）。时刻只记下来，现场导出时再打
    // No logging here: the exit happens on the guest thread's own stack (some are only a dozen or so KiB), and a printf with floating point would blow the stack (real-device smoke test: SIGBUS when LibcurlResolver exits, backtrace fprintf → __dtoa). Only the time is recorded, and it is printed later in the state dump
    pthread_mutex_lock(&G);
    t->exited = 1;
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }   // 同步等待者重查 | Synchronization waiters re-check
    pthread_setspecific(g_keythr, NULL);
    if (t->closed) thr_free(t);
    pthread_mutex_unlock(&G);
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("客体线程入口函数返回（Horizon 要求 ExitThread）"); }

// ---------- 暂停线程（SetThreadActivity / GetThreadContext3）----------
// IL2CPP 的 Boehm 垃圾回收停世界：逐个 SetThreadActivity(句柄, 暂停)、GetThreadContext3 读寄存器找根、扫栈，再恢复。原先两者都是占位成功：
// 线程照跑、现场是空的，回收器漏掉只在寄存器里的指针、提前回收还在用的对象（终结器连带释放原生资源）。现在照 Horizon 语义真停：
//   · 在 SVC 里（门帧已存全寄存器）：立即算停住，现场 = 门帧；SVC 返回前若仍在暂停就停在出口等恢复；
//   · 在跑客体代码：发 SIGUSR2，处理函数存下 ucontext 后原地等恢复（客体代码不持宿主锁，可在信号处理里取 G）；
//   · 在宿主代码里（HLE 函数等）：处理函数不停，请求方隔 100 µs 重发，直到它回到客体代码或进 SVC。
// IL2CPP's Boehm garbage collector stops the world: SetThreadActivity(handle, paused) on each thread, GetThreadContext3 to read registers for roots, scan the stack, then resume. Both used to be placeholders that reported success:
// threads kept running and the context was empty, so the collector missed pointers held only in registers and freed objects still in use (finalizers releasing native resources with them). They now really stop, as on Horizon:
//   · inside an SVC (the gate frame holds every register): stopped at once, context = the gate frame; if still paused when the SVC returns, the thread waits at the exit for the resume;
//   · running guest code: send SIGUSR2; the handler saves the ucontext and waits there for the resume (guest code holds no host locks, so the handler may take G);
//   · inside host code (HLE functions etc.): the handler does not stop it; the requester resends every 100 µs until the thread is back in guest code or enters an SVC.
module_t *module_of(const void *p);
static void on_pause_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || !atomic_load(&t->pause_req) || atomic_load(&t->svc_regs)) return;
    ucontext_t *uc = uc_; const struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    if (!module_of((void *)__darwin_arm_thread_state64_get_pc(*ss))) return;   // 在宿主代码里：不停，请求方重发 | in host code: do not stop; the requester resends
    pthread_mutex_lock(&G);
    if (t->pause_req) {
        for (int i = 0; i < 29; i++) t->ctx_x[i] = ss->__x[i];
        t->ctx_fp = __darwin_arm_thread_state64_get_fp(*ss); t->ctx_lr = __darwin_arm_thread_state64_get_lr(*ss);
        t->ctx_sp = __darwin_arm_thread_state64_get_sp(*ss); t->ctx_pc = __darwin_arm_thread_state64_get_pc(*ss); t->ctx_pstate = ss->__cpsr;
        memcpy(t->ctx_v, uc->uc_mcontext->__ns.__v, sizeof t->ctx_v); t->ctx_fpcr = uc->uc_mcontext->__ns.__fpcr; t->ctx_fpsr = uc->uc_mcontext->__ns.__fpsr;
        t->ctx_ok = 1; t->paused = 1; pthread_cond_broadcast(&t->pcv);
        while (t->pause_req) pthread_cond_wait(&t->pcv, &G);
        t->paused = 0; t->ctx_ok = 0;
    }
    pthread_mutex_unlock(&G);
}
// 门进 SVC / 出 SVC 时调（hle.c gate_dispatch） | called by the gate on SVC entry / exit (gate_dispatch in hle.c)
void thr_svc_enter(uint64_t *regs, uint64_t pc) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    // 唯一入口来自已登记的 SVC 门：entry.S 保存 800 字节，其上的门桩保存原 x29/x30；只在此边界读取完整帧。
    // The sole caller is a registered SVC gate: entry.S saves 800 bytes, followed by the stub's original x29/x30; read the full frame only at this boundary.
    if (g_wait_profile_enabled) t->svc_caller_lr = regs[101];
    t->svc_pc = pc; atomic_store(&t->svc_regs, regs);
    if (atomic_load(&t->pause_req)) { pthread_mutex_lock(&G); if (t->pause_req && !t->paused) { t->paused = 1; pthread_cond_broadcast(&t->pcv); } pthread_mutex_unlock(&G); }
}
void thr_svc_leave(void) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    pthread_mutex_lock(&G);
    while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); pthread_cond_wait(&t->pcv, &G); }
    t->paused = 0; atomic_store(&t->svc_regs, NULL);
    pthread_mutex_unlock(&G);
}
static void pause_install(void) {
    struct sigaction sa = {.sa_sigaction = on_pause_sig, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
}
// Horizon svc::ThreadContext（arm64，0x320 字节） | Horizon svc::ThreadContext (arm64, 0x320 bytes)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; __uint128_t v[32]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx_t;
_Static_assert(sizeof(hz_ctx_t) == 0x320, "ThreadContext");
static void ctx_fill(const thr_t *t, hz_ctx_t *o) {   // 持 G、t 已停住 | holding G, t is stopped
    memset(o, 0, sizeof *o);
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (r) {   // 门帧：x0..x30 在 +0、q0..q31 在 +256、nzcv / fpsr / fpcr 在 +768 / +776 / +784；进门前的栈顶 = 门帧 + 800 | gate frame: x0..x30 at +0, q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784; the stack top before the gate = frame + 800
        for (int i = 0; i < 29; i++) o->r[i] = r[i];
        o->fp = r[29]; o->lr = r[30]; o->sp = (uint64_t)r + 800; o->pc = t->svc_pc; o->pstate = (uint32_t)r[96];
        memcpy(o->v, r + 32, sizeof o->v); o->fpsr = (uint32_t)r[97]; o->fpcr = (uint32_t)r[98];
    } else if (t->ctx_ok) {
        memcpy(o->r, t->ctx_x, sizeof o->r); o->fp = t->ctx_fp; o->lr = t->ctx_lr; o->sp = t->ctx_sp; o->pc = t->ctx_pc; o->pstate = t->ctx_pstate;
        memcpy(o->v, t->ctx_v, sizeof o->v); o->fpcr = t->ctx_fpcr; o->fpsr = t->ctx_fpsr;
    }
}

// 主线程登记，返回其句柄（交给 nnosInitialize 作主线程句柄）
// Register the main thread and return its handle (passed to nnosInitialize as the main thread handle)
uint32_t svc_thread_main_init(void) {
    pthread_key_create(&g_keythr, NULL);
    pause_install();
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    t->started = 1; t->prio = 44; t->core = 0; t->mask = 7; t->pt = pthread_self(); t->mport = pthread_mach_thread_np(t->pt);
    pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
    pthread_setspecific(g_keythr, t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    return t->handle;
}

static uint64_t g_main_yield_n, g_main_yield_ns;   // 只有客体主线程写；诊断行在同一线程读 | Written only by the guest main thread; the diagnostic line reads it on the same thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    pthread_mutex_unlock(&G);
    if (count) *count = n;
    return qos;
}

// ---------- 同步对象 ----------
// ---------- Synchronization objects ----------
static int sig_obj(uint32_t h, int *bad) {    // 1=已触发（持 G） | 1 = signaled (holding G)
    unsigned i = h - 0x1000;
    if (h == H_CUR_THREAD) return 0;
    if (i == 0 || i >= MAX_H) { *bad = 1; return 0; }
    switch (g_h[i].type) {
    case O_THREAD: return ((thr_t *)g_h[i].obj)->exited;
    case O_EVENT_R: return ((event_t *)g_h[i].obj)->signaled;
    default: *bad = 1; return 0;
    }
}
static void wake_sync_waiters(void) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next; if (w->wkind == 4) wake(w, -1); w = n; }
}

static int64_t now_mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000000000ll + ts.tv_nsec; }
void dm_guest_clock_advanced(void) {
    if (!dm_detclock_enabled()) return;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_wait; t; t = t->next) if (t->wkind) pthread_cond_signal(&t->cv);
    pthread_mutex_unlock(&G);
}

// 处理线程 / 同步类 SVC；返回 1 = 已处理
// Handle thread / synchronization SVCs; returns 1 = handled
static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = 名字指针 | SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->handle == 0x1001 ? "主线程" : "?";
    uint64_t np = 0; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), t->arg + 0x1a0, 8, (vm_address_t)&np, &got) || !np ||
        vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nb, &got)) return "?";
    nb[got] = 0; return nb;
}
// 全部活着的客体线程（采样器 DM_SAMPLE=毫秒:* 用）：把 pthread 填进 out，返回个数
// All live guest threads (used by the sampler DM_SAMPLE=ms:*): fill the pthreads into out, return the count
int thr_list(pthread_t *out, int max) {
    int n = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t && n < max; t = t->all) if (!t->exited && t->started) out[n++] = t->pt;
    pthread_mutex_unlock(&G);
    return n;
}
// 浸泡读数（DM_SOAK）：句柄表占用、存活客体线程、线程对象总数（含已退出未释放）
// Soak readings (DM_SOAK): handle table usage, live guest threads, total thread objects (including exited but not yet freed)
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    pthread_mutex_lock(&G);
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    pthread_mutex_unlock(&G);
    *handles = nh; *live = nl; *objs = no;
}
// 按 pthread 取客体线程名（按线程 CPU 采样 DM_THRCPU 用，memstat.m）：不是客体线程返回 0
// Get the guest thread name by pthread (used by per-thread CPU sampling DM_THRCPU, memstat.m): returns 0 if not a guest thread
int thr_name_of(pthread_t pt, char *out, size_t n) {
    int ok = 0; char nb[64];
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (t->pt == pt && !t->exited) { snprintf(out, n, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
uint32_t thr_cur_handle(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->handle : 0; }   // 只读 TSD，信号处理里可用 | Only reads TSD, usable in a signal handler
// 按线程名找活着的客体线程（采样器用）；找不到返回 0
// Find a live guest thread by thread name (used by the sampler); returns 0 if not found
int thr_find_by_name(const char *name, pthread_t *out) {
    char nb[32]; int ok = 0;
    pthread_mutex_lock(&G);
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited && !strcmp(thr_name(t, nb, sizeof nb), name)) { *out = t->pt; ok = 1; break; }
    pthread_mutex_unlock(&G);
    return ok;
}
// 现场导出（看门狗 / 崩溃时由 crash.c 调；只读、不取锁）：每个客体线程在等什么、最近一次 SVC 从哪发出
// 现场导出时取别的客体线程的回溯：暂停 → 读寄存器 → 沿帧指针链走（客体与门块都保持 x29 链）→ 恢复；只打 main / sdk 帧
// State dump (called from crash.c on watchdog / crash; read-only, takes no lock): what each guest thread is waiting on and where its most recent SVC was issued from
// During a state dump, take the backtrace of other guest threads: suspend → read registers → walk the frame pointer chain (both guest code and gate blocks keep the x29 chain) → resume; only main / sdk frames are printed
static void thr_bt(thr_t *t) {
    if (!t->mport || t->exited || t->mport == mach_thread_self()) return;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
        // 没在等系统调用的线程（在跑客体代码，可能在空转等一个条件）：打被调用者保存寄存器 x19–x28，以及 x19 / x20 指向的内存各 128 字节。
        // 偶发「进局后不出帧」现场：主线程在作业池的「边帮忙边等」（main+0x14f08d8）里空转，x19 = 等待对象、x20 = 作业池——有这些才能看出它在等哪一批、未完成计数是几
        // Threads not waiting in a system call (running guest code, possibly spinning on a condition): print the callee-saved registers x19–x28, plus 128 bytes of the memory pointed to by each of x19 / x20.
        // The state seen in the occasional "no frames after entering a run": the main thread spins in the job pool's "help while waiting" (main+0x14f08d8), x19 = wait object, x20 = job pool -- only with these can one tell which batch it is waiting for and what the unfinished count is
        if (t->wkind == 0) { char rl[400]; int q = snprintf(rl, sizeof rl, "[dm]   %#x 寄存器 pc=", t->handle); uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc);
            q += pm ? snprintf(rl + q, sizeof rl - q, "%s+%#llx", pm->name, pc - (uint64_t)pm->base) : snprintf(rl + q, sizeof rl - q, "%#llx", pc);
            for (int i = 19; i <= 28; i++) q += snprintf(rl + q, sizeof rl - q, " x%d=%#llx", i, st.__x[i]);
            fprintf(stderr, "%s\n", rl);
            for (int i = 19; i <= 20; i++) { uint8_t raw[128]; vm_size_t g = 0; uint64_t a = st.__x[i];
                if (a < 0x100000000ull || vm_read_overwrite(mach_task_self(), a, sizeof raw, (vm_address_t)raw, &g) || g != sizeof raw) continue;
                char hx[420]; int h = 0; for (int k = 0; k < 128; k += 4) h += snprintf(hx + h, sizeof hx - h, "%08x ", *(uint32_t *)(raw + k));
                uint64_t vt = *(uint64_t *)raw; module_t *vm_ = module_of((void *)vt); char vs[64] = ""; if (vm_) snprintf(vs, sizeof vs, "（首 8 字节 = %s+%#llx）", vm_->name, vt - (uint64_t)vm_->base);
                fprintf(stderr, "[dm]   %#x x%d → %#llx 起 128 字节%s: %s\n", t->handle, i, a, vs, hx); } }
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
        // 在跑客体代码的线程：栈顶往上 1 KiB 原样打出来（外层各帧保存的寄存器在里面，事后能找回等待对象、资源地址）
        // Threads running guest code: print the 1 KiB above the stack top as is (the registers saved by the outer frames are in there, so the wait object and resource address can be recovered afterwards)
        if (t->wkind == 0 && module_of((void *)__darwin_arm_thread_state64_get_pc(st))) { uint64_t sp = __darwin_arm_thread_state64_get_sp(st), stk[128]; vm_size_t g = 0;   // pc 在客体模块里才打：在睡眠 / 让出里的线程 pc 在系统库，栈没看头 | Printed only if pc is inside a guest module: a thread in sleep / yield has its pc in a system library, and the stack is not worth looking at
            if (sp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), sp, sizeof stk, (vm_address_t)stk, &g) && g == sizeof stk)
                for (int r = 0; r < 128; r += 8) fprintf(stderr, "[dm]   %#x 栈 sp+%#05x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", t->handle, r * 8,
                        stk[r], stk[r + 1], stk[r + 2], stk[r + 3], stk[r + 4], stk[r + 5], stk[r + 6], stk[r + 7]); }
        char line[1024]; int p = snprintf(line, sizeof line, "[dm]   %#x 回溯:", t->handle), nf = 0; uint64_t wait_fp = 0;
        for (int k = 0; k < 64 && fp && nf < 20 && p < (int)sizeof line - 40; k++) {
            module_t *m = module_of((void *)lr);
            if (m) { p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, lr - (uint64_t)m->base); nf++; }
            uint64_t fr[2]; vm_size_t g2 = 0;
            if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
            if (m && !strcmp(m->name, "main") && lr - (uint64_t)m->base == 0x132db30) wait_fp = fp;   // 返回点在资源等待函数 main+0x132d988 里（它调作业池「边帮忙边等」之后）：此刻的帧指针就是它自己的 | The return point is inside the resource wait function main+0x132d988 (after it calls the job pool's "help while waiting"): the frame pointer at this moment is its own
            fp = fr[0]; lr = fr[1];
        }
        fprintf(stderr, "%s\n", line);
        // 资源等待函数 main+0x132d988（序言 sub sp,#0x50 … add x29,sp,#0x40）在自己栈底摆等待对象 {虚表, 指向事件指针的指针}（0x132db14 stp x8,x23,[sp]），
        // 所以等待对象 = 它的帧指针 −0x40；所指 −0x10 = 资源等待对象 {+0 在途作业, +8 互斥锁指针, +0x10 事件指针}。按返回点 0x132db30 定位，与线程此刻停在等待循环的哪一条指令无关。
        // 旧写法按 0x14f0934 取「上一帧」取错一层，把资源等待对象当成等待对象读，「在途作业 为空」「事件 / 锁 读不到」都是错读
        // The resource wait function main+0x132d988 (prologue sub sp,#0x50 … add x29,sp,#0x40) places the wait object {vtable, pointer to the event pointer} at the bottom of its own stack (0x132db14 stp x8,x23,[sp]),
        // so the wait object = its frame pointer −0x40; what that points to, −0x10 = the resource wait object {+0 in-flight job, +8 mutex pointer, +0x10 event pointer}. It is located by the return point 0x132db30, regardless of which instruction of the wait loop the thread is stopped on right now.
        // The old code took "the previous frame" based on 0x14f0934 and was off by one level, reading the resource wait object as the wait object; 「在途作业 为空」(in-flight job is null) and 「事件 / 锁 读不到」(event / lock unreadable) were both misreads
        if (wait_fp) { uint64_t wo[2] = {0, 0}, res[3] = {0, 0, 0}, wjob = 0; vm_size_t g = 0;
            #define RD(addr, buf) ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), sizeof(buf), (vm_address_t)(buf), &g) && g == sizeof(buf))
            #define HEX(tag, addr, nbytes) { uint32_t raw[32]; vm_size_t gg = 0; if ((addr) > 0x100000000ull && !vm_read_overwrite(mach_task_self(), (addr), (nbytes), (vm_address_t)raw, &gg) && gg == (nbytes)) { \
                char hx[300]; int h = 0; for (unsigned q = 0; q < (nbytes) / 4; q++) h += snprintf(hx + h, sizeof hx - h, "%08x ", raw[q]); fprintf(stderr, "[dm]   %#x 作业池等待：%s %#llx: %s\n", t->handle, tag, (unsigned long long)(addr), hx); } \
                else fprintf(stderr, "[dm]   %#x 作业池等待：%s %#llx 读不到\n", t->handle, tag, (unsigned long long)(addr)); }
            if (RD(wait_fp - 0x40, wo)) { module_t *vm_ = module_of((void *)wo[0]);
                fprintf(stderr, "[dm]   %#x 作业池等待：等待对象 %#llx 虚表 %s+%#llx（应为 main+0x20f2408），所指 %#llx\n", t->handle, (unsigned long long)(wait_fp - 0x40), vm_ ? vm_->name : "?", vm_ ? wo[0] - (uint64_t)vm_->base : wo[0], wo[1]);
                if (RD(wo[1] - 0x10, res)) {
                    fprintf(stderr, "[dm]   %#x 作业池等待：资源等待对象 %#llx：在途作业 %#llx（%s）、互斥锁 %#llx、事件 %#llx。判别：在途作业为空 = 信号已被某次等待消费；非空且事件未置位 = 完成信号没来；事件已置位 = 等待循环的问题\n",
                            t->handle, (unsigned long long)(wo[1] - 0x10), res[0], res[0] ? "非空" : "为空", res[1], res[2]);
                    HEX("事件本体 64 字节", res[2], 64);
                    HEX("互斥锁本体 32 字节（+8 重入计数、+0x10 持有线程对象、+0x18 锁字 = 持有者句柄）", res[1], 32);
                    // 作业对象（提交函数 main+0x147f154 填的）：+0x10 标志（取作业时按线程类别掩码过滤，0xeda190 / 0xdf8fac）、+0x20 队列里的下一个、+0x30 起依赖表（低 31 位 = 个数）、+0x60 资源、+0x68 指向事件指针的指针
                    // Job object (filled by the submit function main+0x147f154): +0x10 flags (filtered by a per-thread-class mask when taking a job, 0xeda190 / 0xdf8fac), +0x20 next in the queue, dependency table from +0x30 (low 31 bits = count), +0x60 resource, +0x68 pointer to the event pointer
                    if (res[0]) { HEX("在途作业对象 +0 起 128 字节", res[0], 128); HEX("在途作业对象 +0x80 起 128 字节", res[0] + 0x80, 128); } wjob = res[0]; } }
            // 作业池：线程正停在等待循环 main+0x14f08d8 自己的指令上时 x20 就是它；停在它调出去的函数里时 x20 不可信，不打
            // Job pool: when the thread is stopped on an instruction of the wait loop main+0x14f08d8 itself, x20 is the pool; when it is stopped in a function called from it, x20 cannot be trusted and is not printed
            { uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc); uint64_t off = pm && !strcmp(pm->name, "main") ? pc - (uint64_t)pm->base : 0;
              // 队列（0xeda190）：{+0 尾, +8 头, +0x10 个数, +0x18 锁}，节点 +0x10 标志、+0x20 下一个。顺着头走最多 8 个，看被等的作业在不在这个池里
              // Queue (0xeda190): {+0 tail, +8 head, +0x10 count, +0x18 lock}, node +0x10 flags, +0x20 next. Walk from the head for at most 8 nodes to see whether the awaited job is in this pool
              if (off >= 0x14f0900 && off < 0x14f09b8) { uint64_t qa = st.__x[20] + 0x410, nd = 0; HEX("作业队列头（池 = x20，+0x410 起 32 字节）", qa, 32); int in = 0, cnt = 0;
                  if (RD(qa + 8, &nd)) for (; nd && cnt < 8; cnt++) { uint64_t nb[5] = {0}; if (!RD(nd, nb)) break; fprintf(stderr, "[dm]   %#x 作业池等待：  队列第 %d 个 %#llx 标志 %#x%s\n", t->handle, cnt, (unsigned long long)nd, (uint32_t)nb[2], nd == wjob ? "（就是被等的作业）" : ""); in |= nd == wjob; nd = nb[4]; }
                  fprintf(stderr, "[dm]   %#x 作业池等待：被等的作业%s x20 这个池的队列前 %d 个里\n", t->handle, in ? "在" : "不在", cnt); }
              else fprintf(stderr, "[dm]   %#x 作业池等待：线程不在等待循环自己的指令上，作业池没取\n", t->handle); }
            #undef RD
            #undef HEX
        }
    }
    thread_resume(t->mport);
}
void svc_thread_dump(void) {
    static const char *kinds[] = {"运行/未在等", "锁", "条件变量", "地址", "同步对象"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    fprintf(stderr, "[dm] 句柄表占用 %d / %d，累计建线程 %llu\n", nh, MAX_H, g_next_id - 0x51);
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
        // 线程名：SDK ThreadType+0x1a0 是名字指针（ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4）；CreateThread 的 x2 = ThreadType*
        // Thread name: SDK ThreadType+0x1a0 is the name pointer (ThreadManager::SetInitialThreadNameUnsafe 0x34a2a4); x2 of CreateThread = ThreadType*
        char nb[32]; const char *nm = thr_name(t, nb, sizeof nb);
        double cpu = -1;
        if (t->mport && !t->exited) { thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS)
                cpu = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6; }
        char own[96] = ""; if (t->wkind == 1 && t->waddr) { uint32_t w = 0; vm_size_t g2 = 0;   // 锁等待：锁字 = 持有者句柄 | 0x40000000 | Lock wait: lock word = owner handle | 0x40000000
            if (!vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2)) snprintf(own, sizeof own, " 持有者 %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; vm_size_t g2 = 0;              // 条件等待：关联锁字与条件键的当前值（卡死现场缺这一项） | Condition wait: current values of the associated lock word and the condition key (the deadlock dump lacked this item)
            vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2); vm_read_overwrite(mach_task_self(), t->wkey, 4, (vm_address_t)&k, &g2);
            snprintf(own, sizeof own, " 锁字 %#x 条件键 %#llx=%#x", w, t->wkey, k); }
        uint8_t raw[64]; vm_size_t g3 = 0;                                                   // 所等对象（读写锁 / 条件变量所在结构）原始 64 字节：看计数与持有者 | Raw 64 bytes of the object being waited on (the structure holding the read-write lock / condition variable): to see the counts and the owner
        int have_raw = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) && t->waddr && !vm_read_overwrite(mach_task_self(), t->waddr & ~0xfull, 64, (vm_address_t)raw, &g3) && g3 == 64;
        char life[96]; uint64_t nowns = now_ns();
        if (t->exited) snprintf(life, sizeof life, " 建于拍 %u、退于拍 %u（%.1f 秒前）", t->born_tick, t->exit_tick, (nowns - t->exit_ns) / 1e9); else if (!t->born_ns) snprintf(life, sizeof life, " 进程启动时就在"); else snprintf(life, sizeof life, " 建于拍 %u（%.1f 秒前）", t->born_tick, (nowns - t->born_ns) / 1e9);
        fprintf(stderr, "[dm] 线程 %#x 「%s」优先级 %d 核 %d 掩码 %#llx CPU %.2fs svc %llu %s%s%s 等:%s addr=%#llx%s 最近 svc %#x lr=%s+%#llx\n", t->handle, nm, t->prio, t->core, (unsigned long long)t->mask,
                cpu, t->nsvc, t->exited ? "已退出" : "", t->started ? "" : "未启动", life, kinds[t->wkind & 7], t->waddr, own, t->last_svc,
                m ? m->name : "?", m ? t->last_lr - (uint64_t)m->base : t->last_lr);
        if (have_raw) { char hx[200]; int q = 0; for (int i = 0; i < 64; i += 4) q += snprintf(hx + q, sizeof hx - q, "%08x ", *(uint32_t *)(raw + i));
                        fprintf(stderr, "[dm]   %#x 所等对象 %#llx 起 64 字节: %s\n", t->handle, t->waddr & ~0xfull, hx); }
        thr_bt(t);
    }
    // 宿主自建线程（预取、Granny 后台、图形出图、音频等）：不在客体线程表里，按 mach 线程枚举，dladdr 符号化前 12 帧
    // Threads created by the host itself (prefetch, Granny background, graphics present, audio, etc.): not in the guest thread table, enumerated as mach threads, the first 12 frames symbolized with dladdr
    thread_act_array_t ths; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &ths, &nth) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nth; i++) {
            int guest = 0; for (thr_t *t = g_all; t; t = t->all) if (t->mport == ths[i]) { guest = 1; break; }
            if (!guest && ths[i] != mach_thread_self() && thread_suspend(ths[i]) == KERN_SUCCESS) {
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(ths[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    char line[1200]; int p = snprintf(line, sizeof line, "[dm] 宿主线程 %#x:", ths[i]);
                    uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st);
                    for (int k = 0; k < 12 && p < (int)sizeof line - 80; k++) {
                        Dl_info di; module_t *m = module_of((void *)pc);
                        if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, pc - (uint64_t)m->base);
                        else if (dladdr((void *)pc, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uint64_t)di.dli_saddr));
                        else p += snprintf(line + p, sizeof line - p, " %#llx", pc);
                        uint64_t fr[2]; vm_size_t g2 = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
                        fp = fr[0]; pc = fr[1];
                    }
                    fprintf(stderr, "%s\n", line);
                }
                thread_resume(ths[i]);
            }
            mach_port_deallocate(mach_task_self(), ths[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths);
    }
}

static int svc_thread_inner(int num, uint64_t *regs) {
    // 非门调用没有完整寄存器帧，LR 不可用，不能按门帧读取越界。
    // Non-gate calls do not provide a full register frame; LR is unavailable and must not be read beyond their arguments.
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = atomic_load(&me->svc_regs) == regs ? regs[30] : 0; me->nsvc++; } }
    if (g_flag.thr && num != 0x0B) { thr_t *me = pthread_getspecific(g_keythr); LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]); }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 句柄; x1 入口, x2 参数, x3 栈顶, w4 优先级, w5 核) | CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        // 核 -2（IdealCoreUseProcessValue）= 进程缺省理想核（本作 0 号），亲和掩码只含它——同 Horizon 内核。原先记成 -2，GetCurrentProcessorNumber 也答 -2，
        //   SMBW 的字体引擎按当前核号取每核引擎槽，拿 -2 当下标越界崩（main +0x107dbc0，SequenceBackgroundWorker 线程）
        // Core -2 (IdealCoreUseProcessValue) = the process's default ideal core (0 for this game), with an affinity mask of just that core - as in the Horizon kernel. It used to be recorded as -2 and GetCurrentProcessorNumber answered -2;
        //   SMBW's font engine picks its per-core engine slot by the current core number and crashed indexing with -2 (main +0x107dbc0, SequenceBackgroundWorker thread)
        t->prio = (int)regs[4]; t->core = (int)regs[5] >= 0 ? (int)regs[5] : 0; t->mask = 1ull << t->core;
        t->born_ns = now_ns(); t->born_tick = dm_thread_tick_fn ? dm_thread_tick_fn() : 0;
        pthread_mutex_lock(&G); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; t->all = g_all; g_all = t; pthread_mutex_unlock(&G);
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // 退出即回收宿主资源（客体经句柄等待，不 join） | Host resources are reclaimed on exit (the guest waits through the handle, no join)
        if (pthread_create(&t->pt, &a, thread_main, t)) die("pthread_create");
        pthread_attr_destroy(&a);
        if (g_flag.thr) LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->started = 1; pthread_cond_signal(&t->cv); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns)；0/-1/-2 = 各类让出 | SleepThread(x0 ns); 0/-1/-2 = the various kinds of yield
        int64_t ns = (int64_t)regs[0];
        uint64_t t0 = g_flag.bootprof ? now_ns() : 0;
        if (ns > 0 && dm_detclock_enabled()) {
            uint64_t deadline = guest_ns() + (uint64_t)ns;
            while (guest_ns() < deadline) { struct timespec ts = {0, 1000000}; nanosleep(&ts, 0); }
        } else if (ns > 0) { struct timespec ts = {ns / 1000000000, ns % 1000000000}; nanosleep(&ts, 0); }
        else {   // 让出（0 / -1 / -2）。主线程的让出次数与耗时记账：游戏渲染任务图「等全部做完」是纯轮询 + 让出（main+0x15ee898），量它占一帧多少 | Yield (0 / -1 / -2). Count and time of the main thread's yields are recorded: the game's render task graph "wait for everything to finish" is pure polling + yield (main+0x15ee898); measure how much of a frame it takes
            thr_t *me = pthread_getspecific(g_keythr);
            if (me && me->handle == 0x1001) { uint64_t y0 = now_ns(); sched_yield(); g_main_yield_ns += now_ns() - y0; g_main_yield_n++; }
            else sched_yield();
        }
        if (g_flag.bootprof) { uint64_t d = now_ns() - t0; dm_prof_add(PROF_SLEEP, d, 0); thr_t *me = pthread_getspecific(g_keythr); if (me && me->handle == 0x1001) dm_prof_add(PROF_MAIN_WAIT, d, 0); }
        return 1;
    }
    case 0x0C: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; return 1; }
    case 0x0D: { thr_t *t = h_get((uint32_t)regs[0], O_THREAD); if (t) t->prio = (int)regs[1]; regs[0] = t ? 0 : R_BAD_HANDLE; return 1; }
    case 0x0E: { thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 理想核, x2 掩码)：-2 = 用进程缺省（理想核 0、掩码只含它）、-1 = 不在乎、-3 = 不改理想核（同 Horizon 内核）；宿主只记账 | SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 = use the process default (ideal core 0, mask of just that core), -1 = don't care, -3 = leave the ideal core unchanged (as in the Horizon kernel); the host only keeps the bookkeeping
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1]; if (c >= 0) t->core = c; if (c == -2) { t->core = 0; t->mask = 1; } else if (regs[2]) t->mask = regs[2];
            // 理想核不在新掩码里（-2/-3 不改理想核、却把线程钉到别的核）：同 Horizon，理想核改到掩码里最低的核。
            // 不改的话 GetCurrentProcessorNumber 对钉在 1 / 2 号核的线程也报 0 号，按核分队列的任务系统（SMBW 的 BaseProcExecutor）会去拿 0 号核的活、1 / 2 号核的队列没人取
            // The ideal core is not in the new mask (-2/-3 keep the ideal core but pin the thread to other cores): as in Horizon, move the ideal core to the lowest core in the mask.
            // Otherwise GetCurrentProcessorNumber reports core 0 even for threads pinned to cores 1 / 2, and a job system with per-core queues (SMBW's BaseProcExecutor) takes core 0's work while nobody drains the queues of cores 1 / 2
            if (t->mask && (t->core < 0 || !(t->mask >> t->core & 1))) t->core = __builtin_ctzll(t->mask); }
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: { int c = cur()->core; regs[0] = (uint64_t)(uint32_t)(c >= 0 ? c : 0); return 1; }   // 当前核号总在 0..2（核号从不为负） | The current core number is always 0..2 (never negative)
    case 0x32: {  // SetThreadActivity(w0 线程, w1 0 可运行 / 1 暂停)：见上「暂停线程」 | SetThreadActivity(w0 thread, w1 0 runnable / 1 paused): see "Pausing threads" above
        thr_t *me = cur(); int act = (int)regs[1];
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (t == me) r = R_BAD_STATE;
        else if (act == 1) {
            atomic_store(&t->pause_req, 1);
            if (!t->started || t->exited || atomic_load(&t->svc_regs)) t->paused = 1;
            uint64_t t0 = now_ns(); int warned = 0;
            while (!t->paused && !t->exited) {
                pthread_mutex_unlock(&G); pthread_kill(t->pt, SIGUSR2); pthread_mutex_lock(&G);
                if (t->paused || t->exited) break;
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 100000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
                pthread_cond_timedwait(&t->pcv, &G, &dl);
                if (!warned && now_ns() - t0 > 1000000000ull) { warned = 1; LOG("【红】暂停线程 %#x 超过 1 秒还没停住（一直在宿主代码里？）", t->handle); }
            }
        } else { atomic_store(&t->pause_req, 0); pthread_cond_broadcast(&t->pcv); }
        pthread_mutex_unlock(&G);
        { static int n; if (n++ < 4) LOG("SetThreadActivity %#x → %s（%s）", (uint32_t)regs[0], act ? "暂停" : "恢复", r ? "失败" : "成功"); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // GetThreadContext3(x0 出参 ThreadContext*, w1 线程)：只对已停住的线程 | GetThreadContext3(x0 out ThreadContext*, w1 thread): only for stopped threads
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (!t->paused) r = R_BAD_STATE;
        else ctx_fill(t, (hz_ctx_t *)regs[0]);
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x25: { thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; return 1; }

    case 0x1A: {  // ArbitrateLock(w0 持有者句柄, x1 地址, w2 本线程标签) | ArbitrateLock(w0 owner handle, x1 address, w2 this thread's tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        pthread_mutex_lock(&G);
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2]; r = block(t, 1, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: pthread_mutex_lock(&G); lock_handoff(regs[0]); pthread_mutex_unlock(&G); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 锁地址, x1 条件键, w2 标签, x3 超时) | WaitProcessWideKeyAtomic(x0 lock address, x1 condition key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        pthread_mutex_lock(&G);
        // 顺序要紧：先置条件键 = 1，再放互斥锁。SDK 的 InternalConditionVariable::Signal 在用户态先读条件键、为 0 就不发 svc 0x1D；
        // 用户态抢互斥锁也不经过 G。原先先 lock_handoff 后置键：放锁后、置键前，别的线程可在用户态抢到锁、改完条件、读到键 0 就不唤醒，
        // 本线程随后挂上去永远等不到（并行负载下资源表读写锁 main+0x320e5e0 卡死：读者计数 0、锁空闲、写者条件键 1、写者仍在等）。
        // 挂进等待链在 block 里、仍持 G：svc 0x1D 要取 G，不会落在这之前
        // The order matters: first set the condition key = 1, then release the mutex. The SDK's InternalConditionVariable::Signal first reads the condition key in user mode, and does not issue svc 0x1D if it is 0;
        // grabbing the mutex in user mode does not go through G either. It used to do lock_handoff first and set the key after: after the release and before the key was set, another thread could grab the lock in user mode, change the condition, read key 0 and not wake anyone,
        // and this thread would then link itself in and wait forever (under parallel load the resource table read-write lock main+0x320e5e0 deadlocked: reader count 0, lock free, writer condition key 1, writer still waiting).
        // Linking into the wait chain happens inside block, still holding G: svc 0x1D has to take G, so it cannot land before this
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // 单测：紧跟放锁之后、挂链之前 | Unit test: right after releasing the lock, before linking into the wait chain
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // 仅单测 t_condvar 的负对照：还原修复前「先放锁、后置键」的顺序 | Negative control for the unit test t_condvar only: restores the pre-fix order "release the lock first, set the key after"
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, (int64_t)regs[3]);   // 超时：不重新拿锁（用户态自己拿） | Timeout: the lock is not re-acquired (user mode takes it itself)
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 条件键, w1 个数，<=0 全部) | SignalProcessWideKey(x0 condition key, w1 count, <=0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        pthread_mutex_lock(&G);
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // 按内核顺序；转为锁等待或被唤醒后不再是 kind 2 | In kernel order; once turned into a lock wait or woken it is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 地址, w1 类型, w2 值, x3 超时)：0 小于则等、1 小于则减一再等、2 相等则等 | WaitForAddress(x0 address, w1 type, w2 value, x3 timeout): 0 wait if less than, 1 decrement if less than then wait, 2 wait if equal
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        pthread_mutex_lock(&G);
        // 包括条件不满足、零超时和有限超时，均不能沿用上一次等待的唤醒时刻。
        // Failed conditions, zero timeouts and finite timeouts must not reuse the previous wait's wake time.
        if (g_wait_profile_enabled) t->addr_wake_ns = 0;
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, (int64_t)regs[3]); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 地址, w1 类型, w2 值, w3 个数)：0 唤醒、1 相等则加一并唤醒、2 相等则按等待数改值并唤醒 | SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 if equal modify the value by waiter count and wake
        uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2]; int cnt = (int)regs[3];
        pthread_mutex_lock(&G);
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // 全部唤醒（个数 ≤ 0）且有人等时减 2，不是减 1（mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual；Ryujinx、yuzu 同）。
                // 原先减 1：Unity（Engage）的作业栅栏按这个值判断作业做完没有，等待方提前放行，粒子几何作业与主线程算的个数对不上，写出界（索引写进贴图对象、写空指针崩）。
                // Waking all (count <= 0) with waiters subtracts 2, not 1 (mesosphere KAddressArbiter::SignalAndModifyByWaitingCountIfEqual; Ryujinx and yuzu agree).
                // It used to subtract 1: Unity's (Engage) job fences decide from this value whether jobs are done, waiters were let through early, and the particle geometry job disagreed with the main thread's counts and wrote out of bounds (indices into texture objects, writes to a null pointer).
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 写端, w2 读端) | CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        pthread_mutex_lock(&G); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); pthread_mutex_unlock(&G);
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(写端) / ClearEvent(任一端) / ResetSignal(读端，未触发报错) | SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        pthread_mutex_lock(&G);
        // SignalEvent 只认写端；ClearEvent 两端都认；ResetSignal 只认读端
        // SignalEvent accepts only the write end; ClearEvent accepts both ends; ResetSignal accepts only the read end
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = (num == 0x17 && h_peek(h, O_THREAD)) ? R_BAD_STATE : R_BAD_HANDLE;
                  if (r == R_BAD_HANDLE) LOG("事件 SVC %#x 句柄无效 %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 下标; x1 句柄数组, w2 个数, x3 超时) | WaitSynchronization(→w1 index; x1 handle array, w2 count, x3 timeout)
        thr_t *t = cur(); const uint32_t *hs = (const uint32_t *)regs[1]; int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        int64_t deadline = ns > 0 ? (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns()) + ns : 0;
        pthread_mutex_lock(&G);
        int64_t r; int idx = -1;
        for (;;) {
            if (t->cancel) { t->cancel = 0; r = R_CANCELLED; break; }
            int bad = 0;
            for (int i = 0; i < n && idx < 0; i++) if (sig_obj(hs[i], &bad)) idx = i;
            if (bad) { r = R_BAD_HANDLE; break; }
            if (idx >= 0) { r = 0; break; }
            int64_t left = ns < 0 ? -1 : ns == 0 ? 0 : deadline - (dm_detclock_enabled() ? (int64_t)guest_ns() : now_mono_ns());
            if (ns > 0 && left <= 0) { r = R_TIMEOUT; break; }
            if (block(t, 4, left) == R_TIMEOUT) { r = R_TIMEOUT; break; }
        }
        pthread_mutex_unlock(&G);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 线程) | CancelSynchronization(w0 thread)
        pthread_mutex_lock(&G);
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { t->cancel = 1; if (t->wkind == 4) wake(t, -1); }
        pthread_mutex_unlock(&G);
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0)：事件两端都关才释放；线程句柄关闭后槽位回收，对象在线程也退出后释放 | CloseHandle(w0): an event is freed only when both ends are closed; after a thread handle is closed the slot is recycled, and the object is freed once the thread has also exited
        pthread_mutex_lock(&G);
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (t->exited) thr_free(t);
        }
        pthread_mutex_unlock(&G);
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

// 在真实 SVC 外量墙钟，覆盖等待前争用 G、条件变量释放/重取锁和恢复调度；立即成功或轮询也计为一次调用。
// Time the real SVC boundary, including contention for G, condition-variable unlock/relock and rescheduling; immediate success and polling also count as calls.
int svc_thread(int num, uint64_t *regs) {
    if (!g_wait_profile_enabled) return svc_thread_inner(num, regs);
    int kind; int64_t requested;
    switch (num) {
        case 0x0B: requested = (int64_t)regs[0]; kind = requested > 0 ? DM_WAIT_SLEEP : DM_WAIT_YIELD; if (requested <= 0) requested = 0; break;
        case 0x1A: kind = DM_WAIT_LOCK; requested = -1; break;
        case 0x1C: kind = DM_WAIT_CV; requested = (int64_t)regs[3]; break;
        case 0x34: kind = DM_WAIT_ADDR; requested = (int64_t)regs[3]; break;
        case 0x18: kind = DM_WAIT_SYNC; requested = (int64_t)regs[3]; break;
        default: return svc_thread_inner(num, regs);
    }
    thr_t *t = cur();
    // WaitForAddress 的地址来自 x0；regs[30] 只是门返回点，客体调用者由真实门入口单独保存。
    // WaitForAddress takes its address in x0; regs[30] is only the gate return point, while the real gate entry separately saves the guest caller.
    if (kind == DM_WAIT_ADDR && atomic_load(&t->svc_regs) != regs) die("DM_THREAD_WAIT_PROFILE 地址等待诊断要求真实 SVC 门帧");
    uint64_t address = kind == DM_WAIT_ADDR ? regs[0] : 0, lr = kind == DM_WAIT_ADDR ? regs[30] : 0;
    uint64_t caller_lr = kind == DM_WAIT_ADDR ? t->svc_caller_lr : 0;
    uint64_t begin = now_ns();
    int handled = svc_thread_inner(num, regs);
    uint64_t returned = now_ns(), elapsed = returned - begin;
    dm_thread_wait_stat *s = &t->wait_profile.kind[kind];
    s->calls++; s->elapsed_ns += elapsed;
    if (requested > 0) s->requested_ns += (uint64_t)requested;
    else if (requested < 0) s->infinite_calls++;
    if (kind == DM_WAIT_ADDR && elapsed > t->wait_profile.addr_peak.elapsed_ns) {
        // 内层已重取 G 并摘链，因此 signal 写入已可见且不会再被别的线程改写；此间隔含重取锁和宿主调度，不能当作纯调度延迟。
        // The inner call reacquired G and unlinked the wait, so the signal write is visible and no other thread can change it; this interval includes lock reacquisition and host scheduling, not pure scheduling latency.
        uint64_t wake_ns = t->addr_wake_ns;
        t->wait_profile.addr_peak = (dm_thread_wait_peak){elapsed, address, lr, caller_lr,
            wake_ns != 0, wake_ns ? returned - wake_ns : 0};
    }
    return handled;
}

// ---------- 服务层的 SystemEvent ----------
// 真机：服务把内核事件的读端交给客户端，客体经 SDK nn::os 等待。宿主：在客体线程上调 SDK 自己的
// CreateSystemEvent(跨进程=true)，SDK 经 SVC 0x45 建出内核事件；记下写端句柄，之后任何线程（含宿主音频线程）都可触发。
// ---------- SystemEvent of the service layer ----------
// Real device: the service hands the read end of a kernel event to the client, and the guest waits on it through SDK nn::os. Host: on a guest thread, call the SDK's own
// CreateSystemEvent(inter-process=true), and the SDK creates the kernel event through SVC 0x45; the write-end handle is recorded, after which any thread (including the host audio thread) can signal it.
static struct { void *ev; uint32_t wh; } g_se[512];
static int g_nse;
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
void hle_bind_system_event(void *ev, int autoclear) {      // 必须在客体线程上调（要执行 SDK 代码） | Must be called on a guest thread (it has to execute SDK code)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!pthread_getspecific(g_keythr)) die("hle_bind_system_event 须在客体线程上调用");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent 失败 %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent 绑定表满");
    g_se[g_nse].ev = ev; g_se[g_nse++].wh = wh;
    pthread_mutex_unlock(&g_se_lock);
}
void hle_signal_system_event(void *ev) {   // 未绑定的按手动清除即时绑定（只能在客体线程上发生） | An unbound one is bound on the spot as manual-clear (can only happen on a guest thread)
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (!wh) { hle_bind_system_event(ev, 0); pthread_mutex_lock(&g_se_lock); wh = se_find(ev); pthread_mutex_unlock(&g_se_lock); }
    pthread_mutex_lock(&G);
    event_t *e = h_get(wh, O_EVENT_W);
    if (!e) die("SystemEvent 写端句柄失效 %#x", wh);
    e->signaled = 1; wake_sync_waiters();
    pthread_mutex_unlock(&G);
}
