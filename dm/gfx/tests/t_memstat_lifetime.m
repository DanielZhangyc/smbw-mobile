// 驻留弱表计数回归：统计时资源仍驻留，随后释放 owner，检查统计是否污染长寿命外层池。
// Residency weak-table census regression: count live resident resources, then release owners and check whether statistics contaminate the long-lived outer pool.
#ifndef NVN_MTL_IMPLEMENTATION
#define NVN_MTL_IMPLEMENTATION "../nvn/nvn_mtl.m"
#endif
#include NVN_MTL_IMPLEMENTATION
#include "../nvn/hle_nvn.c"

uint64_t g_t0_ns;
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t g_mod[MAX_MOD]; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }

@interface MemStatWeak : NSObject { @public __weak id<MTLResource> resource; } @end
@implementation MemStatWeak @end
static unsigned failures;
static void check_released(const char *stage, MemStatWeak *buffer, MemStatWeak *texture) {
    @autoreleasepool {
        int buffer_alive = buffer->resource != nil, texture_alive = texture->resource != nil;
        int ok = !buffer_alive && !texture_alive;
        printf("memstat lifetime %s: %s buffer_alive=%d texture_alive=%d\n", stage,
               ok ? "PASS" : "FAIL", buffer_alive, texture_alive);
        failures += !ok;
    }
}
static void run_case(int mode) {
    printf("memstat lifetime mode=%s\n", mode == 0 ? "control" : mode == 1 ? "weak count only" : "full mtl_mem_stat");
    MemStatWeak *buffer = [MemStatWeak new], *texture = [MemStatWeak new];
    @autoreleasepool {
        id<MTLBuffer> buffer_owner;
        id<MTLTexture> texture_owner;
        // 只隔离准备和清理自身的临时引用，计数及生产统计函数始终直接处于被测长寿命池。
        // Isolate only setup and cleanup temporaries; counting and production statistics run directly in the long-lived pool under test.
        @autoreleasepool {
            buffer_owner = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared];
            MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:128 height:128 mipmapped:NO];
            descriptor.storageMode = MTLStorageModePrivate; descriptor.usage = MTLTextureUsageShaderRead;
            texture_owner = [g_dev newTextureWithDescriptor:descriptor];
            if (!buffer_owner || !texture_owner) abort();
            buffer->resource = buffer_owner; texture->resource = texture_owner;
            res_add(buffer_owner); res_add(texture_owner); res_commit();
            pthread_mutex_lock(&g_res_mu);
            int registered = [g_res_objects containsObject:buffer_owner] && [g_res_objects containsObject:texture_owner];
            pthread_mutex_unlock(&g_res_mu);
            if (!registered) abort();
        }
        // 调用发生在两张真实分配仍活跃驻留、且明确 owner 尚存的时刻，不能拿空表代替。
        // Both real allocations are still resident with explicit owners when measured; an empty table cannot substitute for this scenario.
        if (mode == 1) {
            pthread_mutex_lock(&g_res_mu); unsigned long count = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
            printf("memstat live weak count=%lu (includes both test allocations)\n", count);
            if (count < 2) abort();
        } else if (mode == 2) mtl_mem_stat(3000, 0);
        @autoreleasepool {
            res_remove(buffer_owner); res_remove(texture_owner); res_commit();
            buffer_owner = nil; texture_owner = nil;
        }
        check_released("before outer pool drain", buffer, texture);
    }
    check_released("after outer pool drain", buffer, texture);
}
int main(void) { @autoreleasepool {
    setvbuf(stdout, NULL, _IONBF, 0);
    @autoreleasepool { mtl_init(); state_reset(); }
    g_allocprof = 1;
    printf("memstat lifetime device=%s allocprof=%d\n", g_dev.name.UTF8String, g_allocprof);
    run_case(0); run_case(1); run_case(2);
    printf("memstat lifetime failures=%u\n", failures);
    return failures ? 1 : 0;
} }
