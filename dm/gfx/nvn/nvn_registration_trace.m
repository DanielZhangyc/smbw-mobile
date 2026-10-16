#import "nvn_registration_trace.h"
#import <objc/runtime.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum { TRACE_STACK_MAX = 16 };
static pthread_once_t trace_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t trace_install_mu = PTHREAD_MUTEX_INITIALIZER;
static int trace_enabled;
static char trace_association_key;
static _Atomic uint64_t trace_next_id, trace_sequence, trace_objects, trace_registrations, trace_retirements, trace_deallocations, trace_dropped;
static _Thread_local int trace_writing;

// 每条记录只 write 一次；析构路径不使用 Foundation、不取追踪锁，也不触碰已经销毁的资源。
// Write each record once; destruction uses no Foundation or tracing lock and never touches the dying resource.
static void trace_write(const char *record, size_t size) {
    if (trace_writing || !size || size >= 2048) { atomic_fetch_add(&trace_dropped, 1); return; }
    trace_writing = 1;
    ssize_t written = write(STDERR_FILENO, record, size);
    if (written != (ssize_t)size) atomic_fetch_add(&trace_dropped, 1);
    trace_writing = 0;
}
static void trace_init(void) {
    const char *value = getenv("DM_NVN_ALLOC_TRACE");
    trace_enabled = value && !strcmp(value, "1");
    if (!trace_enabled) return;
    Dl_info info = {0};
    if (!dladdr((const void *)&nvn_registration_trace_add, &info)) abort();
    char record[2048];
    int n = snprintf(record, sizeof record, "[nvn-registration-trace] {\"event\":\"image\",\"base\":%llu,\"anchor\":%llu}\n", (unsigned long long)(uintptr_t)info.dli_fbase, (unsigned long long)(uintptr_t)&nvn_registration_trace_add);
    if (n <= 0 || (size_t)n >= sizeof record) abort();
    trace_write(record, (size_t)n);
}
int nvn_registration_trace_enabled(void) { pthread_once(&trace_once, trace_init); return trace_enabled; }

// 关联对象只存值，不回指资源；resource → observer 是单向所有权，不能形成环。
// The association stores values only: resource -> observer is one-way ownership and cannot form a retain cycle.
@interface NvRegistrationObserver : NSObject {
@public uint64_t identity, address, bytes, parent, root, buffer;
    unsigned type, stack_count;
    uintptr_t stack[TRACE_STACK_MAX];
    _Atomic unsigned resident;
    int armed;
}
@end

static void trace_event(NvRegistrationObserver *__unsafe_unretained observer, const char *event, int include_stack) {
    uint64_t thread = 0; pthread_threadid_np(NULL, &thread);
    char record[2048];
    int n = snprintf(record, sizeof record,
        "[nvn-registration-trace] {\"event\":\"%s\",\"seq\":%llu,\"time_ns\":%llu,\"thread\":%llu,\"id\":%llu,\"address\":%llu,\"type\":%u,\"allocated_size\":%llu,\"parent\":%llu,\"root\":%llu,\"buffer\":%llu,\"resident\":%u,\"stack\":[",
        event, (unsigned long long)(atomic_fetch_add(&trace_sequence, 1) + 1), (unsigned long long)clock_gettime_nsec_np(CLOCK_MONOTONIC), (unsigned long long)thread,
        (unsigned long long)observer->identity, (unsigned long long)observer->address, observer->type, (unsigned long long)observer->bytes,
        (unsigned long long)observer->parent, (unsigned long long)observer->root, (unsigned long long)observer->buffer, atomic_load(&observer->resident));
    if (n <= 0 || (size_t)n >= sizeof record) { atomic_fetch_add(&trace_dropped, 1); return; }
    for (unsigned i = 0; include_stack && i < observer->stack_count; i++) {
        int added = snprintf(record + n, sizeof record - (size_t)n, "%s%llu", i ? "," : "", (unsigned long long)observer->stack[i]);
        if (added <= 0 || (size_t)added >= sizeof record - (size_t)n) { atomic_fetch_add(&trace_dropped, 1); return; }
        n += added;
    }
    if ((size_t)n + 4 >= sizeof record) { atomic_fetch_add(&trace_dropped, 1); return; }
    memcpy(record + n, "]}\n", 3); n += 3;
    trace_write(record, (size_t)n);
}
@implementation NvRegistrationObserver
- (void)dealloc {
    if (armed) {
        atomic_fetch_add(&trace_deallocations, 1);
        trace_event(self, "object_dealloc", 0);
    }
}
@end

