// 客体内存写保护脏页跟踪：预排布贴图（CPU 写、GPU 读）的池页上传后设只读，游戏一写即缺页 → 标脏、解保护；
// 使用前查覆盖页的写代号，比上传时新就重传。NVN 语义：贴图描述符指向池内存，GPU 每次读的是当前内容
// （实测游戏会把流式素材写进仍被采样的贴图存储，不重新登记）。
// 信号处理约束：缺页路径只用原子操作与 mprotect，不分配、不加锁。
// Write-protect dirty-page tracking for guest memory: pool pages of packaged textures (CPU writes, GPU reads) are set read-only after upload, and as soon as the game writes it faults -> mark dirty, unprotect;
// before use, check the write generation of the covered pages and re-upload if newer than at upload time. NVN semantics: a texture descriptor points to pool memory, and the GPU reads the current content every time
// (measured: the game writes streamed assets into texture storage that is still being sampled, without re-registering).
// Signal handler constraint: the page-fault path uses only atomic operations and mprotect, no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>

enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2 };   // BUSY：正在解除保护（另一线程同时缺页时自旋等它） | BUSY: being unprotected (another thread faulting at the same time spins waiting for it)
// 跟踪区间 = 各内存池（MemoryPoolInitialize 登记）。池销毁后复用 live=0 的槽，信号处理里无锁扫描。
// Tracked ranges = the memory pools (registered by MemoryPoolInitialize). After a pool is destroyed its live=0 slot is reused; the signal handler scans without locks.
#define MAX_RANGE 4096   // 原为 256：内存池多于它时超出的池不跟踪，里面的贴图每次取用都当成脏的重传（暗黑实测峰值见内存读数行的「内存池」；塞尔达约 300 个，踩过）。扫描只到已用的槽数，容量大不增加开销 | was 256: with more memory pools than that the excess pools are untracked and their textures are treated as dirty and re-uploaded on every use (for Diablo's measured peak see "memory pools" in the memory readout line; Zelda has about 300 and hit this). The scan only covers slots in use, so a larger capacity costs nothing
// 区间按宿主页（16 KB）取整，客体内存池只按 4 KB 对齐：相邻的池会共用首尾页，同一个地址可能落在多个在册区间里。
// 所以：判「贴图在不在跟踪」要找能整段装下它的区间；缺页 / 宿主写前要把覆盖该页的每个区间都解保护并记代号；销池按池的确切起点 mem 认区间。
// （原先一律取第一个包含该地址的区间：SMBW 两张 ASTC 6x5 贴图的池首页与前一个小池共页，被判成未跟踪，每次取用都重传，每帧约 6 MB）
// Ranges are rounded to host pages (16 KB) but guest memory pools are only 4 KB aligned: adjacent pools share their first / last pages, so one address can fall in several registered ranges.
// So: "is this texture tracked" looks for a range that holds its whole span; a fault / host pre-write unprotects and stamps every range covering that page; pool destruction identifies its range by the pool's exact start mem.
// (Previously the first range containing the address was always taken: two SMBW ASTC 6x5 textures whose pool's first page is shared with a small preceding pool were judged untracked and re-uploaded on every use, about 6 MB per frame)
typedef struct { uintptr_t base, end, mem; _Atomic uint8_t *state; _Atomic uint64_t *gen; size_t cap; _Atomic int live; } range_t;
static range_t g_r[MAX_RANGE];
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic uint64_t g_adds_total;
static _Atomic int g_live, g_live_peak;   // 在册的池数与峰值（内存读数行用） | number of registered pools and peak (for the memory readout line)
void nvn_dirty_stat(int *live, int *peak, int *cap) { *live = atomic_load(&g_live); *peak = atomic_load(&g_live_peak); *cap = MAX_RANGE; }
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // 建池 / 销池（客体线程，可能并发）互斥选槽；缺页处理路径不碰它 | Pool creation / destruction (guest threads, possibly concurrent) pick slots under a mutex; the page-fault handling path does not touch it
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // 本模块的 mprotect：调用次数、覆盖页数、耗时（mach 时基，缺页路径里也安全） | This module's mprotect: call count, pages covered, time spent (mach timebase, safe in the page-fault path too)
static void mp(uintptr_t a, size_t len, int prot) {
    uint64_t t = mach_absolute_time(); mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t); atomic_fetch_add(&g_mp_calls, 1); atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static int live_at(int i) { return atomic_load_explicit(&g_r[i].live, memory_order_acquire); }
