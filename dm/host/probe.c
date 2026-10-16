// 探针 DM_PROBE（从 d2r-mobile dm/host/diag.c 拆来，只留探针本身）：Mac 上调试器要系统授权弹窗时，用它在客体地址打印寄存器与回溯。
// Probes DM_PROBE (taken from d2r-mobile dm/host/diag.c, keeping only the probe itself): when the debugger needs a system authorization prompt on the Mac, use this to print registers and a backtrace at a guest address.
#include "dm.h"
#include <TargetConditionals.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
void crash_set_trap_hook(int (*fn)(ucontext_t *uc));

// 只读离线观测门：记录真实分配失败与返回时的 owner，不修补客体数据；每线程栈保留嵌套调用的请求参数。
// Read-only offline observation gates record actual allocation failures and the owner at return without changing guest data; a per-thread stack preserves nested request arguments.
typedef struct {
    uint64_t output, size, heap, minimum_pool_size, label, frame, caller;
    int32_t alignment;
    unsigned failed;
} alloc_trace_frame;
static __thread alloc_trace_frame g_alloc_trace[16];
static __thread unsigned g_alloc_trace_depth;
static uint64_t g_alloc_trace_main;
#if !TARGET_OS_IPHONE
static void probe_heapwalk(uint64_t h);
#endif

static void alloc_trace_heap(uint64_t heap) {
    uint64_t words[0xb0 / sizeof(uint64_t)]; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), heap, sizeof words, (vm_address_t)words, &got) || got != sizeof words) {
        LOG("【红】ALLOC_HEAP heap=%#llx snapshot_unreadable=1 bytes_read=%llu", heap, (unsigned long long)got);
        return;
    }
    // 身份只按已核对 1.2.1 ExpHeap 构造函数写入的 vtable 判定；其他 heap 的偏移值不赋予字段语义。
    // Identify only the vtable written by the verified 1.2.1 ExpHeap constructor; retain raw offsets for other heap types.
    int exp_heap = words[0] == g_alloc_trace_main + 0x3681a20;
    LOG("【红】ALLOC_HEAP heap=%#llx type=%s vtable=%#llx raw_0x28=%#llx raw_0x30=%#llx raw_0x38=%#llx raw_0x80=%#llx raw_0xa8=%#llx snapshot_unlocked=1",
        heap, exp_heap ? "sead::ExpHeap" : "unclassified", words[0], words[0x28 / 8], words[0x30 / 8], words[0x38 / 8], words[0x80 / 8], words[0xa8 / 8]);
    if (exp_heap) {
        LOG("【红】ALLOC_EXPHEAP heap=%#llx start=%#llx size=%#llx parent=%#llx", heap, words[0x28 / 8], words[0x30 / 8], words[0x38 / 8]);
#if !TARGET_OS_IPHONE
        probe_heapwalk(heap);
#endif
    }
}

