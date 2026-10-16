#include "../nvn/nvn_dirty.h"
#include "../../host/dm.h"
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

module_t g_mod[MAX_MOD];
int g_nmod;
module_t *module_of(const void *p) { (void)p; return NULL; }
void crash_install(void);

static int g_fail;
#define EXPECT(name, want, got) do { \
    long long want_ = (long long)(want), got_ = (long long)(got); \
    printf("%-52s 期望 %-4lld → 真结果 %-4lld %s\n", name, want_, got_, want_ == got_ ? "✓" : "✗"); \
    if (want_ != got_) g_fail++; \
} while (0)

static void *page_map(void) {
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); exit(2); }
    return p;
}

int main(void) {
    crash_install();

    for (int i = 0; i < 65; i++) {
        void *p = page_map();
        nvn_dirty_add_range(p, 4096);
        nvn_dirty_remove_range(p);
        munmap(p, 4096);
    }

    void *reused = page_map();
    nvn_dirty_add_range(reused, 4096);
    nvn_dirty_protect(reused, 4096);
    uint64_t stamp = nvn_dirty_now();
    ((volatile uint8_t *)reused)[0] = 1;
    EXPECT("累计 65 次销池后新池写入被判脏", 1, nvn_dirty_since(reused, 4096, stamp));
    nvn_dirty_remove_range(reused);
    munmap(reused, 4096);

    void *live[300];
    for (int i = 0; i < 300; i++) {
        live[i] = page_map();
        nvn_dirty_add_range(live[i], 4096);
    }
    void *overflow = live[299];
    nvn_dirty_protect(overflow, 4096);
    stamp = nvn_dirty_now();
    ((volatile uint8_t *)overflow)[0] = 2;
    EXPECT("同时存活超过上限的池保守判脏", 1, nvn_dirty_since(overflow, 4096, stamp));
    for (int i = 0; i < 300; i++) {
        nvn_dirty_remove_range(live[i]);
        munmap(live[i], 4096);
    }

    // 相邻的池共用宿主页（池 4 KB 对齐，宿主页 16 KB）：SMBW 的 ASTC 贴图池首页与前一个小池共页，曾被判成未跟踪、每次取用都重传
    // Adjacent pools sharing a host page (pools are 4 KB aligned, host pages 16 KB): the first page of SMBW's ASTC texture pool is shared with a small preceding pool, and was judged untracked and re-uploaded on every use
    size_t pg = (size_t)getpagesize();
    uint8_t *blk = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (blk == MAP_FAILED) { perror("mmap"); exit(2); }
    uint8_t *pa = blk, *pb = blk + pg - 0x1000;           // 池 A 占首页前部，池 B 从首页最后 4 KB 起 | pool A covers the start of page 0, pool B starts in the last 4 KB of page 0
    size_t na = pg - 0x1000, nb = 2 * pg, tex = pg + 0x800;   // B 里的贴图跨共用页与下一页 | the texture in B spans the shared page and the next one
    nvn_dirty_add_range(pa, na);
    nvn_dirty_add_range(pb, nb);
    EXPECT("共页的后一个池里的贴图在跟踪", 1, nvn_dirty_tracked(pb, tex));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    EXPECT("共页贴图保护后没人写：不脏", 0, nvn_dirty_since(pb, tex, stamp));
    pb[0x10] = 3;
    EXPECT("游戏写共页里 B 的部分：B 的贴图脏", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    pa[0x10] = 4;   // 同一物理页，A 的部分：页已只读，缺页要由 B 的区间认领，否则当成崩溃 | same physical page, A's part: the page is read-only, the fault must be claimed through B's range or it is treated as a crash
    EXPECT("游戏写共页里 A 的部分：B 的贴图保守判脏", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_protect(pb, tex);
    stamp = nvn_dirty_now();
    dm_guest_write_prepare(pa, 0x20); pa[0x20] = 5;   // 宿主替 GPU 写 A：共页也要解开 B 的保护 | the host writes A on behalf of the GPU: the shared page must also release B's protection
    EXPECT("宿主写前解保护共页：B 的贴图脏", 1, nvn_dirty_since(pb, tex, stamp));
    nvn_dirty_remove_range(pb);
    EXPECT("销 B 后 A 仍在跟踪（按池起点认区间）", 1, nvn_dirty_tracked(pa, na));
    EXPECT("销 B 后 B 不再跟踪", 0, nvn_dirty_tracked(pb, tex));
    nvn_dirty_remove_range(pa);
    munmap(blk, 4 * pg);
    return g_fail != 0;
}