// 能整段装下 [a, a+len) 的在册区间（贴图的跟踪 / 保护 / 查脏都用它） | The registered range that holds all of [a, a+len) (used for a texture's tracking / protection / dirty check)
static range_t *find_span(uintptr_t a, size_t len) {
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++) if (live_at(i) && a >= g_r[i].base && a + len <= g_r[i].end) return &g_r[i];
    return NULL;
}
static void unprotect_page(range_t *r, uintptr_t p) {   // 调用方已把状态从 PROT 改成 BUSY | The caller has already changed the state from PROT to BUSY
    mp(r->base + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    atomic_store(&r->gen[p], atomic_fetch_add(&g_now, 1) + 1);
    atomic_store(&r->state[p], PG_NONE);
}
// 解开一个区间里的一页：受保护就解并记代号；别的线程正在解就等它。返回 1 = 这一页是本模块保护过的 | Unprotect one page of a range: if protected, unprotect and stamp it; if another thread is unprotecting it, wait. Returns 1 = this page was protected by this module
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    uint8_t st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], &st, PG_BUSY)) { unprotect_page(r, p); if (count) atomic_fetch_add(count, 1); return 1; }
    if (st == PG_BUSY) { while (atomic_load(&r->state[p]) == PG_BUSY) {} return 1; }
    return atomic_load(&r->gen[p]) != 0;   // 刚被别的线程解掉（缺页发生在解之前）：重执行；从未保护过的页不是我们的 | Just unprotected by another thread (the fault happened before the unprotect): re-execute; pages that were never protected are not ours
}
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr; int ours = 0, n = atomic_load(&g_nr);
    for (int i = 0; i < n; i++)   // 共页的每个区间都要解：只解一个的话，另一个区间的状态仍是「受保护」，之后的写不再缺页、被漏掉 | Every range sharing the page must be released: releasing only one leaves the other range's state at "protected", and later writes no longer fault and are missed
        if (live_at(i) && a >= g_r[i].base && a < g_r[i].end) ours |= release_page(&g_r[i], (a - g_r[i].base) >> g_shift, &g_faults);
    return ours;
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len; int n = atomic_load(&g_nr);
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = &g_r[i];
        if (!live_at(i) || e <= r->base || a >= r->end) continue;
        uintptr_t lo = a > r->base ? a : r->base, hi = e < r->end ? e : r->end;
        for (uintptr_t p = (lo - r->base) >> g_shift, q = (hi - 1 - r->base) >> g_shift; p <= q; p++) release_page(r, p, &g_preps);
    }
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    static int inited;
    if (!inited) { inited = 1; g_pg = (uintptr_t)getpagesize(); g_shift = __builtin_ctzl(g_pg); dm_fault_hook_register(on_fault); dm_guest_write_prep_register(prep_write); }
    atomic_fetch_add(&g_adds_total, 1);
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!atomic_load_explicit(&g_r[i].live, memory_order_acquire)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("【红】脏页跟踪区间已满（%d），池 %p 不跟踪", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu); return;
        }
        slot = n;
    }
    { int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak); while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {} }
    range_t *r = &g_r[slot];
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        state = calloc(np, 1); gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("脏页表分配失败（%zu 页）", np);
        // 旧数组不 free：信号处理里可能仍有极短暂的陈旧读者，泄漏量 = 每页 9 字节。
        // The old array is not freed: the signal handler may still have a very brief stale reader; leak = 9 bytes per page.
        r->cap = np;
    } else {
        memset((void *)state, 0, np); memset((void *)gen, 0, np * sizeof *gen);
    }
    r->base = base; r->end = end; r->mem = (uintptr_t)mem; r->state = state; r->gen = gen;
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    pthread_mutex_unlock(&g_slot_mu);
}
// 池销毁：先把仍受保护的页还原成可写（池内存归还游戏后可能另作他用），再标失效
// Pool destruction: first restore still-protected pages to writable (the pool memory may be reused for something else once returned to the game), then mark invalid
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL; int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && g_r[i].mem == (uintptr_t)mem) r = &g_r[i];   // 按池的确切起点认：共页的邻池取整后的 base 可能相同 | Match by the pool's exact start: an adjacent pool sharing a page may have the same rounded base
    if (r) { prep_write((void *)r->base, r->end - r->base); atomic_store_explicit(&r->live, 0, memory_order_release); atomic_fetch_sub(&g_live, 1); }
    pthread_mutex_unlock(&g_slot_mu);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// 设只读：先置状态再 mprotect（反过来会有「页已只读、状态仍为 0」的窗口，那时的合法写入会被当成崩溃）
// Set read-only: set the state first, then mprotect (the reverse leaves a window of "page already read-only, state still 0", during which a legitimate write would be treated as a crash)
void nvn_dirty_protect(const void *addr, size_t len) {
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) return;
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    // 连续置成 PROT 的页合成一次 mprotect（逐页调用在首传大贴图时一帧上千次；选角页启动帧 5：4888 次）。
    // 已是 PROT / BUSY 的页断开连续段：不重复保护，也不碰别的线程正在解的页
    // Consecutive pages set to PROT are merged into one mprotect (per-page calls reach thousands per frame on the first upload of large textures; character select page startup frame 5: 4888 calls).
    // Pages already PROT / BUSY break the consecutive run: do not protect again, and do not touch pages another thread is unprotecting
    uintptr_t run = 0; int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_PROT);
        if (took && !open) { run = i; open = 1; }
        if (!took && open) { mp(r->base + (run << g_shift), (i - run) << g_shift, PROT_READ); open = 0; }
    }
}
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    range_t *r = find_span((uintptr_t)addr, len);
    if (!r) return 1;   // 未跟踪时保守视为脏，宁可多传也不显示旧内容 | When untracked, conservatively treat as dirty: better to upload more than to show stale content
    uintptr_t a = (uintptr_t)addr, p = (a - r->base) >> g_shift, q = (a + len - 1 - r->base) >> g_shift;
    for (uintptr_t i = p; i <= q; i++) if (atomic_load(&r->gen[i]) > stamp) return 1;   // 上传前最后一次写的代号 ≤ 戳；之后的写必 > 戳 | The generation of the last write before upload is <= the stamp; any later write must be > the stamp
    return 0;
}
void nvn_dirty_slot_stats(int *live, int *used_slots, uint64_t *adds_total) {
    int n = atomic_load(&g_nr), active = 0;
    for (int i = 0; i < n; i++) active += atomic_load_explicit(&g_r[i].live, memory_order_acquire) != 0;
    *live = active; *used_slots = n; *adds_total = atomic_load(&g_adds_total);
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) { *faults = atomic_load(&g_faults); *preps = atomic_load(&g_preps); }
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls); *pages = atomic_load(&g_mp_pages); *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
