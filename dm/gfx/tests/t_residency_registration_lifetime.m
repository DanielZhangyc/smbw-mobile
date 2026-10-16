// 真实 Metal 批量登记回归：分离弱表扩容与驻留登记对长寿命自动释放池的影响。
// Real Metal bulk-registration regression: separate weak-table growth from residency registration in a long-lived autorelease pool.
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

@interface RegistrationWeak : NSObject { @public __weak id<MTLBuffer> buffer; } @end
@implementation RegistrationWeak @end
enum { RESOURCES = 2048, BUFFER_BYTES = 4096 };
static unsigned failures;
static void check_released(const char *mode, const char *stage, NSArray<RegistrationWeak *> *observations, int expected_retention) {
    @autoreleasepool {
        NSUInteger alive = 0; uint64_t bytes = 0;
        for (RegistrationWeak *box in observations) {
            id<MTLBuffer> buffer = box->buffer;
            if (buffer) { alive++; bytes += buffer.allocatedSize; }
        }
        int ok = expected_retention ? alive > 0 : alive == 0;
        printf("bulk registration mode=%s stage=%s: %s expected=%s observed=%lu alive=%lu bytes=%llu\n",
               mode, stage, ok ? "PASS" : "FAIL", expected_retention ? "Foundation retention exposure" : "released",
               (unsigned long)observations.count,
               (unsigned long)alive, (unsigned long long)bytes);
        failures += !ok;
    }
}
static void run_case(int mode) {
    const char *name = mode == 0 ? "control" : mode == 1 ? "Foundation weak add only" : mode == 2 ? "production res_add" : "production res_remove";
    NSMutableArray<RegistrationWeak *> *observations = [NSMutableArray new];
    @autoreleasepool {
        NSMutableArray<id<MTLBuffer>> *owners = [NSMutableArray new];
        NSHashTable *weak_table;
        @autoreleasepool { weak_table = [NSHashTable weakObjectsHashTable]; }
        for (unsigned i = 0; i < RESOURCES; i++) {
            id<MTLBuffer> buffer;
            // 创建与非拥有观测隔离到局部池，只有被测 add 操作进入长寿命外层池。
            // Isolate creation and non-owning observation in a local pool; expose only the add operation under test to the long-lived outer pool.
            @autoreleasepool {
                buffer = [g_dev newBufferWithLength:BUFFER_BYTES options:MTLResourceStorageModeShared];
                if (!buffer) abort();
                *(uint32_t *)buffer.contents = 0x12340000u + i;
                [owners addObject:buffer];
                RegistrationWeak *box = [RegistrationWeak new]; box->buffer = buffer;
                [observations addObject:box];
                if (mode == 3) res_add(buffer);
            }
            if (mode == 1) [weak_table addObject:buffer];
            else if (mode == 2) res_add(buffer);
            buffer = nil;
        }
        if (mode == 3) {
            @autoreleasepool { res_commit(); }
            for (id<MTLBuffer> buffer in owners) res_remove(buffer);
        }
        @autoreleasepool {
            // 提交和清理本身不属于 add 扩容的被测区间，避免其临时引用污染判读。
            // Commit and cleanup are outside the add-growth operation under test; isolate their temporary references.
            if (mode >= 2) res_commit();
            if (owners.count != RESOURCES) abort();
            unsigned mismatches = 0;
            for (unsigned i = 0; i < RESOURCES; i++)
                mismatches += *(const uint32_t *)owners[i].contents != 0x12340000u + i;
            printf("bulk registration mode=%s real buffer bytes: %s count=%u\n", name,
                   mismatches ? "FAIL" : "PASS", RESOURCES);
            failures += mismatches != 0;
            if (mode == 2) { for (id<MTLBuffer> buffer in owners) res_remove(buffer); res_commit(); }
            [weak_table removeAllObjects];
            [owners removeAllObjects];
        }
        // 裸 Foundation 弱表是故意暴露扩容行为的阴性对照，生产登记与退役必须无残留。
        // The bare Foundation weak table intentionally exposes growth as a negative control; production registration and retirement must retain nothing.
        check_released(name, "before outer pool drain", observations, mode == 1);
    }
    check_released(name, "after outer pool drain", observations, 0);
}
int main(int argc, char **argv) { @autoreleasepool {
    int production_only = argc == 2 && !strcmp(argv[1], "--production-only");
    int remove_only = argc == 2 && !strcmp(argv[1], "--remove-only");
    if (argc != 1 && !production_only && !remove_only) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool { mtl_init(); state_reset(); }
    g_allocprof = 1;
    printf("bulk registration device=%s resources=%u buffer_bytes=%u\n", g_dev.name.UTF8String, RESOURCES, BUFFER_BYTES);
    run_case(0);
    if (!production_only && !remove_only) run_case(1);
    if (!remove_only) run_case(2);
    run_case(3);
    printf("bulk registration failures=%u\n", failures);
    return failures ? 1 : 0;
} }
