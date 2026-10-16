// 通过真实 NVN 录制和 Metal 回放验证条件 clear/draw；像素期望独立手算。
// Verify conditional clear/draw through real NVN recording and Metal replay; pixel expectations are calculated independently.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static int failures, checks;
static void expect(const char *name, int ok) {
    ++checks; failures += !ok;
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
}

int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    mtl_init();
    void *memory = NULL;
    if (posix_memalign(&memory, getpagesize(), 65536)) abort();
    memset(memory, 0, 65536);
    NVNmemoryPoolBuilder pb;
    NVNmemoryPool pool;
    h_nvnMemoryPoolBuilderSetDefaults(&pb);
    h_nvnMemoryPoolBuilderSetStorage(&pb, memory, 65536);
    h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
    NVNtextureBuilder tb = {0};
    NVNtexture target;
    h_nvnTextureBuilderSetDefaults(&tb);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetFormat(&tb, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&tb, 8, 8);
    h_nvnTextureBuilderSetLevels(&tb, 1);
    h_nvnTextureBuilderSetStorage(&tb, &pool, 0);
    if (!h_nvnTextureInitialize(&target, &tb)) abort();

    NSError *error = nil;
    id<MTLLibrary> library = [g_dev newLibraryWithSource:
        @"#include <metal_stdlib>\nusing namespace metal;\n"
        "vertex float4 v(uint i [[vertex_id]]) { float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)}; return float4(p[i],0.5,1); }\n"
        "fragment float4 f() { return float4(0,1,0,1); }\n"
        options:nil error:&error];
    if (!library) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    NVNprogram program;
    h_nvnProgramInitialize(&program, NULL);
    ((prog_t *)&program)->serial = nvn_new_serial();
    NvProg *native = [NvProg new];
    native->sh[0] = [NvShader new]; native->sh[0]->fn = [library newFunctionWithName:@"v"];
    native->sh[1] = [NvShader new]; native->sh[1]->fn = [library newFunctionWithName:@"f"];
    native->sh[1]->stage = 1; native->sh[1]->rf.color_mask = 1;
    serial_put(((prog_t *)&program)->serial, native);

    const struct { const char *name; int conditional, mode, equal, enabled, gpu_write; } cases[] = {
        {"enabled", 0, 1, 0, 1, 0},
        {"disabled", 0, 0, 0, 0, 0},
        {"equal / equal operands", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL, 1, 1, 0},
        {"equal / unequal operands", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL, 0, 0, 0},
        {"not equal / equal operands", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 1, 0, 0},
        {"not equal / unequal operands", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 0, 1, 0},
        {"GPU write makes operands equal", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL, 0, 1, 1},
        {"GPU write makes not-equal false", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 0, 0, 1},
        {"previous submission GPU write makes equal true", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_EQUAL, 0, 1, 2},
        {"previous submission GPU write makes not-equal false", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 0, 0, 2},
        {"TIMESTAMP_TOP report makes not-equal false", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 0, 0, 4},
        {"TIMESTAMP report makes not-equal false", 1, NVN_CONDITIONAL_RENDER_MODE_RENDER_IF_NOT_EQUAL, 0, 0, 5},
    };
    const float blue[4] = {0,0,1,1}, red[4] = {1,0,0,1};
    const uint8_t blue8[4] = {0,0,255,255}, red8[4] = {255,0,0,255}, green8[4] = {0,255,0,255};
    const NVNtexture *targets[] = {&target};
    NVNcopyRegion region = {0,0,0,8,8,1};
    state_reset();
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        // 比较值都超出 32 位，时间戳故意不同；只允许比较首尾两个 64 位值。
        // Compare values exceed 32 bits and the timestamp differs deliberately; only the first and last 64-bit values count.
        uint64_t *predicate = (uint64_t *)((uint8_t *)memory + 8192);
        predicate[0] = 0x100000001ull;
        predicate[1] = 0xdeadbeefcafef00dull;
        predicate[2] = cases[c].equal ? predicate[0] : 0x200000001ull;
        if (cases[c].gpu_write) predicate[2] = 0;
        if (cases[c].gpu_write >= 4) { predicate[0] = 1; predicate[1] = 0; }
        memset((uint8_t *)memory + 32768, 0xa5, 512);
        NVNcommandBuffer command;
        h_nvnCommandBufferInitialize(&command, NULL);
        h_nvnCommandBufferAddCommandMemory(&command, &pool, 16384, 16384);
        h_nvnCommandBufferBeginRecording(&command);
        h_nvnCommandBufferSetRenderEnable(&command, 1);
        h_nvnCommandBufferSetRenderTargets(&command, 1, targets, NULL, NULL, NULL);
        h_nvnCommandBufferSetViewport(&command, 0, 0, 8, 8);
        h_nvnCommandBufferSetScissor(&command, 0, 0, 8, 8);
        h_nvnCommandBufferClearColor(&command, 0, blue, 15);
        h_nvnCommandBufferBindProgram(&command, &program, 3);
        if (cases[c].gpu_write == 1 || cases[c].gpu_write == 2)
            h_nvnCommandBufferClearBuffer(&command, (NVNbufferAddress)predicate, 8, 0);
        if (cases[c].gpu_write == 2) {
            // 生产者先提交但 CPU 不等待；下一提交的条件必须自己等待相关写入。
            // Submit the producer without a CPU wait; the next submission's condition must wait for the relevant write itself.
            NVNcommandHandle producer = h_nvnCommandBufferEndRecording(&command);
            mtl_sub_begin();
            mtl_exec((const uint8_t *)producer);
            (void)mtl_sub_commit();
            h_nvnCommandBufferBeginRecording(&command);
        }
        if (cases[c].gpu_write >= 4)
            h_nvnCommandBufferReportCounter(&command,
                cases[c].gpu_write == 4 ? NVN_COUNTER_TYPE_TIMESTAMP_TOP : NVN_COUNTER_TYPE_TIMESTAMP,
                (NVNbufferAddress)predicate);
        if (cases[c].conditional)
            h_nvnCommandBufferSetRenderEnableConditional(&command, cases[c].mode, (NVNbufferAddress)predicate);
        else h_nvnCommandBufferSetRenderEnable(&command, cases[c].mode);
        h_nvnCommandBufferClearColor(&command, 0, red, 15);
        h_nvnCommandBufferSetScissor(&command, 0, 0, 4, 8);
        h_nvnCommandBufferDrawArrays(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        h_nvnCommandBufferCopyTextureToBuffer(&command, &target, NULL, &region,
            (NVNbufferAddress)((uint8_t *)memory + 32768), 0);
        h_nvnCommandBufferSetRenderEnable(&command, 1);
        h_nvnCommandBufferSetScissor(&command, 4, 0, 4, 8);
        h_nvnCommandBufferDrawArrays(&command, NVN_DRAW_PRIMITIVE_TRIANGLES, 0, 3);
        h_nvnCommandBufferCopyTextureToBuffer(&command, &target, NULL, &region,
            (NVNbufferAddress)((uint8_t *)memory + 33024), 0);
        NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&command);
        mtl_sub_begin();
        mtl_exec((const uint8_t *)handle);
        int done = mtl_wait(mtl_sub_commit(), 5000000000ull);
        expect("Metal submission completes", done);
        if (!done) return 2;
        expect("Metal feedback has no error", !mtl_last_commit_error());
        if (cases[c].gpu_write >= 4) {
            expect("timestamp report counter value is zero", predicate[0] == 0);
            expect("timestamp report timestamp field is nonzero", predicate[1] != 0);
            expect("timestamp report preserves second comparison value", predicate[2] == 0);
        }
        unsigned mismatch[2] = {0,0};
        for (int step = 0; step < 2; ++step) for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x) {
            const uint8_t *want = cases[c].enabled ? (x < 4 ? green8 : red8) : blue8;
            if (step && x >= 4) want = green8;
            const uint8_t *got = (uint8_t *)memory + 32768 + step*256 + (y*8+x)*4;
            mismatch[step] += memcmp(want, got, 4) != 0;
        }
        printf("%s: conditional mismatches=%u/64; re-enabled mismatches=%u/64\n", cases[c].name, mismatch[0], mismatch[1]);
        for (int step = 0; step < 2; ++step) {
            const uint8_t *p = (uint8_t *)memory + 32768 + step*256;
            printf("  readback%d left=%u,%u,%u,%u right=%u,%u,%u,%u\n", step,
                p[0],p[1],p[2],p[3],p[16],p[17],p[18],p[19]);
        }
        expect(cases[c].name, !mismatch[0]);
        expect("SetRenderEnable(true) restores subsequent draw", !mismatch[1]);
        h_nvnCommandBufferFinalize(&command);
    }
    h_nvnTextureFinalize(&target);
    h_nvnProgramFinalize(&program);
    h_nvnMemoryPoolFinalize(&pool);
    // 等待延迟退役完成前保留 no-copy backing，由进程退出回收。
    // Keep no-copy backing alive through deferred retirement; process exit releases it.
    printf("RENDER_ENABLE_GPU checks=%d failures=%d\n", checks, failures);
    return failures != 0;
} }
