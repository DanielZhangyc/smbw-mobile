// 真实 Metal 重传生命周期探针：比较提交内解析、逐次释放池和长寿命外层池。
// Real Metal re-upload lifetime probe: compare in-submit resolution, per-iteration pools, and a long-lived outer pool.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface UploadWeakTexture : NSObject { @public __weak id<MTLTexture> texture; } @end
@implementation UploadWeakTexture @end
@interface UploadWeakBuffer : NSObject { @public __weak id<MTLBuffer> buffer; } @end
@implementation UploadWeakBuffer @end

typedef struct { uint64_t allocated; NSUInteger alive, resident, deferred, resources; } lifetime_sample;
typedef struct { NSUInteger observed, alive, resident; uint64_t alive_bytes, resident_bytes; } staging_sample;
static unsigned failures;
static id<MTLBuffer> readback;
enum { IN_SUBMIT = 1, DESCRIPTOR = 2, MEM_STAT = 4, ASYNC_RETIRE = 8, RECREATE_TEXTURE = 16, RECREATE_POOL = 32, DISTINCT_DESCRIPTOR = 64, FINALIZE_BEFORE_RESOLVE = 128 };

// 观察自身不能把 allObjects 数组或弱引用提升后的强引用留进被测外层池。
// Observation must not leave allObjects arrays or promoted weak references in the outer pool being tested.
static lifetime_sample snapshot(NSArray<UploadWeakTexture *> *textures) {
    lifetime_sample s = {0};
    @autoreleasepool {
        pthread_mutex_lock(&g_res_mu);
        for (UploadWeakTexture *box in textures) {
            id<MTLTexture> t = box->texture;
            if (t) { s.alive++; s.resident += [g_res_objects containsObject:t]; }
        }
        s.resources = g_res_objects.count;
        pthread_mutex_unlock(&g_res_mu);
        pthread_mutex_lock(&g_deferred_mu); s.deferred = g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
        s.allocated = g_dev.currentAllocatedSize;
    }
    return s;
}
static staging_sample staging_snapshot(NSArray<UploadWeakBuffer *> *buffers) {
    staging_sample s = {.observed = buffers.count};
    @autoreleasepool {
        pthread_mutex_lock(&g_res_mu);
        for (UploadWeakBuffer *box in buffers) {
            id<MTLBuffer> buffer = box->buffer;
            if (buffer) {
                s.alive++; s.alive_bytes += buffer.length;
                if ([g_res_objects containsObject:buffer]) { s.resident++; s.resident_bytes += buffer.length; }
            }
        }
        pthread_mutex_unlock(&g_res_mu);
    }
    return s;
}
static void observe_staging_buffer(NSMutableArray<UploadWeakBuffer *> *buffers, id<MTLBuffer> buffer) {
    if (!buffer) return;
    @autoreleasepool {
        // 只在局部池内提升已有弱引用；观测记录永远只持有 wrapper，不持有缓冲。
        // Promote existing weak references only inside this local pool; observation records retain wrappers, never buffers.
        for (UploadWeakBuffer *box in buffers) if (box->buffer == buffer) return;
        UploadWeakBuffer *box = [UploadWeakBuffer new]; box->buffer = buffer; [buffers addObject:box];
    }
}
static void observe_upload_staging(NSMutableArray<UploadWeakBuffer *> *buffers) {
    @autoreleasepool {
        // 在生产 up_flush 清空 owner 之前记录本批真实暂存块，覆盖 private 块和 CPU 上传共享块。
        // Observe this batch's actual staging before production up_flush clears its owners, covering private blocks and CPU-upload shared buffers.
        observe_staging_buffer(buffers, g_up_block);
        for (id object in g_up_keep) {
            if (![object conformsToProtocol:@protocol(MTLBuffer)]) abort();
            observe_staging_buffer(buffers, object);
        }
    }
}
static void report(const char *mode, const char *stage, NSUInteger iterations, NSArray<UploadWeakTexture *> *textures,
                   NSArray<UploadWeakBuffer *> *buffers) {
    lifetime_sample s = snapshot(textures);
    staging_sample staging = staging_snapshot(buffers);
    printf("lifetime mode=%s stage=%s iterations=%lu allocated_bytes=%llu alive_textures=%lu resident_textures=%lu residency_objects=%lu deferred=%lu observed_staging_buffers=%lu alive_staging_buffers=%lu resident_staging_buffers=%lu alive_staging_bytes=%llu resident_staging_bytes=%llu\n",
        mode, stage, (unsigned long)iterations, (unsigned long long)s.allocated, (unsigned long)s.alive,
        (unsigned long)s.resident, (unsigned long)s.resources, (unsigned long)s.deferred,
        (unsigned long)staging.observed, (unsigned long)staging.alive, (unsigned long)staging.resident,
        (unsigned long long)staging.alive_bytes, (unsigned long long)staging.resident_bytes);
}
static void finish(void) {
    uint64_t fence = mtl_sub_commit();
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish();
    // 完成通知先发布 g_done，再排空退役；同步等反馈队列走完，不能把尚在执行的退役回调当成泄漏。
    // Feedback publishes g_done before draining retirement; wait for its queue so in-progress retirement is not mistaken for a leak.
    dispatch_sync(g_feedback_q, ^{});
    defer_drain(); res_commit();
}
static void iteration(NVNtexture *texture, NVNtextureBuilder *builder, NVNmemoryPool *pool, NVNmemoryPoolBuilder *pb,
                      NSMutableArray<UploadWeakTexture *> *weak_textures, NSMutableArray<UploadWeakBuffer *> *weak_buffers, unsigned i, int flags) {
    if (i && (flags & RECREATE_TEXTURE)) {
        if (!(flags & FINALIZE_BEFORE_RESOLVE)) h_nvnTextureFinalize(texture);
        if (flags & RECREATE_POOL) { h_nvnMemoryPoolFinalize(pool); if (!h_nvnMemoryPoolInitialize(pool, pb)) abort(); }
        if (!h_nvnTextureInitialize(texture, builder)) abort();
    }
    int tid = flags & DISTINCT_DESCRIPTOR ? 257 + (int)i : 257;
    if (flags & DESCRIPTOR) mtl_reg_tex(tid, (const tex_t *)texture, NULL);
    const uint8_t pixel[4] = {(uint8_t)i, (uint8_t)(i >> 8), 173, 255};
    NVNcopyRegion region = {0, 0, 0, 1, 1, 1};
    h_nvnTextureWriteTexels(texture, NULL, &region, pixel);
    // 命令保存描述快照后，API 对象可以先销毁；回放不能复活永久的序号所有权。
    // The API object may be finalized after a command snapshots it; replay must not resurrect permanent serial ownership.
    tex_t recorded = *(const tex_t *)texture;
    if (flags & FINALIZE_BEFORE_RESOLVE) h_nvnTextureFinalize(texture);
    if (flags & IN_SUBMIT) mtl_sub_begin();
    id<MTLTexture> source;
    if (flags & DESCRIPTOR) { pthread_mutex_lock(&g_rmu); source = tex_by_id(tid); pthread_mutex_unlock(&g_rmu); }
    else source = tex_mtl(flags & FINALIZE_BEFORE_RESOLVE ? &recorded : (const tex_t *)texture);
    if (!source) abort();
    UploadWeakTexture *box = [UploadWeakTexture new]; box->texture = source; [weak_textures addObject:box];
    observe_upload_staging(weak_buffers);
    if (!(flags & IN_SUBMIT)) mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), source, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(1, 1, 1), readback, 0, 4, 4);
    if (flags & ASYNC_RETIRE) {
        // 只等真实完成栅栏，立即进入下一轮 begin；让生产完成回调和 begin 自行竞争退役队列。
        // Wait only for the real completion fence, then immediately begin again; production feedback and begin race to drain retirement.
        uint64_t fence = mtl_sub_commit();
        if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    } else finish();
    if (memcmp(readback.contents, pixel, sizeof pixel)) {
        fprintf(stderr, "【红】iteration %u GPU readback differs from real WriteTexels bytes\n", i); failures++;
    }
    if ((flags & MEM_STAT) && i % 32 == 0) mtl_mem_stat((unsigned long long)(i + 1) * 3000, 0);
}
static void verify_descriptor(int tid, unsigned last_write) {
    mtl_sub_begin();
    pthread_mutex_lock(&g_rmu); id<MTLTexture> source = tex_by_id(tid); pthread_mutex_unlock(&g_rmu);
    if (!source) abort();
    enc_b_copy_texture_buffer(blit(), source, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(1, 1, 1), readback, 0, 4, 4);
    finish();
    const uint8_t expected[4] = {(uint8_t)last_write, (uint8_t)(last_write >> 8), 173, 255};
    int ok = !memcmp(readback.contents, expected, sizeof expected);
    printf("descriptor reaccess tid=%d current_bytes=%s\n", tid, ok ? "PASS" : "FAIL"); failures += !ok;
}
static void run_case(const char *mode, unsigned iterations, int flags, int per_iteration) {
    NSMutableArray<UploadWeakTexture *> *textures = [NSMutableArray new];
    NSMutableArray<UploadWeakBuffer *> *buffers = [NSMutableArray new];
    @autoreleasepool {
        NVNtextureBuilder builder = {0};
        h_nvnTextureBuilderSetDefaults(&builder);
        h_nvnTextureBuilderSetTarget(&builder, NVN_TEXTURE_TARGET_2D);
        h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBA8);
        h_nvnTextureBuilderSetSize2D(&builder, 512, 512);
        h_nvnTextureBuilderSetLevels(&builder, 1);
        NVNpackagedTextureLayout layout = {0};
        h_nvnTextureBuilderSetPackagedTextureLayout(&builder, &layout);
        size_t bytes = h_nvnTextureBuilderGetStorageSize(&builder);
        size_t storage = (bytes + getpagesize() - 1) & ~(size_t)(getpagesize() - 1);
        void *memory = NULL;
        if (posix_memalign(&memory, getpagesize(), storage)) abort();
        memset(memory, 0, storage);
        NVNmemoryPoolBuilder pb; NVNmemoryPool pool; NVNtexture texture = {0};
        h_nvnMemoryPoolBuilderSetDefaults(&pb);
        h_nvnMemoryPoolBuilderSetStorage(&pb, memory, storage);
        h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
        h_nvnTextureBuilderSetStorage(&builder, &pool, 0);
        if (!h_nvnTextureInitialize(&texture, &builder)) abort();
        uint64_t uploads = g_pkg_uploads, reuploads = g_pkg_reuploads;
        report(mode, "before", 0, textures, buffers);
        for (unsigned i = 0; i < iterations; i++) {
            if (per_iteration) { @autoreleasepool { iteration(&texture, &builder, &pool, &pb, textures, buffers, i, flags); } }
            else iteration(&texture, &builder, &pool, &pb, textures, buffers, i, flags);
            if (i == 0 || (i + 1) % 32 == 0 || i + 1 == iterations) report(mode, "fence-drained", i + 1, textures, buffers);
        }
        if (flags & DISTINCT_DESCRIPTOR) {
            // 再给生产提交入口一次清理机会；尚未再用的槽不应把已退役的整张旧底图留住。
            // Give the production submission entry another cleanup opportunity; untouched slots must not retain whole retired allocations.
            mtl_sub_begin(); finish();
            report(mode, "unused-descriptor-cache-drained", iterations, textures, buffers);
            lifetime_sample s = snapshot(textures);
            int ok = s.alive == 1;
            printf("descriptor cached retired allocation ownership: %s (alive=%lu, expected=1 current allocation)\n", ok ? "PASS" : "FAIL", (unsigned long)s.alive);
            failures += !ok;
            verify_descriptor(257, iterations - 1);
            verify_descriptor(257 + (int)(iterations / 2), iterations - 1);
            verify_descriptor(257 + (int)iterations - 1, iterations - 1);
        }
        unsigned expected_uploads = flags & RECREATE_POOL ? iterations : 1;
        unsigned expected_reuploads = flags & RECREATE_POOL ? 0 : iterations - 1;
        printf("lifetime mode=%s uploads=%llu reuploads=%llu expected_uploads=%u expected_reuploads=%u\n", mode,
            (unsigned long long)(g_pkg_uploads - uploads), (unsigned long long)(g_pkg_reuploads - reuploads), expected_uploads, expected_reuploads);
        failures += g_pkg_uploads - uploads != expected_uploads || g_pkg_reuploads - reuploads != expected_reuploads;
        if (flags & DESCRIPTOR) {
            // 未再使用的槽仍需保留描述符语义，但旧视图是否保留整个退役底图可单独观察。
            // Untouched slots keep descriptor semantics; observe separately whether cached old views keep whole retired allocations.
            for (unsigned i = 0; i < (flags & DISTINCT_DESCRIPTOR ? iterations : 1); i++) mtl_reg_tex(257 + (int)i, NULL, NULL);
        }
        if (!(flags & FINALIZE_BEFORE_RESOLVE)) h_nvnTextureFinalize(&texture);
        h_nvnMemoryPoolFinalize(&pool);
        // Finalize 的退役栅栏是下一次提交；真实空提交让它完成，而非伪造 g_done。
        // Finalize retires at the next submission fence; a real empty submission completes it without forging g_done.
        mtl_sub_begin(); finish();
        report(mode, "finalized-before-pool-drain", iterations, textures, buffers);
        if (flags & IN_SUBMIT) {
            // 此处仍在长寿命外层池内，但真实提交、反馈队列和退役都已完成；旧暂存必须全部释放。
            // Still inside the long-lived outer pool, but real submissions, feedback and retirement have finished: all old staging must be released.
            staging_sample staging = staging_snapshot(buffers);
            int ok = staging.observed >= iterations && staging.alive == 0 && staging.resident == 0;
            printf("staging lifetime before outer pool drain mode=%s: %s observed=%lu expected_at_least=%u alive=%lu resident=%lu\n", mode, ok ? "PASS" : "FAIL",
                (unsigned long)staging.observed, iterations, (unsigned long)staging.alive, (unsigned long)staging.resident);
            failures += !ok;
        }
        // 客体 backing 留到退出，避免 Metal no-copy 缓冲晚于它销毁。
        // Keep guest backing until exit so Metal no-copy buffers cannot outlive it.
    }
    @autoreleasepool { defer_drain(); res_commit(); }
    report(mode, "outer-pool-drained", iterations, textures, buffers);
    failures += snapshot(textures).alive != 0;
    failures += staging_snapshot(buffers).alive != 0;
}
int main(int argc, char **argv) { @autoreleasepool {
    unsigned iterations = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 128;
    int production_only = argc == 3 && !strcmp(argv[2], "--production");
    if (!iterations || iterations > 4096 || argc > 3 || (argc == 3 && !production_only)) { fprintf(stderr, "usage: %s [iterations:1..4096] [--production]\n", argv[0]); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init(); state_reset();
    readback = [g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    if (!readback) abort();
    res_add(readback);
    printf("lifetime device=%s iterations=%u texture_bytes=%u gpu_deswizzle=%d\n", g_dev.name.UTF8String, iterations, 512 * 512 * 4, g_nvn_deswz_gpu);
    run_case("submission-pool", iterations, IN_SUBMIT, 0);
    if (!production_only) {
        run_case("outside-submit-iteration-pool", iterations, 0, 1);
        run_case("outside-submit-outer-pool", iterations, 0, 0);
    }
    run_case("descriptor-replace-outside", iterations, IN_SUBMIT | DESCRIPTOR, 0);
    run_case("descriptor-untouched-old-slots", iterations, IN_SUBMIT | DESCRIPTOR | DISTINCT_DESCRIPTOR, 0);
    run_case("memstat-outside", iterations, IN_SUBMIT | MEM_STAT, 0);
    run_case("async-retirement", iterations, IN_SUBMIT | ASYNC_RETIRE, 0);
    run_case("texture-finalize-outside", iterations, IN_SUBMIT | RECREATE_TEXTURE, 0);
    run_case("pool-finalize-outside", iterations, IN_SUBMIT | RECREATE_TEXTURE | RECREATE_POOL, 0);
    run_case("packaged-finalized-before-replay", iterations, IN_SUBMIT | RECREATE_TEXTURE | FINALIZE_BEFORE_RESOLVE, 0);
    printf("upload lifetime correctness failures=%u; allocation and weak-reference observations above are diagnostic, without arbitrary memory thresholds\n", failures);
    return failures != 0;
} }