static alloc_trace_frame *alloc_trace_top(void) {
    if (!g_alloc_trace_depth) die("DM_ALLOC_FAILURE_TRACE 返回门没有对应入口");
    return &g_alloc_trace[g_alloc_trace_depth - 1];
}
static void alloc_trace_label(uint64_t address, char label[64]) {
    vm_size_t got = 0;
    if (!address) { strlcpy(label, "(null)", 64); return; }
    if (vm_read_overwrite(mach_task_self(), address, 63, (vm_address_t)label, &got) || !got) {
        strlcpy(label, "(unreadable)", 64); return;
    }
    label[got] = 0;
    for (vm_size_t i = 0; i < got && label[i]; i++) if ((unsigned char)label[i] < 0x20 || (unsigned char)label[i] > 0x7e) label[i] = '?';
}
static void alloc_trace_entry(const uint64_t *r) {
    if (g_alloc_trace_depth == sizeof g_alloc_trace / sizeof *g_alloc_trace) die("DM_ALLOC_FAILURE_TRACE 嵌套超过 16 层");
    g_alloc_trace[g_alloc_trace_depth++] = (alloc_trace_frame){
        .output = r[1], .heap = r[2], .size = r[3], .alignment = (int32_t)r[4],
        .minimum_pool_size = r[6], .label = r[7], .frame = (uint64_t)r + 816,
        .caller = r[101] - g_alloc_trace_main,
    };
}
static void alloc_trace_failure(const uint64_t *r, int descriptor) {
    alloc_trace_frame *f = alloc_trace_top(); f->failed = 1;
    char label[64]; alloc_trace_label(f->label, label);
    // 0x429390 是共用失败出口：backing 分配 NULL 直接跳来，描述块失败则先调用 free 再落到这里，此时 x0 是 free 的返回值。
    // 0x429390 is a common failure exit: a NULL backing allocation branches here, while descriptor failure first frees the backing and leaves that free call's result in x0.
    // 实际对齐在入口栈 -0x100+0x38；门帧 +808 保存的客体 LR 可区分分配返回与回滚返回。
    // Actual alignment is at entry SP-0x100+0x38; the guest LR at gate frame+808 distinguishes allocation and rollback returns.
    int32_t alignment = 8;
    uint64_t request = r[27], caller = r[101] - g_alloc_trace_main;
    const char *stage = descriptor ? "descriptor" : caller == 0x429390 ? "backing_freed_after_descriptor_failure" : caller == 0x428a90 && !r[0] ? "backing" : "common_failure";
    const char *event = !descriptor && caller == 0x429390 ? "ALLOC_ROLLBACK" : "ALLOC_FAIL";
    if (!descriptor) memcpy(&alignment, (const void *)(f->frame - 0x100 + 0x38), sizeof alignment);
    else if (caller == 0x428b00) request = r[19] + 0x278;
    else if (caller == 0x4290d8) request = 0x180;
    else if (caller == 0x429200) request = 0x190;
    else die("DM_ALLOC_FAILURE_TRACE 未知描述块返回点 main+%#llx", caller);
    LOG("【红】%s label=%s stage=%s return=%#llx last_call_return=main+%#llx request=%#llx align=%d heap=%#llx output=%#llx original_size=%#llx original_align=%d backing_heap=%#llx minimum_pool_size=%#llx",
        event, label, stage, r[0], caller, request, alignment,
        r[25], f->output, f->size, f->alignment, f->heap, f->minimum_pool_size);
    alloc_trace_heap(r[25]);
}
static void alloc_trace_common_failure(const uint64_t *r) { alloc_trace_failure(r, 0); }
static void alloc_trace_descriptor_fail(const uint64_t *r) { alloc_trace_failure(r, 1); }
static void alloc_trace_return(const uint64_t *r) {
    alloc_trace_frame *f = alloc_trace_top();
    if (!(r[0] & 1) || f->failed) {
        uint64_t fields[2]; vm_size_t got = 0;
        char label[64]; alloc_trace_label(f->label, label);
        if (vm_read_overwrite(mach_task_self(), f->output + 8, sizeof fields, (vm_address_t)fields, &got) || got != sizeof fields)
            die("DM_ALLOC_FAILURE_TRACE 无法读取 output=%#llx 的 data/owner", f->output);
        LOG("【红】ALLOC_RETURN label=%s caller=main+%#llx result=%u request=%#llx align=%d heap=%#llx minimum_pool_size=%#llx output=%#llx data=%#llx owner=%#llx before_BufferInitialize=1",
            label, f->caller, (unsigned)r[0], f->size, f->alignment, f->heap, f->minimum_pool_size, f->output, fields[0], fields[1]);
    }
    g_alloc_trace_depth--;
}
static void alloc_trace_install(void) {
    const char *e = getenv("DM_ALLOC_FAILURE_TRACE"); if (!e || strcmp(e, "1")) return;
    if (strcmp(dm_main_build(), "ff773e90972d544eb79406eaa65396d5")) die("DM_ALLOC_FAILURE_TRACE 未核对 main 构建 %s", dm_main_build());
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) g_alloc_trace_main = (uint64_t)g_mod[i].base;
    for (int id = OBS_ALLOC_ENTRY; id <= OBS_ALLOC_DESCRIPTOR_FAIL; id++)
        if (!gate_obs_present(id)) die("DM_ALLOC_FAILURE_TRACE 缺观测门 %d；请用同名开关重新改写 main", id);
    gate_obs_set(OBS_ALLOC_ENTRY, alloc_trace_entry); gate_obs_set(OBS_ALLOC_RETURN, alloc_trace_return);
    gate_obs_set(OBS_ALLOC_BACKING_FAIL, alloc_trace_common_failure); gate_obs_set(OBS_ALLOC_DESCRIPTOR_FAIL, alloc_trace_descriptor_fail);
    LOG("DM_ALLOC_FAILURE_TRACE：1.2.1 main 分配入口/失败/返回门已启用");
}
#if !TARGET_OS_IPHONE
// 写法 模块:偏移[@x0 偏移]。在客体地址换上 brk，命中时打印 x0..x3（及其指向的可读字符串）与回溯，原指令放进跳板槽执行后跳回。
// 跳板借用 x16：只在函数入口或 x16 已死的点下探针；原指令不能是 PC 相对类（adr/adrp/b/bl/ldr 字面量/cbz…）。
// Syntax module:offset[@x0 offset]. Put a brk at the guest address; on hit print x0..x3 (and the readable strings they point to) and a backtrace, then execute the original instruction from a trampoline slot and jump back.
// The trampoline borrows x16: place probes only at function entries or at points where x16 is dead; the original instruction must not be PC-relative (adr/adrp/b/bl/ldr literal/cbz...).
#define MAX_PROBE 16
typedef void (*probe_fn)(ucontext_t *uc);
static struct { uint64_t at; uint32_t *slot; unsigned hits; char name[40]; long dump; probe_fn fn; int freg; long foff; uint64_t fval; int hreg; } g_probe[MAX_PROBE];

