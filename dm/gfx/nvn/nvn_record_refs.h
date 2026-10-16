// 命令记录的纹理存储引用：只保留键，不持有 GPU 对象或客体对象。
// Texture-storage references in command records: retain only keys, never GPU or guest objects.
#pragma once
#include <stddef.h>
#include "nvn_obj.h"

// 在安装存储生命周期钩子时启用；默认关闭，所有录制热路径仅检查原子开关。
// Enable when installing storage-lifetime hooks; disabled recording hot paths only check an atomic flag.
void nvn_record_refs_enable(void);

// 按 op_hdr.size 登记完整记录内的纹理快照；同记录的相同存储键去重，关闭时不读 header。
// Register a texture snapshot within the full op_hdr.size record; deduplicate identical storage keys in that record and never read header when disabled.
void nvn_record_refs_add(const void *header, const tex_t *texture);

// 在真正覆盖字节前调用；任意部分相交即移除该旧记录的全部引用，授予未写内存不调用。
// Call before actually overwriting bytes; any overlap removes all references in that old record. Do not call for unwritten memory grants.
void nvn_record_refs_overwrite(const void *address, size_t size);

// 按池对象、偏移与描述字节精确查询；查询、登记、覆盖在同一内部锁下串行。
// Query the exact pool object, offset and descriptor bytes; queries, registration and overwrites share one internal mutex.
int nvn_record_refs_contains(const pool_t *pool, int64_t offset, const nvn_texdesc *descriptor);

// 只读计数用于确认引用回收及索引工作量；哈希容量随同时存活记录的峰值增长。
// Read-only counts verify reclamation and index work; hash capacity grows with peak simultaneously live records.
typedef struct {
    size_t records, references, storage_keys, pages;
    uint64_t overwrite_page_lookups, overwrite_record_checks;
} nvn_record_ref_stats;
void nvn_record_refs_get_stats(nvn_record_ref_stats *out);
