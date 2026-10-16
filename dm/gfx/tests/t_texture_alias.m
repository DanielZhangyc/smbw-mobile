// 同一 NVN 存储的真实 Metal 别名回归：同描述 GPU 写、线性/sRGB 格式解释，以及双序号 CPU 写。
// Real Metal alias regressions for one NVN storage: GPU writes with identical descriptors, linear/sRGB interpretation, and CPU writes across two serials.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static unsigned failures;
static NvShader *sample_shader;
static id<MTLRenderPipelineState> sample_pipeline;
static void check(const char *name, const uint8_t *bytes, const uint8_t expected[4], int tolerance) {
    unsigned mismatches = 0;
    for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++)
        mismatches += abs((int)bytes[4 * i + c] - expected[c]) > tolerance;
    printf("%s: mismatched channels=%u/64 first=%u,%u,%u,%u expected=%u,%u,%u,%u\n", name,
        mismatches, bytes[0], bytes[1], bytes[2], bytes[3], expected[0], expected[1], expected[2], expected[3]);
    failures += mismatches != 0;
}
static void finish(void) {
    uint64_t fence = mtl_sub_commit();
    if (!mtl_wait(fence, 5000000000ull) || mtl_last_commit_error()) abort();
}
static void sample(int tid, uint8_t bytes[64]) {
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> output = [g_dev newTextureWithDescriptor:d];
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    if (!output || !readback) abort();
    res_add(output); res_add(readback);
    mtl_sub_begin();
    S.tex[1][0] = tid;
    args_dirty_all();
    MTL4RenderPassDescriptor *pass = [MTL4RenderPassDescriptor new];
    pass.colorAttachments[0].texture = output;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTL4RenderCommandEncoder> e = enc_r_begin(pass);
    enc_r_pso(e, sample_pipeline);
    enc_r_viewport(e, (MTLViewport){0, 0, 4, 4, 0, 1});
    enc_r_scissor(e, (MTLScissorRect){0, 0, 4, 4});
    if (!encode_args((id)e, sample_shader, 1, 0)) abort();
    enc_r_draw(e, MTLPrimitiveTypeTriangle, 0, 3);
    enc_r_end(e);
    enc_b_copy_texture_buffer(blit(), output, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
    finish();
    memcpy(bytes, readback.contents, 64);
    defer_obj_release(output); defer_obj_release(readback);
}
static void read_texture(id<MTLTexture> source, uint8_t bytes[64]) {
    id<MTLBuffer> readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
    res_add(readback);
    mtl_sub_begin();
    enc_b_copy_texture_buffer(blit(), source, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
    finish(); memcpy(bytes, readback.contents, 64); defer_obj_release(readback);
}
static void gpu_write(NVNtexture *t, void *memory, const uint8_t pixels[64], NVNcopyRegion region) {
    memcpy((uint8_t *)memory + 16384, pixels, 64);
    p_copy_b2t copy = {.src = (uint64_t)memory + 16384, .tex = (const tex_t *)t, .r = region};
    mtl_sub_begin(); copy_b2t(&copy); finish();
}
static void texture(NVNtexture *t, NVNmemoryPool *pool, int off, int format) {
    NVNtextureBuilder b = {0};
    h_nvnTextureBuilderSetDefaults(&b);
    h_nvnTextureBuilderSetTarget(&b, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&b, format);
    h_nvnTextureBuilderSetSize2D(&b, 4, 4);
    h_nvnTextureBuilderSetLevels(&b, 1);
    h_nvnTextureBuilderSetStorage(&b, pool, off);
    if (!h_nvnTextureInitialize(t, &b)) abort();
}
static void format_swizzle(NVNmemoryPool *pool, NVNtexturePool *descriptors, void *memory) {
    const uint8_t raw[4] = {25, 75, 125, 9}, rgbx[4] = {25, 75, 125, 255};
    uint8_t pixels[64], bytes[64];
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, raw, 4);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    NVNtexture x = {0}, builder_x = {0}, rgba = {0};
    texture(&x, pool, 12288, NVN_FORMAT_RGBX8);
    texture(&rgba, pool, 24576, NVN_FORMAT_RGBA8);
    NVNtextureBuilder builder = {0};
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBX8);
    h_nvnTextureBuilderSetSize2D(&builder, 4, 4);
    h_nvnTextureBuilderSetStorage(&builder, pool, 12288);
    h_nvnTextureBuilderSetSwizzle(&builder, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_B);
    if (!h_nvnTextureInitialize(&builder_x, &builder)) abort();
    NVNtextureView user = {0}, alpha_red = {0}, constants = {0}, rgba_format = {0}, x_format = {0};
    h_nvnTextureViewSetDefaults(&user);
    h_nvnTextureViewSetSwizzle(&user, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_G);
    h_nvnTextureViewSetDefaults(&alpha_red);
    h_nvnTextureViewSetSwizzle(&alpha_red, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_R);
    h_nvnTextureViewSetDefaults(&constants);
    h_nvnTextureViewSetSwizzle(&constants, NVN_TEXTURE_SWIZZLE_ZERO, NVN_TEXTURE_SWIZZLE_ONE, NVN_TEXTURE_SWIZZLE_A, NVN_TEXTURE_SWIZZLE_R);
    h_nvnTextureViewSetDefaults(&rgba_format); h_nvnTextureViewSetFormat(&rgba_format, NVN_FORMAT_RGBA8);
    h_nvnTextureViewSetDefaults(&x_format); h_nvnTextureViewSetFormat(&x_format, NVN_FORMAT_RGBX8);
    h_nvnTexturePoolRegisterTexture(descriptors, 280, &x, NULL);
    h_nvnTexturePoolRegisterTexture(descriptors, 281, &builder_x, NULL);
    h_nvnTexturePoolRegisterTexture(descriptors, 282, &builder_x, &user);
    h_nvnTexturePoolRegisterTexture(descriptors, 283, &x, &alpha_red);
    h_nvnTexturePoolRegisterTexture(descriptors, 284, &x, &constants);
    h_nvnTexturePoolRegisterTexture(descriptors, 285, &x, &rgba_format);
    h_nvnTexturePoolRegisterTexture(descriptors, 286, &rgba, &x_format);
    gpu_write(&x, memory, pixels, region); gpu_write(&rgba, memory, pixels, region);
    sample(280, bytes); check("RGBX format alpha is one", bytes, rgbx, 0);
    const uint8_t builder_expected[4] = {255, 75, 25, 125};
    sample(281, bytes); check("builder swizzle selects logical RGBX alpha", bytes, builder_expected, 0);
    const uint8_t view_expected[4] = {255, 25, 125, 75};
    sample(282, bytes); check("descriptor overrides builder and selects logical alpha", bytes, view_expected, 0);
    const uint8_t alpha_red_expected[4] = {25, 75, 125, 25};
    sample(283, bytes); check("descriptor may route red into output alpha", bytes, alpha_red_expected, 0);
    const uint8_t constants_expected[4] = {0, 255, 255, 25};
    sample(284, bytes); check("constant selectors and logical alpha compose", bytes, constants_expected, 0);
    sample(285, bytes); check("RGBA view of RGBX storage exposes stored alpha", bytes, raw, 0);
    sample(286, bytes); check("RGBX view of RGBA storage supplies alpha one", bytes, rgbx, 0);
    for (int i = 280; i <= 286; i++) mtl_reg_tex(i, NULL, NULL);
    h_nvnTextureFinalize(&x); h_nvnTextureFinalize(&builder_x); h_nvnTextureFinalize(&rgba);
}

