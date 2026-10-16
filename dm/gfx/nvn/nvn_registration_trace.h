// 可关闭的 Metal 资源登记追踪；记录宿主对象，不代表驱动 malloc 或 GPU 内存实际回收。
// Optional Metal registration tracing observes host objects, not driver mallocs or actual GPU memory reclamation.
// DM_NVN_ALLOC_TRACE=1 才启用；stack 是首次 res_add 的调用栈，type 为 0 其他 / 1 buffer / 2 texture。
// Only DM_NVN_ALLOC_TRACE=1 enables tracing; stack belongs to the first res_add, and type is 0 other / 1 buffer / 2 texture.
// retirement 表示撤驻留，object_dealloc 表示关联 observer 随宿主对象析构；两者均不证明 GPU 物理内存已释放。
// retirement means residency removal, and object_dealloc observes host-object destruction; neither proves GPU physical memory was freed.
// allocated_size 是首次登记时对象报告的大小；视图可能重叠，应按 root / buffer 归属分析，不能直接求和。
// allocated_size is reported at first registration; views may overlap, so attribute by root / buffer instead of summing object sizes.
#pragma once
#import <Metal/Metal.h>
#include <stdint.h>

typedef struct {
    uint64_t objects, registrations, retirements, object_deallocations, dropped_records;
} nvn_registration_trace_stats;

int nvn_registration_trace_enabled(void);
void nvn_registration_trace_add(id<MTLResource> resource);
void nvn_registration_trace_remove(id<MTLResource> resource);
uint64_t nvn_registration_trace_id(id<MTLResource> resource);
nvn_registration_trace_stats nvn_registration_trace_snapshot(void);
