// 长寿命客体 API 线程回归：生产内存池入口在 GPU 完成后不能把缓冲留到外层池排空。
// Long-lived guest API-thread regression: production pool entry points must release buffers after GPU completion, before the outer pool drains.
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

@interface PoolAPIWeak : NSObject { @public __weak id<MTLBuffer> buffer; } @end
@implementation PoolAPIWeak @end
static unsigned failures;
enum { ITERATIONS = 8, POOL_BYTES = 65536, COPY_BYTES = 256, DESTINATION_OFFSET = 32768 };

static void wait_fence(uint64_t fence) {
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish();
    dispatch_sync(g_feedback_q, ^{});
    // 与客体线程上的生产调用相同，不为待测退役操作另设自动释放池。
    // Match production calls on the guest thread; do not add an autorelease pool around retirement under test.
    defer_drain(); res_commit();
}
static void observe_pool(NSMutableArray<PoolAPIWeak *> *observations, void *memory) {
    // 只有非拥有观测放入局部池，生产创建、销毁、退役与驻留提交全部留在调用方长寿命池中。
    // Pool only non-owning observation; production creation, finalization, retirement, and residency commit stay in the caller's long-lived pool.
    @autoreleasepool {
        void *buffer = NULL; uint64_t offset;
        if (!mtl_addr((uint64_t)(uintptr_t)memory, POOL_BYTES, &buffer, &offset) || offset) abort();
        PoolAPIWeak *box = [PoolAPIWeak new]; box->buffer = (__bridge id<MTLBuffer>)buffer;
        [observations addObject:box];
    }
}
static void check_released(const char *mode, const char *stage, NSArray<PoolAPIWeak *> *observations) {
    @autoreleasepool {
        unsigned alive = 0, resident = 0; uint64_t bytes = 0;
        pthread_mutex_lock(&g_res_mu);
        for (PoolAPIWeak *box in observations) {
            id<MTLBuffer> buffer = box->buffer;
            if (buffer) { alive++; bytes += buffer.length; resident += [g_res_objects containsObject:buffer]; }
        }
        pthread_mutex_unlock(&g_res_mu);
        printf("pool API lifetime mode=%s stage=%s: %s observed=%lu alive=%u resident=%u bytes=%llu\n",
               mode, stage, alive ? "FAIL" : "PASS", (unsigned long)observations.count, alive, resident,
               (unsigned long long)bytes);
        failures += alive != 0;
    }
}
static void run_case(int nvn_api) {
    const char *mode = nvn_api ? "NVN MemoryPool Initialize/Finalize" : "mtl_pool_add/del";
    NSMutableArray<PoolAPIWeak *> *observations = [NSMutableArray new];
    void *memory[ITERATIONS] = {0};
    @autoreleasepool {
        for (unsigned iteration = 0; iteration < ITERATIONS; iteration++) {
            if (posix_memalign(&memory[iteration], getpagesize(), POOL_BYTES)) abort();
            memset(memory[iteration], 0, POOL_BYTES);
            uint32_t expected[COPY_BYTES / sizeof(uint32_t)];
            for (unsigned word = 0; word < COPY_BYTES / sizeof(uint32_t); word++)
                expected[word] = 0x12340000u + iteration * 97u + word * 37u;
            memcpy(memory[iteration], expected, COPY_BYTES);
            NVNmemoryPool pool; NVNmemoryPoolBuilder builder;
            if (nvn_api) {
                h_nvnMemoryPoolBuilderSetDefaults(&builder);
                h_nvnMemoryPoolBuilderSetStorage(&builder, memory[iteration], POOL_BYTES);
                h_nvnMemoryPoolBuilderSetFlags(&builder, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
                if (!h_nvnMemoryPoolInitialize(&pool, &builder)) abort();
            } else mtl_pool_add(memory[iteration], POOL_BYTES);
            observe_pool(observations, memory[iteration]);
            mtl_sub_begin();
            uint64_t source = (uint64_t)(uintptr_t)memory[iteration];
            mtl_sub_copy(source, source + DESTINATION_OFFSET, COPY_BYTES);
            wait_fence(mtl_sub_commit());
            int correct = !memcmp((uint8_t *)memory[iteration] + DESTINATION_OFFSET, expected, COPY_BYTES);
            printf("pool API GPU copy mode=%s iteration=%u: %s\n", mode, iteration, correct ? "PASS" : "FAIL");
            failures += !correct;
            if (nvn_api) h_nvnMemoryPoolFinalize(&pool);
            else mtl_pool_del(memory[iteration]);
            // 销毁入口按下一主栅栏退役；真实提交推进该栅栏，再等反馈与驻留变更完成。
            // Finalization retires against the next main fence; a real submission advances it before waiting for feedback and residency updates.
            mtl_sub_begin(); wait_fence(mtl_sub_commit());
        }
        check_released(mode, "GPU complete before outer pool drain", observations);
    }
    check_released(mode, "after outer pool drain", observations);
    // backing 必须活到 Metal 零拷贝缓冲释放后；失败时保留到进程退出，避免测试制造悬空资源。
    // Backing must outlive its no-copy Metal buffer; retain it until process exit on failure to avoid creating dangling resources in the test.
    @autoreleasepool {
        for (unsigned iteration = 0; iteration < ITERATIONS; iteration++)
            if (!observations[iteration]->buffer) free(memory[iteration]);
    }
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    // 首次 begin 自行初始化 Metal，覆盖提交池必须早于初始化建立的边界。
    // Let the first begin initialize Metal itself, covering the boundary that must create the submission pool before initialization.
    state_reset();
    mtl_sub_begin(); wait_fence(mtl_sub_commit());
    printf("pool API lifetime device=%s\n", g_dev.name.UTF8String);
    run_case(0); run_case(1);
    printf("pool API lifetime failures=%u\n", failures);
    return failures ? 1 : 0;
} }