static void packaged_texture(NVNtexture *t, NVNmemoryPool *pool) {
    NVNtextureBuilder builder = {0}; NVNpackagedTextureLayout layout = {0};
    h_nvnTextureBuilderSetDefaults(&builder);
    h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&builder, 4, 4);
    h_nvnTextureBuilderSetLevels(&builder, 1);
    h_nvnTextureBuilderSetPackagedTextureLayout(&builder, &layout);
    h_nvnTextureBuilderSetStorage(&builder, pool, 0);
    if (!h_nvnTextureInitialize(t, &builder)) abort();
}
static void packaged_alias(void) {
    const uint8_t first[4] = {128, 64, 200, 255}, second[4] = {32, 128, 240, 255};
    const uint8_t third[4] = {192, 48, 96, 255}, fourth[4] = {17, 51, 85, 255};
    const uint8_t swizzled_second[4] = {240, 128, 32, 255};
    uint8_t pixels[64], bytes[64]; NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    void *memory = NULL;
    if (posix_memalign(&memory, getpagesize(), 65536)) abort();
    memset(memory, 0, 65536);
    NVNmemoryPoolBuilder pb; NVNmemoryPool pool;
    h_nvnMemoryPoolBuilderSetDefaults(&pb);
    h_nvnMemoryPoolBuilderSetStorage(&pb, memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
    NVNtexture a = {0}, b = {0}, recycled = {0};
    packaged_texture(&a, &pool); packaged_texture(&b, &pool);
    NVNtexturePool descriptors;
    h_nvnTexturePoolInitialize(&descriptors, &pool, 32768, 512);
    // 本测试直接编码 Metal；显式选择与 NVN 命令回放相同的描述符池。
    // This test encodes Metal directly; explicitly select the descriptor pool used by NVN replay.
    tex_pool_bind((const dpool_t *)&descriptors);
    NVNtextureView integer_view = {0}, swizzle_view = {0};
    h_nvnTextureViewSetDefaults(&integer_view); h_nvnTextureViewSetFormat(&integer_view, NVN_FORMAT_R32UI);
    h_nvnTextureViewSetDefaults(&swizzle_view);
    h_nvnTextureViewSetSwizzle(&swizzle_view, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_A);
    h_nvnTexturePoolRegisterTexture(&descriptors, 300, &a, NULL);
    h_nvnTexturePoolRegisterTexture(&descriptors, 301, &b, NULL);
    h_nvnTexturePoolRegisterTexture(&descriptors, 302, &b, &integer_view);
    h_nvnTexturePoolRegisterTexture(&descriptors, 303, &a, &swizzle_view);
    h_nvnTexturePoolRegisterTexture(&descriptors, 304, &a, NULL);
    pthread_mutex_lock(&g_smu); unsigned before = g_upload_storage.n; pthread_mutex_unlock(&g_smu);
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, first, 4);
    h_nvnTextureWriteTexels(&a, NULL, &region, pixels);
    sample(300, bytes); check("packaged writer uploads real source bytes", bytes, first, 0);
    sample(301, bytes); check("packaged peer shares uploaded bytes", bytes, first, 0);
    sample(304, bytes); check("packaged untouched descriptor initially caches bytes", bytes, first, 0);
    pthread_mutex_lock(&g_rmu);
    int same = tex_allocation_root(tex_by_id(300)) == tex_allocation_root(tex_by_id(301));
    pthread_mutex_unlock(&g_rmu);
    pthread_mutex_lock(&g_smu); unsigned owners = g_upload_storage.n - before; pthread_mutex_unlock(&g_smu);
    printf("packaged identical storage: roots_shared=%d owners=%u expected=1\n", same, owners);
    failures += !same || owners != 1;
    // 两个序号共用 NvPkg；整数描述符升级当前底图，同时读取已编码使用的旧底图。
    // Two serials share NvPkg; an integer descriptor upgrades its current root while already-encoded reads still use the old root.
    @autoreleasepool {
        id<MTLBuffer> readback = [g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared]; res_add(readback);
        mtl_sub_begin(); pthread_mutex_lock(&g_rmu);
        id<MTLTexture> old = tex_by_id(300), integer = tex_by_id(302);
        int upgraded = tex_allocation_root(old) != tex_allocation_root(integer);
        pthread_mutex_unlock(&g_rmu);
        printf("packaged shared NvPkg PFV changes root: %s\n", upgraded ? "PASS" : "FAIL"); failures += !upgraded;
        id<MTL4ComputeCommandEncoder> copy = blit();
        enc_b_copy_texture_buffer(copy, old, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 0, 16, 64);
        enc_b_copy_texture_buffer(copy, integer, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), readback, 64, 16, 64);
        finish();
        check("packaged old root survives PFV in-flight fence", readback.contents, first, 0);
        check("packaged PFV integer view preserves bytes", (const uint8_t *)readback.contents + 64, first, 0);
        defer_obj_release(readback);
    }
    sample(300, bytes); check("packaged PFV writer rebuilds onto shared root", bytes, first, 0);
    sample(301, bytes); check("packaged PFV peer rebuilds onto shared root", bytes, first, 0);
    // 从另一个 API 对象写同一存储；普通视图和整数视图都必须跟随重传。
    // Write the same storage through the other API object; ordinary and integer views must both follow the re-upload.
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, second, 4);
    h_nvnTextureWriteTexels(&b, NULL, &region, pixels);
    sample(301, bytes); check("packaged peer triggers shared re-upload", bytes, second, 0);
    sample(300, bytes); check("packaged original writer sees peer re-upload", bytes, second, 0);
    sample(303, bytes); check("packaged swizzle follows peer re-upload", bytes, swizzled_second, 0);
    @autoreleasepool {
        mtl_sub_begin(); pthread_mutex_lock(&g_rmu); id<MTLTexture> integer = tex_by_id(302); pthread_mutex_unlock(&g_rmu); finish();
        read_texture(integer, bytes); check("packaged integer descriptor follows re-upload", bytes, second, 0);
    }
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b);
    h_nvnMemoryPoolFinalize(&pool);
    pthread_mutex_lock(&g_rmu);
    int frozen_shared = g_treg[300].frozen && g_treg[300].frozen == g_treg[301].frozen && g_treg[300].frozen == g_treg[302].frozen;
    pthread_mutex_unlock(&g_rmu);
    printf("packaged descriptors share frozen owner: %s\n", frozen_shared ? "PASS" : "FAIL"); failures += !frozen_shared;
    sample(300, bytes); check("packaged frozen writer retains latest upload", bytes, second, 0);
    sample(301, bytes); check("packaged frozen peer retains latest upload", bytes, second, 0);
    sample(304, bytes); check("packaged untouched frozen descriptor resolves latest upload", bytes, second, 0);
    // 复用池对象、客体 backing 和偏移，必须创建新上传归属而不改旧冻结内容。
    // Reuse the pool object, guest backing and offset: create fresh upload ownership without changing frozen contents.
    memset(memory, 0, 65536);
    if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
    packaged_texture(&recycled, &pool);
    h_nvnTexturePoolRegisterTexture(&descriptors, 305, &recycled, NULL);
    h_nvnTexturePoolRegisterTexture(&descriptors, 306, &recycled, &integer_view);
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, third, 4);
    h_nvnTextureWriteTexels(&recycled, NULL, &region, pixels);
    sample(305, bytes); check("packaged reused pool owns fresh upload", bytes, third, 0);
    sample(300, bytes); check("packaged old frozen storage survives pool reuse", bytes, second, 0);
    sample(303, bytes); check("packaged old frozen swizzle survives pool reuse", bytes, swizzled_second, 0);
    // 未解析的最后一次 CPU 写在销池时深拷贝；改写已退役 backing 后只能读冻结快照。
    // Finalize snapshots the last unresolved CPU write; overwriting retired backing must not change the frozen snapshot.
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, fourth, 4);
    h_nvnTextureWriteTexels(&recycled, NULL, &region, pixels);
    h_nvnTextureFinalize(&recycled); h_nvnMemoryPoolFinalize(&pool);
    memset(memory, 0, 65536);
    sample(305, bytes); check("packaged frozen dirty snapshot survives backing overwrite", bytes, fourth, 0);
    @autoreleasepool {
        mtl_sub_begin(); pthread_mutex_lock(&g_rmu); id<MTLTexture> integer = tex_by_id(306); pthread_mutex_unlock(&g_rmu); finish();
        read_texture(integer, bytes); check("packaged frozen snapshot permits first PFV upgrade", bytes, fourth, 0);
    }
    sample(305, bytes); check("packaged frozen peer follows frozen PFV upgrade", bytes, fourth, 0);
    sample(301, bytes); check("packaged older frozen generation remains independent", bytes, second, 0);
    h_nvnTexturePoolFinalize(&descriptors);
    mtl_sub_begin(); finish(); dispatch_sync(g_feedback_q, ^{});
    mtl_sub_begin(); finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
    pthread_mutex_lock(&g_smu); unsigned after = g_upload_storage.n; pthread_mutex_unlock(&g_smu);
    printf("packaged pool finalization retires upload owners: %s before=%u after=%u\n", before == after ? "PASS" : "FAIL", before, after);
    failures += before != after;
    // no-copy backing 保留到进程退出，不让观测者早于 Metal 释放客体存储。
    // Keep no-copy backing until process exit so the observer cannot free guest storage before Metal.
}

