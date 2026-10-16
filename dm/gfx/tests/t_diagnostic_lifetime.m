// 真实 Metal 诊断快照回归：外层自动释放池尚未排空时，不再使用的资源必须已释放。
// Real Metal diagnostic snapshot regression: unused resources must release before the outer autorelease pool drains.
#ifndef NVN_MTL_IMPLEMENTATION
#define NVN_MTL_IMPLEMENTATION "../nvn/nvn_mtl.m"
#endif
#include NVN_MTL_IMPLEMENTATION
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface DiagnosticWeak : NSObject { @public __weak id<MTLBuffer> buffer; } @end
@implementation DiagnosticWeak @end
static unsigned failures;
static void check_released(const char *stage, DiagnosticWeak *resident, DiagnosticWeak *old) {
    @autoreleasepool {
        int resident_alive = resident->buffer != nil, old_alive = old->buffer != nil;
        int ok = !resident_alive && !old_alive;
        printf("diagnostic snapshots %s: %s resident_alive=%d old_block_alive=%d\n",
               stage, ok ? "PASS" : "FAIL", resident_alive, old_alive);
        failures += !ok;
    }
}
static void run_case(const char *name, int snapshot) {
    printf("diagnostic lifetime case=%s\n", name);
    DiagnosticWeak *resident = [DiagnosticWeak new], *old = [DiagnosticWeak new];
    @autoreleasepool {
        id<MTLBuffer> resident_owner, old_owner;
        // 创建与驻留操作可能产生 Metal 内部自动释放引用，必须与被测诊断快照隔离。
        // Creation and residency operations can autorelease internal Metal references; isolate them from the diagnostic snapshot under test.
        @autoreleasepool {
            resident_owner = [g_dev newBufferWithLength:1 << 20 options:MTLResourceStorageModeShared];
            old_owner = [g_dev newBufferWithLength:2 << 20 options:MTLResourceStorageModePrivate];
            if (!resident_owner || !old_owner) abort();
            resident->buffer = resident_owner; old->buffer = old_owner;
            res_add(resident_owner); res_add(old_owner); res_commit();
            g_up_old = [NSHashTable weakObjectsHashTable];
            [g_up_old addObject:old_owner];
            // 已退役旧块只由 weak 表观察；另一个仍驻留的缓冲覆盖驻留统计快照路径。
            // Observe the retired block only through a weak table; the other resident buffer covers the residency snapshot path.
            res_remove(old_owner); res_commit();
        }
        if (g_cb) abort();
        if (snapshot) up_diag();
        @autoreleasepool {
            res_remove(resident_owner); res_commit();
            resident_owner = nil; old_owner = nil;
        }
        check_released("before outer pool drain", resident, old);
    }
    check_released("after outer pool drain", resident, old);
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool { mtl_init(); }
    printf("diagnostic lifetime device=%s\n", g_dev.name.UTF8String);
    run_case("without diagnostic snapshot", 0);
    run_case("with diagnostic snapshot", 1);
    printf("diagnostic lifetime failures=%u\n", failures);
    return failures ? 1 : 0;
} }