void nvn_registration_trace_add(id<MTLResource> resource) {
    if (!resource || !nvn_registration_trace_enabled()) return;
    @autoreleasepool {
        pthread_mutex_lock(&trace_install_mu);
        NvRegistrationObserver *observer = objc_getAssociatedObject(resource, &trace_association_key);
        int first = observer == nil;
        if (!observer) {
            observer = [NvRegistrationObserver new];
            observer->identity = atomic_fetch_add(&trace_next_id, 1) + 1;
            observer->address = (uintptr_t)(__bridge void *)resource;
            observer->bytes = resource.allocatedSize;
            observer->root = observer->address;
            observer->type = [resource conformsToProtocol:@protocol(MTLBuffer)] ? 1 : [resource conformsToProtocol:@protocol(MTLTexture)] ? 2 : 0;
            if (observer->type == 2) {
                id<MTLTexture> texture = (id<MTLTexture>)resource;
                observer->parent = (uintptr_t)(__bridge void *)texture.parentTexture;
                while (texture.parentTexture) texture = texture.parentTexture;
                observer->root = (uintptr_t)(__bridge void *)texture;
                observer->buffer = (uintptr_t)(__bridge void *)texture.buffer;
            }
            void *frames[TRACE_STACK_MAX];
            int count = backtrace(frames, TRACE_STACK_MAX);
            observer->stack_count = count > 0 ? (unsigned)count : 0;
            for (unsigned i = 0; i < observer->stack_count; i++) observer->stack[i] = (uintptr_t)frames[i];
            objc_setAssociatedObject(resource, &trace_association_key, observer, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            // Metal 实现必须支持关联；独立真实资源测试验证本机驱动和 debug wrapper，失败不能伪称已追踪。
            // Require association support; real-resource tests validate the local driver and debug wrapper, and unsupported objects fail explicitly.
            if (objc_getAssociatedObject(resource, &trace_association_key) != observer) abort();
            observer->armed = 1; atomic_fetch_add(&trace_objects, 1);
        }
        if (!atomic_exchange(&observer->resident, 1)) {
            atomic_fetch_add(&trace_registrations, 1);
            trace_event(observer, first ? "first_registration" : "registration", first);
        }
        pthread_mutex_unlock(&trace_install_mu);
    }
}
void nvn_registration_trace_remove(id<MTLResource> resource) {
    if (!resource || !nvn_registration_trace_enabled()) return;
    @autoreleasepool {
        pthread_mutex_lock(&trace_install_mu);
        NvRegistrationObserver *observer = objc_getAssociatedObject(resource, &trace_association_key);
        if (observer && atomic_exchange(&observer->resident, 0)) {
            atomic_fetch_add(&trace_retirements, 1);
            trace_event(observer, "retirement", 0);
        }
        pthread_mutex_unlock(&trace_install_mu);
    }
}
uint64_t nvn_registration_trace_id(id<MTLResource> resource) {
    if (!resource || !nvn_registration_trace_enabled()) return 0;
    @autoreleasepool {
        NvRegistrationObserver *observer = objc_getAssociatedObject(resource, &trace_association_key);
        return observer ? observer->identity : 0;
    }
}
nvn_registration_trace_stats nvn_registration_trace_snapshot(void) {
    return (nvn_registration_trace_stats){atomic_load(&trace_objects), atomic_load(&trace_registrations), atomic_load(&trace_retirements), atomic_load(&trace_deallocations), atomic_load(&trace_dropped)};
}
