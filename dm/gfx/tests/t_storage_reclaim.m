// 真实 Metal 与 NVN 录制验证：只回收已归还且无命令、描述符或出图所有者的缓存。
// Real Metal and NVN recording: reclaim only returned cache with no command, descriptor, or present owner.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static unsigned failures;
static void check(const char *name, int ok) { printf("%s: %s\n", name, ok ? "PASS" : "FAIL"); failures += !ok; }
static void finish(void) {
    uint64_t f = mtl_sub_commit();
    if (!mtl_wait(f, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
}
static void collect(void) {
    for (int i = 0; i < 64; i++) storage_lease_collect();
    for (int i = 0; i < 2; i++) { @autoreleasepool { mtl_sub_begin(); finish(); } }
}
static void texture(NVNtexture *out, NVNmemoryPool *pool, uint64_t offset, int width) {
    NVNtextureBuilder b; h_nvnTextureBuilderSetDefaults(&b);
    h_nvnTextureBuilderSetFormat(&b, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetSize2D(&b, width, width);
    h_nvnTextureBuilderSetStorage(&b, pool, offset);
    if (!h_nvnTextureInitialize(out, &b)) abort();
    mtl_storage_lease_begin(out);
}
static int cached(const NVNtexture *t) {
    alias_key key = alias_key_for((const tex_t *)t);
    pthread_mutex_lock(&g_smu); int found = hm_get_id(&g_alias, &key) != nil; pthread_mutex_unlock(&g_smu);
    return found;
}
static void return_texture(NVNtexture *t) {
    const tex_t *v = (const tex_t *)t;
    mtl_storage_lease_end(v->pool, v->off, v->L.size);
}
static void pressure(NVNmemoryPool *pool, unsigned start, unsigned count, int width) {
    for (unsigned i = start; i < start + count; i++) {
        NVNtexture t = {0}; texture(&t, pool, (uint64_t)i << 22, width);
        @autoreleasepool { mtl_sub_begin(); if (!tex_mtl((const tex_t *)&t)) abort(); finish(); }
        return_texture(&t); h_nvnTextureFinalize(&t);
    }
    collect();
}
int main(void) {
    @autoreleasepool {
        mtl_init(); nvn_record_refs_enable();
        NVNmemoryPool pool; NVNmemoryPoolBuilder pb;
        h_nvnMemoryPoolBuilderSetDefaults(&pb);
        h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_GPU_NO_ACCESS);
        h_nvnMemoryPoolBuilderSetStorage(&pb, NULL, 1ull << 30);
        if (!h_nvnMemoryPoolInitialize(&pool, &pb)) abort();
        NVNtexture pending = {0}, live = {0}, rebound = {0}, described = {0}, adjacent = {0}, peer_source = {0}, peer = {0};
        texture(&pending, &pool, 0, 4); texture(&live, &pool, 1ull << 22, 4);
        texture(&rebound, &pool, 2ull << 22, 4); texture(&described, &pool, 3ull << 22, 4);
        texture(&adjacent, &pool, 4ull << 22, 4);
        texture(&peer_source, &pool, 5ull << 22, 4);
        NVNtextureBuilder peer_builder; h_nvnTextureBuilderSetDefaults(&peer_builder);
        h_nvnTextureBuilderSetFormat(&peer_builder, NVN_FORMAT_RGBA8_SRGB);
        h_nvnTextureBuilderSetSize2D(&peer_builder, 4, 4);
        h_nvnTextureBuilderSetStorage(&peer_builder, &pool, 5ull << 22);
        if (!h_nvnTextureInitialize(&peer, &peer_builder)) abort();
        mtl_storage_lease_begin(&peer);
        NVNcommandBuffer cmd; h_nvnCommandBufferInitialize(&cmd, NULL);
        h_nvnCommandBufferAddCommandMemory(&cmd, NULL, 0, 0); h_nvnCommandBufferBeginRecording(&cmd);
        NVNtexture *targets[] = {&pending};
        h_nvnCommandBufferSetRenderTargets(&cmd, 1, (const NVNtexture *const *)targets, NULL, NULL, NULL);
        NVNcommandHandle handle = h_nvnCommandBufferEndRecording(&cmd);
        h_nvnCommandBufferFinalize(&cmd);
        mtl_reg_tex(312, (const tex_t *)&described, NULL);
        mtl_reg_tex(313, (const tex_t *)&peer, NULL);
        __weak id old;
        @autoreleasepool {
            mtl_sub_begin(); id<MTLTexture> p = tex_mtl((const tex_t *)&pending); old = p;
            tex_mtl((const tex_t *)&live); tex_mtl((const tex_t *)&rebound);
            tex_mtl((const tex_t *)&described); tex_mtl((const tex_t *)&adjacent);
            id<MTLTexture> linear = tex_mtl((const tex_t *)&peer_source);
            id<MTLBuffer> pixels = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
            if (!pixels) abort(); memset(pixels.contents, 0x5a, 64); res_add(pixels);
            enc_b_copy_buffer_texture(blit(), pixels, 0, 16, 64, MTLSizeMake(4, 4, 1), p, 0, 0, MTLOriginMake(0, 0, 0));
            enc_b_copy_buffer_texture(blit(), pixels, 0, 16, 64, MTLSizeMake(4, 4, 1), linear, 0, 0, MTLOriginMake(0, 0, 0));
            defer_obj_release(pixels); finish();
        }
        return_texture(&pending); return_texture(&rebound); return_texture(&described);
        return_texture(&peer_source);
        // 同对象快速再租借不触发 TextureInitialize；完成门必须恢复 live。
        // Fast reacquisition does not call TextureInitialize; the completion hook must restore live state.
        mtl_storage_lease_begin(&rebound);
        const tex_t *a = (const tex_t *)&adjacent;
        mtl_storage_lease_end(&pool, a->off - 64, 64);
        mtl_storage_lease_end(&pool, a->off, a->L.size / 2);
        pressure(&pool, 16, 32, 1024);
        check("unsubmitted recorded storage survives returned-cache pressure", old != nil && cached(&pending));
        check("live storage is outside idle-cache budget", cached(&live));
        check("same-object reacquisition cancels returned status", cached(&rebound));
        check("written descriptor retains returned storage", cached(&described));
        check("adjacent and partial free cannot retire a whole texture", cached(&adjacent));
        check("unresolved sRGB descriptor protects its linear allocation", cached(&peer_source));
        mtl_reg_tex(313, NULL, NULL); mtl_storage_lease_begin(&peer); collect();
        check("unresolved live sRGB lease protects its linear allocation", cached(&peer_source));
        @autoreleasepool {
            const uint8_t *record = (const uint8_t *)(uintptr_t)handle + 8;
            const p_set_rt *rt = (const p_set_rt *)(record + 8);
            id<MTLBuffer> output = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared];
            if (!output) abort(); res_add(output); mtl_sub_begin();
            id<MTLTexture> source = tex_mtl(rt->color[0]);
            enc_b_copy_texture_buffer(blit(), source, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), output, 0,16,64);
            finish(); const uint8_t *bytes = output.contents; unsigned bad = 0;
            for (int i = 0; i < 64; i++) bad += bytes[i] != 0x5a;
            check("late snapshot resolves original GPU contents", !bad);
            mtl_sub_begin(); source = tex_mtl((const tex_t *)&peer);
            enc_b_copy_texture_buffer(blit(), source, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), output, 0,16,64);
            finish(); bad = 0;
            for (int i = 0; i < 64; i++) bad += bytes[i] != 0x5a;
            check("late sRGB interpretation preserves original GPU bytes", !bad);
            defer_obj_release(output);
        }
        const uint8_t *record = (const uint8_t *)(uintptr_t)handle + 8;
        nvn_record_refs_overwrite(record, 1);
        mtl_reg_tex(312, NULL, NULL);
        mtl_reg_tex(313, NULL, NULL);
        return_texture(&peer);
        pressure(&pool, 48, 160, 4);
        check("overwritten record allows returned cache to retire", !cached(&pending) && old == nil);
        check("overwritten descriptor allows returned cache to retire", !cached(&described));
        check("idle historical storage stays within bounded cache", g_storage_leases.n <= 3 + 128);
        pressure(&pool, 112, 32, 1024);
        uint64_t idle_bytes = 0; unsigned idle_count = 0;
        for (uint32_t i = 0; i < g_storage_leases.cap; i++) if (g_storage_leases.vals[i] && !((storage_lease *)g_storage_leases.vals[i])->live) {
            alias_key key; memcpy(&key, g_storage_leases.keys + (size_t)i * sizeof key, sizeof key);
            id<MTLTexture> t = hm_get_id(&g_alias, &key); idle_bytes += t ? tex_allocation_root(t).allocatedSize : 0; idle_count++;
        }
        check("another allocation round cannot exceed idle byte or entry budget", idle_bytes <= (32ull << 20) && idle_count <= 128);
        NVNtexture large = {0}; texture(&large, &pool, 700ull << 20, 4096);
        __weak id fenced_texture; id<MTLSharedEvent> gate = [g_dev newSharedEvent]; uint64_t fence;
        if (!gate) abort();
        @autoreleasepool {
            mtl_sub_begin(); id<MTLTexture> t = tex_mtl((const tex_t *)&large); fenced_texture = t;
            return_texture(&large);
            for (int i = 0; i < 64; i++) storage_lease_collect();
            [g_q4 waitForEvent:gate value:1]; fence = mtl_sub_commit();
        }
        @autoreleasepool { check("evicted storage survives an unfinished real GPU fence", !cached(&large) && g_done < fence && fenced_texture != nil); }
        gate.signaledValue = 1;
        if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
        collect();
        @autoreleasepool { check("evicted storage releases after its GPU fence", fenced_texture == nil); }
        // 出图队列不依赖主提交 fence，整个交换链池显式排除。
        // The present queue has its own fence; explicitly exclude its whole pool.
        mtl_storage_preserve_pool(&pool); return_texture(&live); pressure(&pool, 80, 16, 1024);
        check("present-owned pool is not reclaimed", cached(&live));
        h_nvnMemoryPoolFinalize(&pool); collect();
        check("pool finalization removes lease metadata", g_storage_leases.n == 0);
        printf("storage reclaim failures=%u\n", failures);
    }
    return failures ? 1 : 0;
}
