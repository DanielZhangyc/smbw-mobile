// 存储归属生命周期回归：真实 Metal 读回、提交前销毁及非拥有弱引用观测。
// Storage-ownership lifetime regressions: real Metal readback, pre-submit finalization, and non-owning weak observations.
#ifndef DM_STORAGE_MTL_SOURCE
#define DM_STORAGE_MTL_SOURCE "../nvn/nvn_mtl.m"
#endif
#include DM_STORAGE_MTL_SOURCE
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface StorageWeak : NSObject { @public __weak id object; } @end
@implementation StorageWeak @end
static unsigned failures;
static id<MTLComputePipelineState> buffer_pipeline;
static id<MTL4ArgumentTable> buffer_arguments;

static void check(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL"); failures += !ok;
}
static void wait_fence(uint64_t fence) {
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
}
static void finish(void) { wait_fence(mtl_sub_commit()); }
static void drain(void) {
    @autoreleasepool { mtl_sub_begin(); finish(); }
    @autoreleasepool { mtl_sub_begin(); finish(); }
}
static void initialize_pool(NVNmemoryPool *pool, NVNmemoryPoolBuilder *builder, void **memory) {
    if (posix_memalign(memory, getpagesize(), 65536)) abort();
    memset(*memory, 0, 65536);
    h_nvnMemoryPoolBuilderSetDefaults(builder);
    h_nvnMemoryPoolBuilderSetStorage(builder, *memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(builder, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(pool, builder)) abort();
}
static void initialize_texture(NVNtexture *texture, NVNmemoryPool *pool, int target, int format) {
    NVNtextureBuilder builder;
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetTarget(&builder, target);
    h_nvnTextureBuilderSetFormat(&builder, format);
    h_nvnTextureBuilderSetSize2D(&builder, target == NVN_TEXTURE_TARGET_BUFFER ? 16 : 4, target == NVN_TEXTURE_TARGET_BUFFER ? 1 : 4);
    h_nvnTextureBuilderSetLevels(&builder, 1);
    h_nvnTextureBuilderSetStorage(&builder, pool, 0);
    if (!h_nvnTextureInitialize(texture, &builder)) abort();
}
static int resident(id object) {
    pthread_mutex_lock(&g_res_mu); int found = [g_res_objects containsObject:object]; pthread_mutex_unlock(&g_res_mu);
    return found;
}
static void check_bytes(const char *name, id<MTLBuffer> buffer, const uint8_t color[4]) {
    const uint8_t *bytes = buffer.contents; unsigned bad = 0;
    for (int p = 0; p < 16; p++) for (int c = 0; c < 4; c++) bad += bytes[p * 4 + c] != color[c];
    printf("%s: %s mismatched_channels=%u/64\n", name, bad ? "FAIL" : "PASS", bad); failures += bad != 0;
}
static void copy_texture(id<MTLTexture> texture, id<MTLBuffer> buffer) {
    enc_b_copy_texture_buffer(blit(), texture, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), buffer, 0, 16, 64);
}
// 让 NVN 的真实录制器生成快照；解析该快照后用计算核读取 buffer texture，避免依赖不支持此纹理类型的 blit。
// Let the real NVN recorder create the snapshot; resolve it and read the buffer texture with compute, avoiding blits unsupported for that texture type.
static NVNcommandHandle record_buffer_texture(NVNtexture *texture) {
    NVNcommandBuffer command;
    if (!h_nvnCommandBufferInitialize(&command, NULL)) abort();
    h_nvnCommandBufferAddCommandMemory(&command, NULL, 0, 0);
    h_nvnCommandBufferBeginRecording(&command);
    NVNcopyRegion region = {0, 0, 0, 16, 1, 1};
    const tex_t *source = (const tex_t *)texture;
    NVNbufferAddress destination = (NVNbufferAddress)(uintptr_t)(source->pool->mem + 4096);
    h_nvnCommandBufferCopyTextureToBuffer(&command, texture, NULL, &region, destination, 0);
    NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&command);
    h_nvnCommandBufferFinalize(&command);
    return handle;
}
static id<MTLTexture> resolve_buffer_recording(NVNcommandHandle handle) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)handle;
    op_hdr header; memcpy(&header, p, sizeof header);
    if (header.op != OP_BEGIN) abort();
    p += header.size; memcpy(&header, p, sizeof header);
    if (header.op != OP_COPY_T2B) abort();
    const p_copy_b2t *copy = (const p_copy_b2t *)(p + 8);
    return tex_mtl(copy->tex);
}
static void read_buffer_texture(id<MTLTexture> texture, id<MTLBuffer> output) {
    id<MTL4ComputeCommandEncoder> encoder = enc_c_begin(NULL);
    [encoder setArgumentTable:buffer_arguments];
    [buffer_arguments setTexture:texture.gpuResourceID atIndex:0];
    [buffer_arguments setAddress:output.gpuAddress atIndex:0];
    enc_c_pso(encoder, buffer_pipeline);
    enc_c_dispatch(encoder, MTLSizeMake(1, 1, 1), MTLSizeMake(16, 1, 1));
    enc_c_end(encoder); args_dirty_all();
}
static void buffer_lifetime(void) {
    NVNmemoryPool pool; NVNmemoryPoolBuilder builder; void *memory;
    initialize_pool(&pool, &builder, &memory);
    uint32_t expected[16]; for (unsigned i = 0; i < 16; i++) expected[i] = 0x12340000u + 37u * i;
    memcpy(memory, expected, sizeof expected);
    NVNtexture a = {0}, b = {0};
    initialize_texture(&a, &pool, NVN_TEXTURE_TARGET_BUFFER, NVN_FORMAT_R32UI);
    initialize_texture(&b, &pool, NVN_TEXTURE_TARGET_BUFFER, NVN_FORMAT_R32UI);
    NVNcommandHandle first = record_buffer_texture(&a), second = record_buffer_texture(&b);
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b);
    StorageWeak *view = [StorageWeak new], *backing = [StorageWeak new];
    id<MTLBuffer> output = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!output) abort(); res_add(output);
    id<MTLSharedEvent> gate = [g_dev newSharedEvent]; if (!gate) abort();
    uint64_t fence;
    @autoreleasepool {
        mtl_sub_begin();
        id<MTLTexture> one = resolve_buffer_recording(first), two = resolve_buffer_recording(second);
        if (!one || !two) abort();
        view->object = one; backing->object = one.buffer;
        check("finalized command snapshots reuse one buffer texture by storage", one == two);
        read_buffer_texture(one, output);
        h_nvnMemoryPoolFinalize(&pool);
        [g_q4 waitForEvent:gate value:1]; fence = mtl_sub_commit();
    }
    @autoreleasepool {
        check("buffer texture and pool backing survive a pending GPU fence", g_done < fence && view->object && backing->object);
    }
    gate.signaledValue = 1; wait_fence(fence);
    check("buffer texture GPU readback matches original pool bytes", !memcmp(output.contents, expected, sizeof expected));
    drain();
    @autoreleasepool {
        check("buffer texture releases after pool retirement and GPU fence", view->object == nil);
        check("buffer texture cannot retain retired whole-pool Metal buffer", backing->object == nil);
    }
    // 复用同一池对象和客体 backing，新的归属必须读新内容。
    // Reuse the same pool object and guest backing: new ownership must read new contents.
    if (!h_nvnMemoryPoolInitialize(&pool, &builder)) abort();
    for (unsigned i = 0; i < 16; i++) expected[i] ^= 0x55aa3300u;
    memcpy(memory, expected, sizeof expected);
    initialize_texture(&a, &pool, NVN_TEXTURE_TARGET_BUFFER, NVN_FORMAT_R32UI);
    NVNcommandHandle reused = record_buffer_texture(&a); h_nvnTextureFinalize(&a);
    @autoreleasepool {
        mtl_sub_begin(); id<MTLTexture> texture = resolve_buffer_recording(reused);
        if (!texture) abort(); read_buffer_texture(texture, output); h_nvnMemoryPoolFinalize(&pool); finish();
    }
    check("reused pool object and backing read the new buffer contents", !memcmp(output.contents, expected, sizeof expected));
    defer_obj_release(output); drain();
    // no-copy 存储保留到进程结束；基线泄漏时也不能提前释放其 backing。
    // Keep no-copy storage until process exit, including when the baseline still leaks its Metal buffer.
}
static void first_upload(int with_peer) {
    printf("first upload mode=%s\n", with_peer ? "RGBA8/sRGB peer" : "one storage alias");
    NVNmemoryPool pool; NVNmemoryPoolBuilder builder; void *memory;
    initialize_pool(&pool, &builder, &memory);
    NVNtexture writer = {0}, peer = {0};
    initialize_texture(&writer, &pool, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8);
    if (with_peer) initialize_texture(&peer, &pool, NVN_TEXTURE_TARGET_2D, NVN_FORMAT_RGBA8_SRGB);
    StorageWeak *old = [StorageWeak new], *old_peer = [StorageWeak new], *uploaded = [StorageWeak new];
    id<MTLBuffer> output = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!output) abort(); res_add(output);
    @autoreleasepool {
        mtl_sub_begin(); id<MTLTexture> original = tex_mtl((const tex_t *)&writer);
        if (!original) abort(); old->object = tex_allocation_root(original);
        if (with_peer) {
            id<MTLTexture> sibling = tex_mtl((const tex_t *)&peer); old_peer->object = sibling;
            check("RGBA8 peer initially shares the same allocation", tex_allocation_root(sibling) == old->object);
        }
        finish();
    }
    const uint8_t color[4] = {17, 51, 85, 255}, zero[4] = {0}; uint8_t pixels[64];
    for (int p = 0; p < 16; p++) memcpy(pixels + p * 4, color, 4);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    h_nvnTextureWriteTexels(&writer, NULL, &region, pixels);
    @autoreleasepool {
        mtl_sub_begin(); id<MTLTexture> texture = tex_mtl((const tex_t *)&writer);
        if (!texture) abort(); uploaded->object = texture;
        check("first WriteTexels upload replaces the former GPU allocation", tex_allocation_root(texture) != old->object);
        copy_texture(texture, output); finish();
    }
    check_bytes("first WriteTexels upload has correct GPU bytes", output, color);
    drain();
    @autoreleasepool {
        if (with_peer) {
            check("untouched sRGB peer keeps its old allocation resident", old->object && old_peer->object && resident(old->object));
            mtl_sub_begin(); id<MTLTexture> texture = tex_mtl((const tex_t *)&peer);
            check("untouched sRGB peer resolves its original allocation", tex_allocation_root(texture) == old->object);
            copy_texture(texture, output); finish();
        } else check("first upload retires the old storage alias after GPU fence", old->object == nil);
    }
    if (with_peer) check_bytes("untouched sRGB peer retains its own GPU contents", output, zero);
    h_nvnTextureFinalize(&writer); if (with_peer) h_nvnTextureFinalize(&peer);
    h_nvnMemoryPoolFinalize(&pool); drain();
    @autoreleasepool {
        check("pool finalization releases uploaded storage", uploaded->object == nil);
        check("pool finalization releases former alias and RGBA8 peer", old->object == nil && old_peer->object == nil);
    }
    defer_obj_release(output); drain();
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0); mtl_init(); state_reset();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void read_buffer(texture_buffer<uint, access::read> source [[texture(0)]], device uint *output [[buffer(0)]], uint x [[thread_position_in_grid]]) { output[x] = source.read(x).x; }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    buffer_pipeline = [g_dev newComputePipelineStateWithFunction:[library newFunctionWithName:@"read_buffer"] error:&error];
    MTL4ArgumentTableDescriptor *descriptor = [MTL4ArgumentTableDescriptor new]; descriptor.maxTextureBindCount = 1; descriptor.maxBufferBindCount = 1;
    buffer_arguments = [g_dev newArgumentTableWithDescriptor:descriptor error:&error];
    if (!buffer_pipeline || !buffer_arguments) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    printf("storage lifetime device=%s\n", g_dev.name.UTF8String);
    buffer_lifetime(); first_upload(0); first_upload(1);
    printf("storage lifetime failures=%u\n", failures); return failures != 0;
} }