// !xN：把 xN 当 sead::ExpHeap，打印空闲块与已用块（SMBW 布局：空闲表 +0xb0、节点偏移 +0xc4；已用表 +0xc8、节点偏移 +0xdc；块大小在块 +0x18）
// !xN: treat xN as a sead::ExpHeap and print its free and used blocks (SMBW layout: free list +0xb0, node offset +0xc4; used list +0xc8, node offset +0xdc; block size at block +0x18)
static int rd64(uint64_t a, uint64_t *v) { vm_size_t g = 0; return !vm_read_overwrite(mach_task_self(), a, 8, (vm_address_t)v, &g) && g == 8; }
static void probe_heapwalk(uint64_t h) {
    uint64_t start = 0, size = 0;
    if (!rd64(h + 0x28, &start) || !rd64(h + 0x30, &size)) {
        fprintf(stderr, "[dm]   ExpHeap %#llx header_unreadable=1\n", (unsigned long long)h);
        return;
    }
    fprintf(stderr, "[dm]   ExpHeap %#llx 起 %#llx 大小 %#llx\n", (unsigned long long)h, (unsigned long long)start, (unsigned long long)size);
    static const struct { int list, off; const char *nm; } L[2] = { { 0xb0, 0xc4, "空闲/free" }, { 0xc8, 0xdc, "已用/used" } };
    for (int k = 0; k < 2; k++) {
        uint64_t offw = 0, node = 0, sum = 0, largest = 0; int n = 0;
        int readable = rd64(h + L[k].off, &offw) && rd64(h + L[k].list + 8, &node);
        int32_t off = (int32_t)offw;
        fprintf(stderr, "[dm]   %s:", L[k].nm);
        while (readable && node && node != h + L[k].list && n < 2000) {
            uint64_t blk = node - off, bsz = 0;
            if (!rd64(blk + 0x18, &bsz)) { readable = 0; break; }
            fprintf(stderr, "%s%llx+%llx", n % 8 ? " " : "\n[dm]     ", (unsigned long long)(blk - start), (unsigned long long)bsz);
            sum += bsz; if (bsz > largest) largest = bsz; n++;
            readable = rd64(node + 8, &node);
        }
        fprintf(stderr, "\n[dm]   %s 共 %d 块 %#llx 字节 largest_block=%#llx walk_complete=%d read_failed=%d\n",
            L[k].nm, n, (unsigned long long)sum, (unsigned long long)largest, readable && node == h + L[k].list, !readable);
    }
}   // dump：命中时打印 [x0+dump] 16 字节（-1 = 不打）；fn：专用处理（替代缺省打印） | dump: on hit print 16 bytes at [x0+dump] (-1 = do not print); fn: dedicated handler (replaces the default print)
static int g_nprobe;
static void probe_str(uint64_t p) {
    char buf[80]; vm_size_t got = 0;
    if (p < 0x1000 || vm_read_overwrite(mach_task_self(), p, sizeof buf - 1, (vm_address_t)buf, &got) || !got) return;
    buf[got] = 0; int n = 0; while (buf[n] >= 0x20 && buf[n] < 0x7f) n++;
    if (n >= 3 && (buf[n] == 0 || n == (int)got)) { fprintf(stderr, " \"%.*s\"", n, buf); return; }
    // 游戏字符串对象 {char* 指针, u64 长度, u64 容量|最高位}（0xdf67f0 FJSONUtils 的路径参数即此布局）
    // Game string object {char* pointer, u64 length, u64 capacity|top bit} (the path argument of 0xdf67f0 FJSONUtils has exactly this layout)
    if (got >= 24) {
        uint64_t ptr, len; memcpy(&ptr, buf, 8); memcpy(&len, buf + 8, 8);
        char t[96]; got = 0;
        if (len >= 3 && len < sizeof t && !vm_read_overwrite(mach_task_self(), ptr, len, (vm_address_t)t, &got) && got == len) {
            int k = 0; while (k < (int)len && t[k] >= 0x20 && t[k] < 0x7f) k++;
            if (k == (int)len) fprintf(stderr, " S\"%.*s\"", (int)len, t);
        }
    }
}
// DM_PROBE_DUMP=模块:偏移:长度[:文件]：任一探针首次命中时把该段客体内存写成文件（缺省 run/platform/probe_dump.bin）
// DM_PROBE_DUMP=module:offset:length[:file]: on the first hit of any probe, write that range of guest memory to a file (default run/platform/probe_dump.bin)
static void probe_dump_once(void) {
    static int done; const char *e = getenv("DM_PROBE_DUMP"); if (done || !e) return; done = 1;
    char spec[512]; strlcpy(spec, e, sizeof spec);
    char *mod = strtok(spec, ":"), *o = strtok(NULL, ":"), *n = strtok(NULL, ":"), *f = strtok(NULL, ":");
    module_t *m = NULL; for (int i = 0; i < g_nmod; i++) if (mod && !strcmp(g_mod[i].name, mod)) m = &g_mod[i];
    if (!m || !o || !n) { LOG("DM_PROBE_DUMP 格式：模块:偏移:长度[:文件]"); return; }
    char def[512]; snprintf(def, sizeof def, "%s%s/run/platform/probe_dump.bin", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work");
    FILE *fp = fopen(f ? f : def, "wb"); if (!fp) return;
    fwrite(m->base + strtoull(o, 0, 16), 1, strtoull(n, 0, 16), fp); fclose(fp);
    LOG("探针转储 %s+%s 长 %s → %s", mod, o, n, f ? f : def);
}
static int probe_hit(ucontext_t *uc) {   // 由 probe_install 注册给 crash.c 的 SIGTRAP 分支 | Registered by probe_install with the SIGTRAP branch of crash.c
    uint64_t pc = uc->uc_mcontext->__ss.__pc;
    for (int i = 0; i < g_nprobe; i++) if (g_probe[i].at == pc) {
        probe_dump_once();
        if (g_probe[i].fn) { g_probe[i].hits++; g_probe[i].fn(uc); uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        if (g_probe[i].freg >= 0) {   // ?xN=值 / ?[xN+偏移]=值：不符就静默放行 | ?xN=value / ?[xN+offset]=value: pass through silently when it does not match
            uint64_t v = uc->uc_mcontext->__ss.__x[g_probe[i].freg]; vm_size_t g = 0;
            if (g_probe[i].foff >= 0 && vm_read_overwrite(mach_task_self(), v + g_probe[i].foff, 8, (vm_address_t)&v, &g)) v = ~g_probe[i].fval;
            if (v != g_probe[i].fval) { uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot; return 1; }
        }
        { extern uint64_t g_t0_ns; uint64_t now_ns(void); uint32_t thr_cur_handle(void); fprintf(stderr, "[dm] PROBE %s #%u [%.4fs] T=%x", g_probe[i].name, ++g_probe[i].hits, (now_ns() - g_t0_ns) / 1e9, thr_cur_handle()); }
        for (int r = 0; r < 8; r++) { uint64_t v = uc->uc_mcontext->__ss.__x[r]; fprintf(stderr, " x%d=%#llx", r, v); probe_str(v); }
        for (int r = 19; r < 24; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);
        if (getenv("DM_PROBE_ALL")) for (int r = 9; r < 19; r++) fprintf(stderr, " x%d=%#llx", r, uc->uc_mcontext->__ss.__x[r]);   // DM_PROBE_ALL=1：再打 x9–x18 | also print x9–x18
        fprintf(stderr, " lr=%#llx", uc->uc_mcontext->__ss.__lr);
        { uint64_t x8 = uc->uc_mcontext->__ss.__x[8]; Dl_info di; fprintf(stderr, " x8=%#llx", x8);   // x8：间接调用目标常在这里（blr x8 前下探针） | x8: indirect call targets are often here (probe placed before blr x8)
          if (dladdr((void *)x8, &di) && di.dli_sname) fprintf(stderr, "（%s+%#lx）", di.dli_sname, (unsigned long)(x8 - (uint64_t)di.dli_saddr)); }
        if (g_probe[i].dump >= 0) {
            uint8_t b[16]; vm_size_t got = 0;
            int dr = (g_probe[i].dump >> 25) & 31;                                    // @xN+偏移：以 xN 为基址 | @xN+offset: based on xN
            uint64_t at = uc->uc_mcontext->__ss.__x[dr] + (g_probe[i].dump & 0xffffff);
            if (g_probe[i].dump & 0x1000000) { uint64_t p = 0; vm_size_t g = 0; if (!vm_read_overwrite(mach_task_self(), at, 8, (vm_address_t)&p, &g)) at = p; else at = 0; }   // @*偏移：先解引用再转储 | @*offset: dereference first, then dump
            if (at && !vm_read_overwrite(mach_task_self(), at, 16, (vm_address_t)b, &got) && got == 16) {
                fprintf(stderr, " [%sx%d+%#lx]=", (g_probe[i].dump & 0x1000000) ? "*" : "", dr, g_probe[i].dump & 0xffffff); for (int k = 0; k < 16; k++) fprintf(stderr, "%02x", b[k]); }
        }
        fputc('\n', stderr);
        { static long xd = -2; if (xd == -2) { const char *e = getenv("DM_PROBE_X2"); xd = e ? strtol(e, 0, 16) : -1; }   // DM_PROBE_X2=字节数：另打 x2 指向的内存（看对象字段） | DM_PROBE_X2=bytes: also print the memory x2 points to (to read object fields)
          if (xd > 0) { uint64_t at = uc->uc_mcontext->__ss.__x[2]; uint8_t b[512]; vm_size_t got = 0; long n = xd > 512 ? 512 : xd;
            if (!vm_read_overwrite(mach_task_self(), at, n, (vm_address_t)b, &got)) { fprintf(stderr, "[dm]   [x2]:"); for (long k = 0; k < (long)got; k += 8) { uint64_t v; memcpy(&v, b + k, 8); fprintf(stderr, "%s%llx", k % 32 ? " " : "\n[dm]   ", (unsigned long long)v); } fputc('\n', stderr); } } }
        if (g_probe[i].hreg >= 0) probe_heapwalk(uc->uc_mcontext->__ss.__x[g_probe[i].hreg]);
        dm_backtrace(uc->uc_mcontext->__ss.__fp);
        uc->uc_mcontext->__ss.__pc = (uint64_t)g_probe[i].slot;
        return 1;
    }
    return 0;
}
static uint32_t *g_slots; static int g_slots_sealed;
static void probe_add(module_t *m, uint64_t off, long dump, probe_fn fn) {
    if (g_nprobe >= MAX_PROBE) die("探针过多");
    if (!g_slots) g_slots = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint64_t at = (uint64_t)m->base + off;
    uint32_t *w = (uint32_t *)at, *sl = g_slots + g_nprobe * 8;
    sl[0] = *w;                                  // 原指令 | Original instruction
    sl[1] = 0x58000050;                          // ldr x16, #8
    sl[2] = 0xD61F0200;                          // br x16
    *(uint64_t *)(sl + 3) = at + 4;
    uint64_t pg = at & ~0x3FFFull;
    if (mprotect((void *)pg, 0x4000, PROT_READ | PROT_WRITE)) die("probe mprotect");
    *w = 0xD43E0000;                             // brk #0xf000
    mprotect((void *)pg, 0x4000, PROT_READ | PROT_EXEC);
    sys_icache_invalidate(w, 4);
    g_probe[g_nprobe].at = at; g_probe[g_nprobe].slot = sl; g_probe[g_nprobe].dump = dump; g_probe[g_nprobe].fn = fn; g_probe[g_nprobe].freg = -1; g_probe[g_nprobe].hreg = -1;
    snprintf(g_probe[g_nprobe].name, sizeof g_probe[0].name, "%s+%#llx", m->name, off);
    LOG("探针 %s（原指令 %08x）", g_probe[g_nprobe].name, sl[0]);
    g_nprobe++;
}

#endif

void probe_install(void) {
    alloc_trace_install();
    storage_trace_install();
#if !TARGET_OS_IPHONE
    crash_set_trap_hook(probe_hit);
    const char *e = getenv("DM_PROBE");
    if (e) {
        char spec[512]; strlcpy(spec, e, sizeof spec);
        for (char *t = strtok(spec, ","); t; t = strtok(NULL, ",")) {
            char *c = strchr(t, ':'); if (!c) die("DM_PROBE 格式：模块:偏移");
            *c = 0; module_t *m = NULL;
            for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, t)) m = &g_mod[i];
            if (!m) die("DM_PROBE 无此模块 %s", t);
            char *at_ = strchr(c + 1, '@');
            long dump = -1;
            if (at_) {   // @偏移 / @*偏移 / @xN+偏移 / @*xN+偏移 | @offset / @*offset / @xN+offset / @*xN+offset
                const char *q = at_ + 1; long deref = 0, reg = 0;
                if (*q == '*') { deref = 0x1000000; q++; }
                if (*q == 'x') { char *e2; reg = strtol(q + 1, &e2, 10) & 31; q = *e2 == '+' ? e2 + 1 : e2; }
                dump = deref | (reg << 25) | (strtol(q, 0, 16) & 0xffffff);
            }
            char *qm = strchr(c + 1, '?'), *bang = strchr(c + 1, '!');
            probe_add(m, strtoull(c + 1, 0, 16), dump, NULL);
            if (bang && bang[1] == 'x') g_probe[g_nprobe - 1].hreg = (int)strtol(bang + 2, 0, 10) & 31;
            if (qm) {   // 过滤：?x0=706c400000 或 ?[x0+38]=781548（十六进制） | Filter: ?x0=706c400000 or ?[x0+38]=781548 (hex)
                const char *q = qm + 1; int br = *q == '[';
                if (br) q++;
                if (*q != 'x') die("DM_PROBE 过滤格式：?xN=值 或 ?[xN+偏移]=值");
                char *e2; g_probe[g_nprobe - 1].freg = (int)strtol(q + 1, &e2, 10) & 31; g_probe[g_nprobe - 1].foff = -1;
                if (br) { g_probe[g_nprobe - 1].foff = *e2 == '+' ? strtol(e2 + 1, &e2, 16) : 0; if (*e2 == ']') e2++; }
                if (*e2 != '=') die("DM_PROBE 过滤缺 =");
                g_probe[g_nprobe - 1].fval = strtoull(e2 + 1, 0, 16);
            }
        }
    }
    if (g_slots && !g_slots_sealed) { mprotect(g_slots, 0x4000, PROT_READ | PROT_EXEC); sys_icache_invalidate(g_slots, 0x4000); g_slots_sealed = 1; }
#endif
}
