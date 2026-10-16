// 真实 NVN 录制回归：缺省仅用虚拟池和生产宿主补段，不初始化 Metal；--metal 补测真实池覆盖。
// Real NVN recording regression: virtual pools and production host segments avoid Metal initialization by default; --metal also tests writes into real pools.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

static unsigned failures, checks;
static void expect(const char *name, size_t wanted, size_t observed) {
    printf("record refs %s: %s expected=%zu observed=%zu\n", name, wanted == observed ? "PASS" : "FAIL", wanted, observed);
    checks++; failures += wanted != observed;
}
static void require(int condition) { if (!condition) abort(); }
static void make_pool(NVNmemoryPool *pool, NVNdevice *device, void *memory, size_t size) {
    NVNmemoryPoolBuilder builder;
    h_nvnMemoryPoolBuilderSetDefaults(&builder);
    h_nvnMemoryPoolBuilderSetDevice(&builder, device);
    h_nvnMemoryPoolBuilderSetStorage(&builder, memory, size);
    h_nvnMemoryPoolBuilderSetFlags(&builder, memory
        ? NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED
        : NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
    require(h_nvnMemoryPoolInitialize(pool, &builder));
}
static void make_texture(NVNtexture *texture, NVNdevice *device, NVNmemoryPool *pool, ptrdiff_t offset) {
    NVNtextureBuilder builder;
    h_nvnTextureBuilderSetDefaults(&builder); h_nvnTextureBuilderSetDevice(&builder, device);
    h_nvnTextureBuilderSetTarget(&builder, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&builder, 16); h_nvnTextureBuilderSetHeight(&builder, 16);
    h_nvnTextureBuilderSetFormat(&builder, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&builder, pool, offset);
    require(h_nvnTextureInitialize(texture, &builder));
}
static int contains(const tex_t *texture) {
    return nvn_record_refs_contains(texture->pool, texture->off, &texture->d);
}
static uint8_t *targets(NVNcommandBuffer *command, NVNtexture *a, NVNtexture *b, int count) {
    const NVNtexture *textures[8];
    for (int i = 0; i < count; i++) textures[i] = i == 1 && b ? b : a;
    h_nvnCommandBufferSetRenderTargets(command, count, textures, NULL, NULL, NULL);
    size_t size = (8 + SNAP_OFF(p_set_rt) + (size_t)(count + 1) * sizeof(tex_t) + 7) & ~(size_t)7;
    uint8_t *header = CB(command)->cur - size;
    op_hdr actual; memcpy(&actual, header, sizeof actual);
    require(actual.op == OP_SET_RT && actual.size == size);
    return header;
}
static uint8_t *record_targets(NVNcommandBuffer *command, NVNtexture *a, NVNtexture *b) {
    h_nvnCommandBufferBeginRecording(command);
    uint8_t *record = targets(command, a, b, b ? 2 : 1);
    h_nvnCommandBufferEndRecording(command);
    return record;
}
static void erase_record(uint8_t *header) {
    op_hdr actual; memcpy(&actual, header, sizeof actual);
    nvn_record_refs_overwrite(header, actual.size);
    memset(header, 0, actual.size);
}
static void empty_stats(const char *stage) {
    nvn_record_ref_stats stats; nvn_record_refs_get_stats(&stats);
    expect(stage, 0, stats.records + stats.references + stats.storage_keys + stats.pages);
}

typedef struct { NVNdevice *device; NVNtexture *texture; const tex_t *key; unsigned failed; } worker_args;
static void *worker(void *opaque) {
    worker_args *args = opaque;
    NVNcommandBuffer command; require(h_nvnCommandBufferInitialize(&command, args->device));
    for (unsigned i = 0; i < 256; i++) {
        uint8_t *record = record_targets(&command, args->texture, NULL);
        args->failed += !contains(args->key);
        erase_record(record);
    }
    h_nvnCommandBufferFinalize(&command);
    return NULL;
}

static void cpu_cases(NVNdevice *device, NVNtexture *a, NVNtexture *b) {
    tex_t key_a = *CTX(a), key_b = *CTX(b);
    NVNcommandBuffer command; require(h_nvnCommandBufferInitialize(&command, device));
    record_targets(&command, a, b);
    expect("disabled recorder", 0, contains(&key_a)); empty_stats("disabled retains no metadata");
    nvn_record_refs_enable();
    uint8_t *record = record_targets(&command, a, b);
    NVNcommandHandle child = (NVNcommandHandle)CB(&command)->begin;
    expect("recorded first storage", 1, contains(&key_a)); expect("recorded second storage", 1, contains(&key_b));
    NVNcommandBuffer parent; require(h_nvnCommandBufferInitialize(&parent, device));
    h_nvnCommandBufferBeginRecording(&parent);
    for (unsigned i = 0; i < 64; i++) h_nvnCommandBufferCallCommands(&parent, 1, &child);
    h_nvnCommandBufferEndRecording(&parent);
    expect("repeated CALL preserves child storage", 1, contains(&key_a));
    nvn_record_ref_stats stats; nvn_record_refs_get_stats(&stats);
    expect("CALL does not duplicate texture metadata", 2, stats.references);
    h_nvnCommandBufferAddCommandMemory(&parent, NULL, 0, 0);
    expect("host-segment grant without writes preserves child", 1, contains(&key_a));
    h_nvnCommandBufferFinalize(&command);
    expect("command object finalization preserves recorded bytes", 1, contains(&key_b));
    nvn_record_refs_overwrite(record + 1, 1); record[1] ^= 0x80;
    expect("one-byte header overwrite removes first storage", 0, contains(&key_a));
    expect("one-byte header overwrite removes second storage", 0, contains(&key_b));
    empty_stats("partial overwrite reclaims all record metadata");

    require(h_nvnCommandBufferInitialize(&command, device));
    record = record_targets(&command, a, b);
    op_hdr header; memcpy(&header, record, sizeof header);
    nvn_record_refs_overwrite(record + header.size, 1);
    expect("adjacent byte leaves record intact", 1, contains(&key_a));
    nvn_record_ref_stats before; nvn_record_refs_get_stats(&before);
    uint8_t unrelated_byte = 0;
    for (unsigned i = 0; i < 1024; i++) {
        nvn_record_refs_overwrite(&unrelated_byte, 1); unrelated_byte++;
    }
    nvn_record_refs_get_stats(&stats);
    expect("unrelated-page writes never scan texture records", 0, stats.overwrite_record_checks - before.overwrite_record_checks);
    expect("unrelated writes preserve recorded storage", 1, contains(&key_a));
    erase_record(record);
    uint8_t *first = record_targets(&command, a, NULL), *second = record_targets(&command, a, NULL);
    erase_record(first);
    expect("another record keeps the same storage", 1, contains(&key_a));
    erase_record(second); empty_stats("last record releases shared storage key");

    h_nvnCommandBufferBeginRecording(&command);
    // 用真实 Barrier 把下条多纹理记录推到逻辑页尾，验证第二页任意字节覆盖也能找回整条记录。
    // Real Barrier records position the next multi-texture record across a logical page; a byte in page two must find the complete record.
    while (((uintptr_t)CB(&command)->cur & 4095) < 3500 || ((uintptr_t)CB(&command)->cur & 4095) > 3900)
        h_nvnCommandBufferBarrier(&command, 0);
    record = targets(&command, a, b, 8);
    h_nvnCommandBufferEndRecording(&command);
    memcpy(&header, record, sizeof header);
    require((uintptr_t)record >> 12 != ((uintptr_t)record + header.size - 1) >> 12);
    nvn_record_refs_get_stats(&stats); expect("eight slots deduplicate to two keys", 2, stats.references);
    nvn_record_refs_overwrite(record + header.size - 1, 1); record[header.size - 1] ^= 1;
    empty_stats("second-page overwrite removes full record");

    NVNtexture original = {0};
    make_texture(&original, device, (NVNmemoryPool *)key_a.pool, 12288);
    tex_t old_key = *CTX(&original);
    record = record_targets(&command, &original, NULL);
    h_nvnTextureFinalize(&original);
    expect("TextureFinalize preserves recorded metadata", 1, contains(&old_key));
    NVNtextureBuilder replacement;
    h_nvnTextureBuilderSetDefaults(&replacement); h_nvnTextureBuilderSetDevice(&replacement, device);
    h_nvnTextureBuilderSetTarget(&replacement, NVN_TEXTURE_TARGET_2D);
    h_nvnTextureBuilderSetWidth(&replacement, 32); h_nvnTextureBuilderSetHeight(&replacement, 16);
    h_nvnTextureBuilderSetFormat(&replacement, NVN_FORMAT_RGBA8);
    h_nvnTextureBuilderSetStorage(&replacement, (NVNmemoryPool *)key_a.pool, 12288);
    require(h_nvnTextureInitialize(&original, &replacement));
    expect("API object reuse cannot change old snapshot key", 1, contains(&old_key));
    expect("unrecorded replacement has no record reference", 0, contains(CTX(&original)));
    erase_record(record); h_nvnTextureFinalize(&original);
    empty_stats("API reuse leaves no historical serial metadata");

    pthread_t threads[4]; worker_args args[4];
    for (unsigned i = 0; i < 4; i++) {
        args[i] = (worker_args){device, a, &key_a, 0};
        require(!pthread_create(&threads[i], NULL, worker, &args[i]));
    }
    for (unsigned i = 0; i < 4; i++) { require(!pthread_join(threads[i], NULL)); expect("concurrent registration/query/overwrite", 0, args[i].failed); }
    empty_stats("concurrent recording leaves no historical keys");
    expect("CPU cases never initialize Metal", 1, g_dev == nil);
    h_nvnCommandBufferFinalize(&command); h_nvnCommandBufferFinalize(&parent);
}

static void metal_cases(NVNdevice *device, NVNtexture *texture) {
    void *memory = NULL; require(!posix_memalign(&memory, 16384, 65536)); memset(memory, 0, 65536);
    NVNmemoryPool pool; make_pool(&pool, device, memory, 65536);
    tex_t key = *CTX(texture);
    NVNcommandBuffer command; require(h_nvnCommandBufferInitialize(&command, device));
    h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 65536);
    record_targets(&command, texture, NULL);
    h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 65536);
    expect("overlapping real grant without writes preserves storage", 1, contains(&key));
    h_nvnCommandBufferBeginRecording(&command);
    expect("writing adjacent BEGIN preserves texture record", 1, contains(&key));
    h_nvnCommandBufferBarrier(&command, 0);
    expect("actual cmd_alloc overwrites old record", 0, contains(&key));
    h_nvnCommandBufferEndRecording(&command);

    h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 65536);
    record_targets(&command, texture, NULL);
    h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 65536);
    h_nvnCommandBufferBeginRecording(&command);
    expect("old record survives before JUMP write", 1, contains(&key));
    h_nvnCommandBufferAddCommandMemory(&command, &pool, 32768, 32768);
    expect("actual segment JUMP overwrites old record", 0, contains(&key));
    h_nvnCommandBufferEndRecording(&command);
    h_nvnCommandBufferAddCommandMemory(&command, &pool, 0, 65536);
    record_targets(&command, texture, NULL);
    h_nvnMemoryPoolFinalize(&pool);
    expect("real command-memory pool finalization clears refs", 0, contains(&key));
    empty_stats("real pool leaves no metadata");
    h_nvnCommandBufferFinalize(&command);
    mtl_sub_begin(); uint64_t fence = mtl_sub_commit();
    require(mtl_wait(fence, 10000000000ull)); mtl_finish();
    dispatch_sync(g_feedback_q, ^{}); defer_drain();
    free(memory);
}
int main(int argc, char **argv) { @autoreleasepool {
    int metal = argc == 2 && !strcmp(argv[1], "--metal");
    if (argc != 1 && !metal) return 2;
    NVNdevice device; NVNdeviceBuilder builder;
    h_nvnDeviceBuilderSetDefaults(&builder); require(h_nvnDeviceInitialize(&device, &builder));
    NVNmemoryPool texture_pool; make_pool(&texture_pool, &device, NULL, 1u << 20);
    NVNtexture a = {0}, b = {0};
    make_texture(&a, &device, &texture_pool, 4096); make_texture(&b, &device, &texture_pool, 8192);
    cpu_cases(&device, &a, &b);
    if (metal) metal_cases(&device, &a);
    h_nvnTextureFinalize(&a); h_nvnTextureFinalize(&b); h_nvnMemoryPoolFinalize(&texture_pool);
    h_nvnDeviceFinalize(&device);
    printf("record refs checks=%u failures=%u metal=%d\n", checks, failures, metal);
    return failures ? 1 : 0;
} }
