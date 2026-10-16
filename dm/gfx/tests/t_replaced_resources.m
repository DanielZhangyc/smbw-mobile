// 真实 Metal 替换生命周期回归：变值填充图案和泛光子资源视图必须按栅栏退役。
// Real Metal replacement lifetime regressions: changing fill patterns and bloom subresource views retire by fence.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface ReplacementWeak : NSObject { @public __weak id object; } @end
@implementation ReplacementWeak @end
static unsigned failures;
enum { ITERATIONS = 8 };

static void finish(void) {
    uint64_t fence = mtl_sub_commit();
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
}
static void check_lifetime(const char *name, NSArray<ReplacementWeak *> *objects, NSUInteger expected) {
    @autoreleasepool {
        NSUInteger alive = 0, resident = 0;
        pthread_mutex_lock(&g_res_mu);
        for (ReplacementWeak *box in objects) { id object = box->object; if (object) { alive++; resident += [g_res_objects containsObject:object]; } }
        pthread_mutex_unlock(&g_res_mu);
        int ok = alive == expected && resident == expected;
        printf("%s: %s alive=%lu resident=%lu expected=%lu\n", name, ok ? "PASS" : "FAIL", (unsigned long)alive, (unsigned long)resident, (unsigned long)expected);
        failures += !ok;
    }
}
static void initialize_pool(NVNmemoryPool *pool, void **memory, size_t bytes) {
    if (posix_memalign(memory, getpagesize(), bytes)) abort();
    memset(*memory, 0, bytes);
    NVNmemoryPoolBuilder builder;
    h_nvnMemoryPoolBuilderSetDefaults(&builder);
    h_nvnMemoryPoolBuilderSetStorage(&builder, *memory, bytes);
    h_nvnMemoryPoolBuilderSetFlags(&builder, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(pool, &builder)) abort();
}
static void fill_patterns(void) {
    NVNmemoryPool pool; void *memory;
    initialize_pool(&pool, &memory, 65536);
    NSMutableArray<ReplacementWeak *> *patterns = [NSMutableArray array];
    mtl_sub_begin();
    for (unsigned i = 0; i < ITERATIONS; i++) { @autoreleasepool {
        uint32_t value = 0x12345678u + i;
        mtl_sub_fill((uint64_t)memory + i * 16, 16, value);
        ReplacementWeak *box = [ReplacementWeak new];
        pthread_mutex_lock(&g_res_mu);
        for (id object in g_res_objects) if ([object conformsToProtocol:@protocol(MTLBuffer)]) {
            id<MTLBuffer> buffer = object;
            if (buffer.length == 65536 && buffer.contents && *(const uint32_t *)buffer.contents == value) box->object = buffer;
        }
        pthread_mutex_unlock(&g_res_mu);
        if (!box->object) abort();
        [patterns addObject:box];
    } }
    finish();
    unsigned bad = 0;
    for (unsigned i = 0; i < ITERATIONS; i++) for (unsigned word = 0; word < 4; word++) bad += ((uint32_t *)memory)[i * 4 + word] != 0x12345678u + i;
    printf("fill GPU bytes: %s mismatches=%u\n", bad ? "FAIL" : "PASS", bad); failures += bad != 0;
    check_lifetime("replaced fill patterns after GPU completion", patterns, 1);
    h_nvnMemoryPoolFinalize(&pool); mtl_sub_begin(); finish();
    // no-copy 客体存储留到进程退出，避免宿主缓冲晚于 backing 释放。
    // Keep no-copy guest backing until process exit so host buffers cannot outlive it.
}
static void initialize_texture(NVNtexture *texture, NVNmemoryPool *pool, ptrdiff_t offset, int levels) {
    NVNtextureBuilder builder;
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&builder, 8, 8);
    h_nvnTextureBuilderSetLevels(&builder, levels);
    h_nvnTextureBuilderSetStorage(&builder, pool, offset);
    if (!h_nvnTextureInitialize(texture, &builder)) abort();
}
static void bloom_views(void) {
    NVNmemoryPool pool; void *memory;
    initialize_pool(&pool, &memory, 65536);
    NVNtexture source = {0}, destinations[ITERATIONS] = {0};
    initialize_texture(&source, &pool, 0, 1);
    for (int i = 0; i < ITERATIONS; i++) initialize_texture(&destinations[i], &pool, 4096 * (i + 1), 2);
    NSMutableArray<ReplacementWeak *> *views = [NSMutableArray array];
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!readback) abort(); res_add(readback);
    mtl_sub_begin();
    p_clear_tex clear = {.tex = (const tex_t *)&source, .r = {0, 0, 0, 8, 8, 1}, .mask = 15};
    for (int c = 0; c < 4; c++) clear.c.f[c] = 1;
    clear_tex(&clear); finish();
    for (int i = 0; i < ITERATIONS; i++) { @autoreleasepool {
        mtl_sub_begin();
        R.has_rt = 1; R.rt = (p_set_rt){.n = 1}; R.rt.color[0] = (const tex_t *)&destinations[i];
        R.rt.has_cview[0] = 1; R.rt.cview[0] = (view_t){.set_levels = 1, .base_level = 1, .nlevels = 1};
        g_bloomq.s_tex = (const tex_t *)&source;
        g_bloomq.threshold = 0; g_bloomq.coeff_g = 0.7152f; g_bloomq.coeff_b = 0.0722f;
        if (!bloomq_capture()) abort();
        ReplacementWeak *box = [ReplacementWeak new]; box->object = g_bloomq_view; [views addObject:box];
        enc_b_copy_texture_buffer(blit(), g_bloomq_view, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
        finish();
        unsigned bad = 0; const uint8_t *bytes = readback.contents;
        for (unsigned p = 0; p < 16; p++) for (unsigned c = 0; c < 4; c++) bad += abs((int)bytes[p * 4 + c] - (c == 3 ? 255 : 128)) > 1;
        printf("bloom GPU bytes iteration=%d: %s mismatches=%u\n", i, bad ? "FAIL" : "PASS", bad); failures += bad != 0;
    } }
    check_lifetime("replaced bloom views after GPU completion", views, 1);
    memset(&R, 0, sizeof R); g_bloomq.s_tex = NULL;
    for (int i = 0; i < ITERATIONS; i++) h_nvnTextureFinalize(&destinations[i]);
    h_nvnTextureFinalize(&source); h_nvnMemoryPoolFinalize(&pool); defer_obj_release(readback);
    mtl_sub_begin(); finish();
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0); mtl_init(); state_reset();
    printf("replacement lifetime device=%s\n", g_dev.name.UTF8String);
    fill_patterns(); bloom_views();
    printf("replacement lifetime failures=%u\n", failures);
    return failures ? 1 : 0;
} }
