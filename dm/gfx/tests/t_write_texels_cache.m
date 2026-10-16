// 真实 Metal 参数缓存回归：先缓存非预排布贴图，再经 NVN WriteTexels 写源，不重新登记或换通道。
// Real Metal argument-cache regression: cache a non-packaged texture, then write through NVN WriteTexels without re-registering or changing passes.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int failures;
static void expect(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    failures += !ok;
}

int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
    void *memory = NULL;
    if (posix_memalign(&memory, getpagesize(), 65536)) abort();
    memset(memory, 0, 65536);
    NVNmemoryPoolBuilder pool_builder;
    NVNmemoryPool pool;
    h_nvnMemoryPoolBuilderSetDefaults(&pool_builder);
    h_nvnMemoryPoolBuilderSetStorage(&pool_builder, memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(&pool_builder, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(&pool, &pool_builder)) abort();

    NVNtextureBuilder texture_builder = {0};
    NVNtexture texture = {0};
    h_nvnTextureBuilderSetDefaults(&texture_builder);
    h_nvnTextureBuilderSetTarget(&texture_builder, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&texture_builder, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&texture_builder, 4, 4);
    h_nvnTextureBuilderSetLevels(&texture_builder, 1);
    h_nvnTextureBuilderSetStorage(&texture_builder, &pool, 0);
    if (!h_nvnTextureInitialize(&texture, &texture_builder)) abort();
    NVNtexturePool descriptor_pool;
    h_nvnTexturePoolInitialize(&descriptor_pool, &pool, 32768, 512);
    h_nvnTexturePoolRegisterTexture(&descriptor_pool, 257, &texture, NULL);
    // 此测试直接编码 Metal，需显式绑定注册时使用的描述符池。
    // This test encodes Metal directly and must explicitly bind the registered descriptor pool.
    tex_pool_bind((const dpool_t *)&descriptor_pool);

    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 vertex_main(uint n [[vertex_id]]) {\n"
        "  float2 p[3] = {float2(-1,-1), float2(3,-1), float2(-1,3)}; return float4(p[n],0,1);\n}\n"
        "struct Args { texture2d<float> source [[id(0)]]; };\n"
        "fragment float4 fragment_main(float4 p [[position]], constant Args &a [[buffer(0)]]) {\n"
        "  return a.source.read(uint2(uint(p.x) % 4, uint(p.y)));\n}\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    NvShader *shader = [NvShader new];
    shader->fn = [library newFunctionWithName:@"fragment_main"];
    shader->stage = 1;
    shader->ae = [shader->fn newArgumentEncoderWithBufferIndex:0];
    shader->rf.nb = 1;
    shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D};
    MTLRenderPipelineDescriptor *pipeline_descriptor = [MTLRenderPipelineDescriptor new];
    pipeline_descriptor.vertexFunction = [library newFunctionWithName:@"vertex_main"];
    pipeline_descriptor.fragmentFunction = shader->fn;
    pipeline_descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline = [g_dev newRenderPipelineStateWithDescriptor:pipeline_descriptor error:&error];
    if (!pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    MTLTextureDescriptor *destination_descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:4 mipmapped:NO];
    destination_descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> destination = [g_dev newTextureWithDescriptor:destination_descriptor];
    id<MTLBuffer> readback = [g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
    if (!destination || !readback) abort();
    res_add(destination); res_add(readback);

    state_reset();
    S.tex[1][0] = 257;
    mtl_sub_begin();
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = destination;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 1, 1);
    id<MTL4RenderCommandEncoder> encoder = enc_r_begin(pass);
    enc_r_pso(encoder, pipeline);
    enc_r_viewport(encoder, (MTLViewport){0, 0, 8, 4, 0, 1});
    // 两次相同绑定建立并命中真实参数缓存；写入后仍在同一编码器、同一通道取用。
    // Identical bindings establish and reuse the real argument cache; the write is followed by another use in the same encoder and pass.
    if (!encode_args((id)encoder, shader, 1, 0) || !encode_args((id)encoder, shader, 1, 0)) abort();
    enc_r_scissor(encoder, (MTLScissorRect){0, 0, 4, 4});
    enc_r_draw(encoder, MTLPrimitiveTypeTriangle, 0, 3);

    uint8_t pixels[64];
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        uint8_t *p = pixels + 4 * (4 * y + x);
        p[0] = 20 + 40 * x; p[1] = 30 + 30 * y; p[2] = 100; p[3] = 255;
    }
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    h_nvnTextureWriteTexels(&texture, NULL, &region, pixels);
    if (!encode_args((id)encoder, shader, 1, 0)) abort();
    enc_r_scissor(encoder, (MTLScissorRect){4, 0, 4, 4});
    enc_r_draw(encoder, MTLPrimitiveTypeTriangle, 0, 3);
    enc_r_end(encoder);
    enc_b_copy_texture_buffer(blit(), destination, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(8, 4, 1), readback, 0, 32, 128);
    uint64_t fence = mtl_sub_commit();
    int completed = mtl_wait(fence, 5000000000ull);
    expect("Metal submission completed", completed);
    if (!completed) return 2;
    expect("Metal feedback succeeded", !mtl_last_commit_error());
    unsigned stale_pixels = 0, updated_pixels = 0;
    const uint8_t *output = readback.contents;
    const uint8_t zero[4] = {0};
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        stale_pixels += memcmp(output + 4 * (8 * y + x), zero, 4) != 0;
        updated_pixels += memcmp(output + 4 * (8 * y + x + 4), pixels + 4 * (4 * y + x), 4) != 0;
    }
    printf("initial zero mismatches=%u; WriteTexels mismatches=%u / 16\n", stale_pixels, updated_pixels);
    expect("cached non-packaged texture initially samples zero", stale_pixels == 0);
    expect("same-pass cached arguments sample real WriteTexels bytes", updated_pixels == 0);
    h_nvnTexturePoolFinalize(&descriptor_pool);
    h_nvnTextureFinalize(&texture);
    h_nvnMemoryPoolFinalize(&pool);
    // 客体 backing 留到进程退出，避免延迟退役的 Metal no-copy buffer 比源内存活得更久。
    // Keep guest backing until process exit so a deferred Metal no-copy buffer cannot outlive its source memory.
    printf("WriteTexels cache integration failures=%d\n", failures);
    return failures != 0;
} }
