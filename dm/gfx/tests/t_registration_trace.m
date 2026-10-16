// 真实 Metal 登记追踪：关联支持、重复/并发登记、弱引用寿命及真实 GPU 栅栏。
// Real Metal registration tracing: associations, repeated/concurrent registration, weak lifetimes, and an actual GPU fence.
#include "../nvn/nvn_mtl.m"
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface RegistrationWeak : NSObject { @public __weak id resource; } @end
@implementation RegistrationWeak @end
static unsigned failures;
static void check(const char *name, int ok) { printf("%s: %s\n", name, ok ? "PASS" : "FAIL"); failures += !ok; }
static void finish(uint64_t fence) {
    if (!mtl_wait(fence, 10000000000ull) || mtl_last_commit_error()) abort();
    mtl_finish(); dispatch_sync(g_feedback_q, ^{}); defer_drain(); res_commit();
}
static void drain(void) {
    @autoreleasepool { mtl_sub_begin(); finish(mtl_sub_commit()); }
    @autoreleasepool { mtl_sub_begin(); finish(mtl_sub_commit()); }
}
static void association_lifetime(int enabled) {
    RegistrationWeak *buffer_weak = [RegistrationWeak new], *texture_weak = [RegistrationWeak new], *view_weak = [RegistrationWeak new];
    nvn_registration_trace_stats before = nvn_registration_trace_snapshot();
    @autoreleasepool {
        id<MTLBuffer> buffer = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
        descriptor.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        id<MTLTexture> texture = [g_dev newTextureWithDescriptor:descriptor];
        id<MTLTexture> view = [texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
        if (!buffer || !texture || !view) abort();
        buffer_weak->resource = buffer; texture_weak->resource = texture; view_weak->resource = view;
        res_add(buffer); res_add(texture); res_add(view); res_commit();
        uint64_t bid = nvn_registration_trace_id(buffer), tid = nvn_registration_trace_id(texture), vid = nvn_registration_trace_id(view);
        check("real Metal buffer, texture, and format view support the association", enabled ? bid && tid && vid && bid != tid && tid != vid && bid != vid : !bid && !tid && !vid);
        // 只用真实资源与生产登记入口；并发调用结束后再观测计数，不伪造事件。
        // Use real resources and the production registration entry; inspect counters only after concurrent calls finish, never fabricate events.
        dispatch_apply(32, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) { (void)index; @autoreleasepool { res_add(buffer); } });
        check("repeated registration keeps the same observer", nvn_registration_trace_id(buffer) == bid);
        nvn_registration_trace_stats registered = nvn_registration_trace_snapshot();
        check("concurrent registration installs each observer exactly once", registered.objects - before.objects == (enabled ? 3 : 0) && registered.registrations - before.registrations == (enabled ? 3 : 0));
        res_remove(view); res_remove(texture); res_remove(buffer); res_commit();
        res_add(buffer); res_commit(); res_remove(buffer); res_commit();
        check("a retired resource keeps its original identity on re-registration", nvn_registration_trace_id(buffer) == bid);
    }
    @autoreleasepool {
        check("diagnostic associations do not extend any real Metal resource lifetime", !buffer_weak->resource && !texture_weak->resource && !view_weak->resource);
        nvn_registration_trace_stats after = nvn_registration_trace_snapshot();
        check("real retirement and object destruction counters match", after.objects - before.objects == (enabled ? 3 : 0) && after.registrations - before.registrations == (enabled ? 4 : 0) && after.retirements - before.retirements == (enabled ? 4 : 0) && after.object_deallocations - before.object_deallocations == (enabled ? 3 : 0));
    }
}
static void fence_lifetime(void) {
    RegistrationWeak *weak = [RegistrationWeak new];
    id<MTLBuffer> output = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent> gate = [g_dev newSharedEvent];
    if (!output || !gate) abort(); res_add(output);
    uint64_t fence, identity;
    @autoreleasepool {
        mtl_sub_begin();
        id<MTLBuffer> source = [g_dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        if (!source) abort(); memset(source.contents, 0x5a, 4096); res_add(source);
        weak->resource = source; identity = nvn_registration_trace_id(source);
        enc_b_copy_buffer(blit(), source, 0, output, 0, 4096);
        defer_obj_release(source);
        [g_q4 waitForEvent:gate value:1]; fence = mtl_sub_commit();
    }
    @autoreleasepool {
        pthread_mutex_lock(&g_mu); int pending = g_done < fence; pthread_mutex_unlock(&g_mu);
        check("real GPU fence retains a tracked buffer until completion", pending && weak->resource && nvn_registration_trace_id(weak->resource) == identity);
    }
    gate.signaledValue = 1; finish(fence); drain();
    @autoreleasepool { check("tracked buffer releases after the actual GPU fence", weak->resource == nil); }
    unsigned bad = 0; const uint8_t *bytes = output.contents;
    for (unsigned i = 0; i < 4096; i++) bad += bytes[i] != 0x5a;
    check("GPU copy reads the original tracked buffer contents", bad == 0);
    defer_obj_release(output); drain();
}
int main(int argc, char **argv) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    int enabled = !(argc == 2 && !strcmp(argv[1], "--disabled"));
    if (nvn_registration_trace_enabled() != enabled) { fprintf(stderr, "set DM_NVN_ALLOC_TRACE=%d for this invocation\n", enabled); return 2; }
    @autoreleasepool { mtl_init(); }
    printf("registration trace device=%s enabled=%d\n", g_dev.name.UTF8String, enabled);
    association_lifetime(enabled);
    if (enabled) fence_lifetime();
    nvn_registration_trace_stats stats = nvn_registration_trace_snapshot();
    check("registration tracing does not drop event records", stats.dropped_records == 0);
    printf("registration trace failures=%u\n", failures); return failures != 0;
} }