int main(int argc, char **argv) { @autoreleasepool {
    int cpu_only = argc == 2 && !strcmp(argv[1], "--cpu-only");
    int format_only = argc == 2 && !strcmp(argv[1], "--format-only");
    int packaged_only = argc == 2 && !strcmp(argv[1], "--packaged-only");
    setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init(); state_reset();
    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint n [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};return float4(p[n],0,1); }\n"
        "struct Args { texture2d<float> source [[id(0)]]; };\n"
        "fragment float4 f(constant Args &a [[buffer(0)]]) { return a.source.read(uint2(0)); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    sample_shader = [NvShader new];
    sample_shader->fn = [library newFunctionWithName:@"f"];
    sample_shader->ae = [sample_shader->fn newArgumentEncoderWithBufferIndex:0];
    sample_shader->rf.nb = 1;
    sample_shader->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D};
    MTLRenderPipelineDescriptor *p = [MTLRenderPipelineDescriptor new];
    p.vertexFunction = [library newFunctionWithName:@"v"];
    p.fragmentFunction = sample_shader->fn;
    p.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    sample_pipeline = [g_dev newRenderPipelineStateWithDescriptor:p error:&error];
    if (!sample_pipeline) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    if (!cpu_only && !format_only) packaged_alias();
    if (packaged_only) { printf("packaged alias integration failures=%u\n", failures); return failures != 0; }

    void *memory = NULL;
    if (posix_memalign(&memory, getpagesize(), 65536)) abort();
    memset(memory, 0, 65536);
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    h_nvnMemoryPoolBuilderSetDefaults(&pb);
    h_nvnMemoryPoolBuilderSetStorage(&pb, memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
    NVNtexture textures[9] = {0};
    texture(&textures[0], &pool, 0, NVN_FORMAT_RGBA8);
    texture(&textures[1], &pool, 0, NVN_FORMAT_RGBA8_SRGB);
    texture(&textures[2], &pool, 0, NVN_FORMAT_RGBA8);
    texture(&textures[3], &pool, 4096, NVN_FORMAT_RGBA8);
    texture(&textures[4], &pool, 4096, NVN_FORMAT_RGBA8);
    texture(&textures[5], &pool, 8192, NVN_FORMAT_RGBA8);
    texture(&textures[6], &pool, 8192, NVN_FORMAT_RGBA8_SRGB);
    texture(&textures[7], &pool, 0, NVN_FORMAT_BGRA8);
    NVNtextureBuilder different = {0};
    h_nvnTextureBuilderSetDefaults(&different);
    h_nvnTextureBuilderSetTarget(&different, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&different, NVN_FORMAT_RGBA8_SRGB);
    h_nvnTextureBuilderSetSize2D(&different, 2, 2);
    h_nvnTextureBuilderSetStorage(&different, &pool, 0);
    if (!h_nvnTextureInitialize(&textures[8], &different)) abort();
    NVNtexturePool descriptors;
    h_nvnTexturePoolInitialize(&descriptors, &pool, 32768, 512);
    tex_pool_bind((const dpool_t *)&descriptors);
    for (int i = 0; i < 9; i++) h_nvnTexturePoolRegisterTexture(&descriptors, 257 + i, &textures[i], NULL);
    NVNtextureView integer_view = {0}, swizzle_view = {0};
    h_nvnTextureViewSetDefaults(&integer_view); h_nvnTextureViewSetFormat(&integer_view, NVN_FORMAT_R32UI);
    h_nvnTexturePoolRegisterTexture(&descriptors, 266, &textures[0], &integer_view);
    h_nvnTextureViewSetDefaults(&swizzle_view);
    h_nvnTextureViewSetSwizzle(&swizzle_view, NVN_TEXTURE_SWIZZLE_B, NVN_TEXTURE_SWIZZLE_G, NVN_TEXTURE_SWIZZLE_R, NVN_TEXTURE_SWIZZLE_A);
    h_nvnTexturePoolRegisterTexture(&descriptors, 267, &textures[0], &swizzle_view);
    h_nvnTexturePoolRegisterTexture(&descriptors, 268, &textures[1], NULL);
    h_nvnTexturePoolRegisterTexture(&descriptors, 270, &textures[5], &integer_view);
    const uint8_t zero[4] = {0}, pixel[4] = {128, 64, 200, 255};
    uint8_t bytes[64], pixels[64];
    for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, pixel, 4);
    NVNcopyRegion region = {0, 0, 0, 4, 4, 1};
    format_swizzle(&pool, &descriptors, memory);
    if (format_only) { printf("format swizzle integration failures=%u\n", failures); return failures != 0; }
    const uint8_t linear[4] = {55, 13, 147, 255};
    const uint8_t second[4] = {32, 128, 240, 255}, second_linear[4] = {4, 55, 222, 255};
    id<MTLTexture> old_linear = nil, old_srgb = nil;
    if (!cpu_only) {
        sample(258, bytes); check("sRGB alias initially zero", bytes, zero, 0);
        sample(259, bytes); check("identical alias initially zero", bytes, zero, 0);
        sample(264, bytes); check("other format initially independent", bytes, zero, 0);
        sample(265, bytes); check("different dimensions initially independent", bytes, zero, 0);
        gpu_write(&textures[0], memory, pixels, region);
        sample(259, bytes); check("GPU write visible through identical alias", bytes, pixel, 0);
        // 手写 sRGB 解码期望，允许一次 UNORM 输出量化误差，不用被测格式转换 helper 生成期望。
        // Hand-written sRGB decode expectation allows one UNORM quantization step, without deriving expectations through the format helper under test.
        sample(258, bytes); check("GPU write visible through sRGB alias", bytes, linear, 1);
        sample(264, bytes); check("other format remains independent", bytes, zero, 0);
        sample(265, bytes); check("different dimensions remain independent", bytes, zero, 0);
        sample(262, bytes); check("reverse linear alias initially zero", bytes, zero, 0);
        gpu_write(&textures[6], memory, pixels, region);
        sample(262, bytes); check("sRGB GPU write visible through linear alias", bytes, pixel, 0);
        sample(263, bytes); check("sRGB GPU write retains decoded interpretation", bytes, linear, 1);
        pthread_mutex_lock(&g_rmu);
        old_linear = tex_by_id(257); old_srgb = tex_by_id(258);
        pthread_mutex_unlock(&g_rmu);
        // 通过真实整数格式描述符触发 PFV：输入是线性视图，真实底图首先以 sRGB 创建。
        // Trigger PFV through a real integer-format descriptor: input is a linear view of storage first created as sRGB.
        id<MTLBuffer> old_readback = [g_dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
        res_add(old_readback);
        mtl_sub_begin(); pthread_mutex_lock(&g_rmu);
        id<MTLTexture> integer = tex_by_id(266);
        pthread_mutex_unlock(&g_rmu);
        // 紧接升级拷贝读回旧视图，覆盖同编码器屏障与在途分配保留。
        // Read old views immediately after the upgrade copy, covering intra-encoder barriers and in-flight allocation retention.
        id<MTL4ComputeCommandEncoder> old_reads = blit();
        enc_b_copy_texture_buffer(old_reads, old_linear, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), old_readback, 0, 16, 64);
        enc_b_copy_texture_buffer(old_reads, old_srgb, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), old_readback, 64, 16, 64);
        finish();
        check("old linear view survives upgrade in-flight fence", old_readback.contents, pixel, 0);
        check("old sRGB view survives upgrade in-flight fence", (const uint8_t *)old_readback.contents + 64, pixel, 0);
        defer_obj_release(old_readback);
        read_texture(integer, bytes); check("PFV integer view preserves bytes", bytes, pixel, 0);
        sample(257, bytes); check("PFV keeps linear alias", bytes, pixel, 0);
        sample(258, bytes); check("PFV keeps sRGB alias", bytes, linear, 1);
        const uint8_t swizzled[4] = {200, 64, 128, 255};
        sample(267, bytes); check("ordinary swizzle view remains correct", bytes, swizzled, 0);
        for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, second, 4);
        gpu_write(&textures[1], memory, pixels, region);
        sample(257, bytes); check("PFV family still shares later GPU writes", bytes, second, 0);
        sample(258, bytes); check("PFV sRGB interpretation after later write", bytes, second_linear, 1);
        for (int i = 0; i < 16; i++) memcpy(pixels + 4 * i, pixel, 4);
    }
    sample(261, bytes); check("CPU peer initially zero", bytes, zero, 0);
    h_nvnTextureWriteTexels(&textures[3], NULL, &region, pixels);
    sample(260, bytes); check("WriteTexels visible through writer", bytes, pixel, 0);
    sample(261, bytes); check("WriteTexels visible through same-storage peer", bytes, pixel, 0);
    h_nvnTextureFinalize(&textures[3]); h_nvnTextureFinalize(&textures[4]);
    sample(261, bytes); check("CPU peer descriptor survives both API objects", bytes, pixel, 0);
    // 销池后 descriptor 仍持有冻结内容，不能因共享 CPU owner 退役而丢掉有效槽。
    // Descriptors keep frozen contents after pool finalization; retiring a shared CPU owner must not erase valid slots.
    h_nvnMemoryPoolFinalize(&pool);
    sample(261, bytes); check("CPU peer descriptor survives pool finalize", bytes, pixel, 0);
    if (!cpu_only) {
        sample(257, bytes); check("frozen linear alias preserves GPU contents", bytes, second, 0);
        sample(258, bytes); check("frozen sRGB alias preserves interpretation", bytes, second_linear, 1);
        sample(268, bytes); check("uncached frozen sRGB descriptor preserves interpretation", bytes, second_linear, 1);
        sample(263, bytes); check("frozen linear-root sRGB alias preserves interpretation", bytes, linear, 1);
        sample(262, bytes); check("frozen linear-root linear alias preserves bytes", bytes, pixel, 0);
        // 销池后才首次请求整数视图：共享冻结 owner 换根，已有 sRGB 槽也必须重建到新分配。
        // First request an integer view after pool finalization: replace the shared frozen owner's root and rebuild existing sRGB slots onto it.
        id<MTLBuffer> frozen_old_readback = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
        res_add(frozen_old_readback);
        mtl_sub_begin(); pthread_mutex_lock(&g_rmu);
        id<MTLTexture> frozen_old_srgb = tex_by_id(263);
        id<MTLTexture> frozen_integer = tex_by_id(270);
        pthread_mutex_unlock(&g_rmu);
        enc_b_copy_texture_buffer(blit(), frozen_old_srgb, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), frozen_old_readback, 0, 16, 64);
        finish();
        check("frozen old sRGB view survives PFV in-flight fence", frozen_old_readback.contents, pixel, 0);
        defer_obj_release(frozen_old_readback);
        read_texture(frozen_integer, bytes); check("frozen PFV integer view preserves bytes", bytes, pixel, 0);
        sample(262, bytes); check("frozen PFV linear alias preserves bytes", bytes, pixel, 0);
        sample(263, bytes); check("frozen PFV sRGB alias preserves interpretation", bytes, linear, 1);
    }
    // 同地址新池实例不继承旧写标记或上传记录，旧 frozen descriptor 仍读原来的内容。
    // A new pool instance at the same address inherits neither write markers nor upload records; old frozen descriptors retain their original contents.
    memset(memory, 0, 65536);
    if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
    NVNtexture recycled = {0};
    texture(&recycled, &pool, 4096, NVN_FORMAT_RGBA8);
    h_nvnTexturePoolRegisterTexture(&descriptors, 269, &recycled, NULL);
    sample(269, bytes); check("reused pool address has fresh texture contents", bytes, zero, 0);
    sample(261, bytes); check("old frozen peer survives pool address reuse", bytes, pixel, 0);
    if (!cpu_only) { sample(268, bytes); check("old frozen sRGB survives pool address reuse", bytes, second_linear, 1); }
    h_nvnTexturePoolFinalize(&descriptors);
    for (int i = 0; i < 9; i++) if (i != 3 && i != 4) h_nvnTextureFinalize(&textures[i]);
    h_nvnTextureFinalize(&recycled); h_nvnMemoryPoolFinalize(&pool);
    printf("texture alias integration failures=%u\n", failures);
    return failures != 0;
} }
