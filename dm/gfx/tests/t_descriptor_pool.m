// 描述符池生命周期回归：真实 Metal 读回与弱引用检查，不依赖游戏素材。
// Descriptor-pool lifetime regressions: real Metal readback and weak-reference checks without game assets.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface DescriptorWeak : NSObject { @public __weak id object; } @end
@implementation DescriptorWeak @end
static unsigned failures;
static void check(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL"); failures += !ok;
}
static void wait_fence(uint64_t fence) {
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
}
static void finish(void) { wait_fence(mtl_sub_commit()); }
static void drain(void) {
    mtl_sub_begin(); finish();
    mtl_sub_begin(); finish();
}
static void initialize_pool(NVNmemoryPool *pool, void **memory) {
    if (posix_memalign(memory, getpagesize(), 65536)) abort();
    memset(*memory, 0, 65536);
    NVNmemoryPoolBuilder builder;
    h_nvnMemoryPoolBuilderSetDefaults(&builder);
    h_nvnMemoryPoolBuilderSetStorage(&builder, *memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(&builder, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(pool, &builder)) abort();
}
static void initialize_texture(NVNtexture *texture, NVNmemoryPool *pool, int offset) {
    NVNtextureBuilder builder;
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&builder, 4, 4);
    h_nvnTextureBuilderSetStorage(&builder, pool, offset);
    if (!h_nvnTextureInitialize(texture, &builder)) abort();
}
static void clear_texture(NVNtexture *texture, const uint8_t color[4]) {
    p_clear_tex clear = {.tex = (const tex_t *)texture, .r = {0, 0, 0, 4, 4, 1}, .mask = 15};
    for (int c = 0; c < 4; c++) clear.c.f[c] = color[c] / 255.f;
    mtl_sub_begin(); clear_tex(&clear); finish();
}
static void check_bytes(const char *name, id<MTLBuffer> readback, const uint8_t color[4]) {
    const uint8_t *bytes = readback.contents; unsigned bad = 0;
    for (int p = 0; p < 16; p++) for (int c = 0; c < 4; c++) bad += bytes[p * 4 + c] != color[c];
    printf("%s: %s mismatched_channels=%u/64 first=%u,%u,%u,%u\n", name, bad ? "FAIL" : "PASS", bad, bytes[0], bytes[1], bytes[2], bytes[3]);
    failures += bad != 0;
}
static NVNcommandHandle record_pool(NVNtexturePool *descriptors) {
    NVNcommandBuffer command;
    if (!h_nvnCommandBufferInitialize(&command, NULL)) abort();
    h_nvnCommandBufferAddCommandMemory(&command, NULL, 0, 0);
    h_nvnCommandBufferBeginRecording(&command);
    h_nvnCommandBufferSetTexturePool(&command, descriptors);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&command);
    h_nvnCommandBufferFinalize(&command);
    return handle;
}
static void bind_recording(NVNcommandHandle handle) { mtl_exec((const uint8_t *)(uintptr_t)handle); }
static void copy_descriptor(int tid, id<MTLBuffer> readback) {
    pthread_mutex_lock(&g_rmu);
    id<MTLTexture> texture = tex_by_id(tid);
    pthread_mutex_unlock(&g_rmu);
    if (!texture) abort();
    enc_b_copy_texture_buffer(blit(), texture, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
}
static uint64_t recorded_generation(NVNcommandHandle handle) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)handle;
    op_hdr header; memcpy(&header, p, sizeof header);
    if (header.op != OP_BEGIN) abort();
    p += header.size; memcpy(&header, p, sizeof header);
    if (header.op != OP_SET_TEX_POOL) abort();
    p_set_pool recorded; memcpy(&recorded, p + 8, sizeof recorded);
    return recorded.pool.generation;
}
static void pool_namespaces(void) {
    const uint8_t red[4] = {255, 0, 0, 255}, blue[4] = {0, 0, 255, 255};
    NVNmemoryPool storage; void *memory; initialize_pool(&storage, &memory);
    NVNtexture a = {0}, b = {0}; initialize_texture(&a, &storage, 0); initialize_texture(&b, &storage, 4096);
    clear_texture(&a, red); clear_texture(&b, blue);
    NVNtexturePool pool_a, pool_b;
    h_nvnTexturePoolInitialize(&pool_a, &storage, 32768, 512);
    h_nvnTexturePoolInitialize(&pool_b, &storage, 49152, 512);
    h_nvnTexturePoolRegisterTexture(&pool_a, 300, &a, NULL);
    h_nvnTexturePoolRegisterTexture(&pool_b, 300, &b, NULL);
    NVNcommandHandle old_a = record_pool(&pool_a), command_b = record_pool(&pool_b);
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!readback) abort(); res_add(readback);
    mtl_sub_begin(); bind_recording(old_a); copy_descriptor(300, readback); finish();
    check_bytes("pool A resolves its relative texture ID", readback, red);
    mtl_sub_begin(); bind_recording(command_b); copy_descriptor(300, readback); finish();
    check_bytes("pool B independently resolves the same relative texture ID", readback, blue);
    NVNtexturePool stale_a = pool_a;
    h_nvnTexturePoolFinalize(&pool_a);
    mtl_sub_begin(); bind_recording(command_b); copy_descriptor(300, readback); finish();
    check_bytes("finalizing pool A preserves pool B descriptor", readback, blue);
    uint64_t old_generation = ((dpool_t *)&stale_a)->generation;
    h_nvnTexturePoolInitialize(&pool_a, &storage, 32768, 512);
    check("same pool object and descriptor backing receive a new generation", ((dpool_t *)&pool_a)->generation != old_generation);
    h_nvnTexturePoolRegisterTexture(&pool_a, 300, &a, NULL);
    NVNcommandHandle new_a = record_pool(&pool_a);
    check("old recording retains original generation after object and backing reuse", recorded_generation(old_a) == old_generation);
    check("new recording captures the replacement generation", recorded_generation(new_a) == ((dpool_t *)&pool_a)->generation);
    h_nvnTexturePoolFinalize(&stale_a);
    mtl_sub_begin(); bind_recording(new_a); copy_descriptor(300, readback); finish();
    check_bytes("retiring an old generation preserves reused pool descriptors", readback, red);
    h_nvnTexturePoolFinalize(&pool_a); h_nvnTexturePoolFinalize(&pool_b);
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b); h_nvnMemoryPoolFinalize(&storage);
    defer_obj_release(readback); drain();
}
static void frozen_lifetime(int submitted) {
    printf("frozen descriptor lifetime mode=%s\n", submitted ? "submitted GPU wait" : "encoded before commit");
    const uint8_t color[4] = {17, 51, 85, 255};
    NVNmemoryPool storage; void *memory; initialize_pool(&storage, &memory);
    NVNtexture texture = {0}; initialize_texture(&texture, &storage, 0);
    NVNtexturePool descriptors;
    h_nvnTexturePoolInitialize(&descriptors, &storage, 32768, 512);
    NVNtextureView subview; h_nvnTextureViewSetDefaults(&subview);
    h_nvnTextureViewSetLevels(&subview, 0, 1);
    h_nvnTexturePoolRegisterTexture(&descriptors, 300, &texture, &subview);
    NVNcommandHandle recording = record_pool(&descriptors);
    clear_texture(&texture, color);
    DescriptorWeak *view = [DescriptorWeak new], *root = [DescriptorWeak new], *frozen = [DescriptorWeak new];
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!readback) abort(); res_add(readback);
    id<MTLSharedEvent> gate = submitted ? [g_dev newSharedEvent] : nil;
    if (submitted && !gate) abort();
    uint64_t fence = 0;
    @autoreleasepool {
        mtl_sub_begin();
        bind_recording(recording);
        pthread_mutex_lock(&g_rmu);
        id<MTLTexture> resolved = tex_by_id(300);
        view->object = resolved; root->object = tex_allocation_root(resolved);
        pthread_mutex_unlock(&g_rmu);
        h_nvnTextureFinalize(&texture); h_nvnMemoryPoolFinalize(&storage);
        pthread_mutex_lock(&g_rmu); frozen->object = (__bridge id)g_treg[300].frozen; pthread_mutex_unlock(&g_rmu);
        check("memory-pool finalization creates frozen descriptor owner", frozen->object != nil);
        enc_b_copy_texture_buffer(blit(), resolved, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
        if (submitted) {
            // 用真实 Metal 事件阻住队列，保证销池时提交仍未执行完成。
            // Gate the queue with a real Metal event so finalization occurs before the submission completes.
            [g_q4 waitForEvent:gate value:1];
            fence = mtl_sub_commit();
        }
        h_nvnTexturePoolFinalize(&descriptors);
        check("encoded descriptor resources survive before submit fence", view->object && root->object);
        if (!submitted) finish();
    }
    if (submitted) {
        check("GPU submission is still pending during descriptor finalization", g_done < fence);
        check("in-flight frozen descriptor resources remain alive", view->object && root->object && frozen->object);
        gate.signaledValue = 1; wait_fence(fence);
    }
    check_bytes("GPU readback survives descriptor-pool finalization", readback, color);
    drain();
    check("descriptor finalization releases cached view after GPU fence", view->object == nil);
    check("descriptor finalization releases frozen owner after GPU fence", frozen->object == nil);
    check("descriptor finalization releases frozen root after GPU fence", root->object == nil);
    defer_obj_release(readback); drain();
    // no-copy 存储保留至进程退出，避免 Metal 对象晚于 backing 析构。
    // Keep no-copy storage until process exit so Metal objects cannot outlive their backing.
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0); mtl_init(); state_reset();
    printf("descriptor pool device=%s\n", g_dev.name.UTF8String);
    pool_namespaces(); frozen_lifetime(0); frozen_lifetime(1);
    printf("descriptor pool failures=%u\n", failures);
    return failures ? 1 : 0;
} }
