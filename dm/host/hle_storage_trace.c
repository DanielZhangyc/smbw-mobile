// 记录 1.2.1 子分配边界；TRACE 只读，RECLAIM 另向图形层报告经过构建核验的租借开始与结束。
// Observe 1.2.1 suballocation boundaries; TRACE is read-only, while RECLAIM also reports build-verified lease starts and ends to graphics.
#include "dm.h"
#include "../gfx/nvn/nvn_mtl.h"
#include "../gfx/nvn/nvn_obj.h"
#include "../gfx/nvn/nvn_record_refs.h"
#include <errno.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <unistd.h>

static pthread_mutex_t g_storage_trace_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_storage_trace_sequence;
static __thread uint64_t g_storage_trace_thread;
static int g_storage_trace_enabled, g_storage_reclaim_enabled;
static uint64_t g_storage_main_base;
uint64_t dm_nvn_frame_count(void);

static void storage_trace_read(uint64_t address, void *destination, size_t size, const char *field) {
    if (!address || size > UINT64_MAX - address)
        die("DM_STORAGE_LIFETIME_TRACE 读取边界无效 field=%s address=%#" PRIx64 " size=%zu", field, address, size);
    vm_size_t received = 0;
    kern_return_t result = vm_read_overwrite(mach_task_self(), (vm_address_t)address,
                                           (vm_size_t)size, (vm_address_t)destination, &received);
    if (result != KERN_SUCCESS || received != size)
        die("DM_STORAGE_LIFETIME_TRACE 无法完整读取 field=%s address=%#" PRIx64 " size=%zu received=%zu kernel=%d",
            field, address, size, (size_t)received, result);
}

static uint64_t storage_trace_address(uint64_t base, uint64_t offset, const char *field) {
    if (!base || base > UINT64_MAX - offset)
        die("DM_STORAGE_LIFETIME_TRACE 字段地址无效 field=%s base=%#" PRIx64 " offset=%#" PRIx64, field, base, offset);
    return base + offset;
}

static uint64_t storage_trace_u64(const uint8_t *bytes, size_t offset) {
    uint64_t value; memcpy(&value, bytes + offset, sizeof value); return value;
}
static uint32_t storage_trace_u32(const uint8_t *bytes, size_t offset) {
    uint32_t value; memcpy(&value, bytes + offset, sizeof value); return value;
}
static uint16_t storage_trace_u16(const uint8_t *bytes, size_t offset) {
    uint16_t value; memcpy(&value, bytes + offset, sizeof value); return value;
}

// 模块名来自装载器，仍转义后写 JSON；不读取客体字符串，也不采集逐事件回溯。
// Module names come from the loader but are escaped for JSON; never read guest strings or collect per-event backtraces.
static void storage_trace_module_name(const module_t *module, char output[97]) {
    static const char hex[] = "0123456789abcdef";
    size_t written = 0;
    for (size_t i = 0; i < sizeof module->name && module->name[i]; i++) {
        unsigned char c = (unsigned char)module->name[i];
        if (c < 0x20 || c == '"' || c == '\\' || c >= 0x7f) {
            output[written++] = '\\'; output[written++] = 'u';
            output[written++] = '0'; output[written++] = '0';
            output[written++] = hex[c >> 4]; output[written++] = hex[c & 15];
        } else output[written++] = (char)c;
    }
    output[written] = 0;
}

static void storage_trace_event(const uint64_t *registers, int observation, const char *event,
                                uint64_t source_offset, uint64_t context, uint64_t block,
                                int inline_fields, uint64_t texture, uint64_t owner,
                                const tex_t *lease, uint64_t nvn_texture) {
    uint8_t block_bytes[0x80 - 0x10], context_bytes[0x38 - 0x20];
    uint32_t context_flags = 0;
    char block_fields[1200], context_fields[400], caller[300], inline_values[160], lease_fields[800], line[3400];
    if (!g_storage_trace_thread) {
        int error = pthread_threadid_np(NULL, &g_storage_trace_thread);
        if (error || !g_storage_trace_thread)
            die("DM_STORAGE_LIFETIME_TRACE 无法读取线程编号 error=%d", error);
    }

    // 锁仅排列日志，不取得客体锁；快照是观测点的原始内存值，不保证其他线程不并发修改。
    // This lock orders logging only, without taking guest locks; snapshots contain raw values at the hook and may race guest writes on other threads.
    pthread_mutex_lock(&g_storage_trace_mu);
    if (g_storage_trace_sequence == UINT64_MAX) die("DM_STORAGE_LIFETIME_TRACE 序号溢出");
    uint64_t sequence = ++g_storage_trace_sequence, time = now_ns();
    if (block) {
        storage_trace_read(storage_trace_address(block, 0x10, "block+0x10"), block_bytes, sizeof block_bytes, "block+0x10..0x80");
        int length = snprintf(block_fields, sizeof block_fields,
            "{\"range_offset_raw_u32_0x10\":%" PRIu32 ",\"span_raw_u64_0x28\":%" PRIu64
            ",\"texture_offset_raw_u32_0x30\":%" PRIu32 ",\"pool_id_raw_u32_0x34\":%" PRIu32
            ",\"raw_ptr_0x18\":\"0x%" PRIx64 "\",\"raw_ptr_0x20\":\"0x%" PRIx64
            "\",\"raw_ptr_0x38\":\"0x%" PRIx64 "\",\"raw_ptr_0x40\":\"0x%" PRIx64
            "\",\"raw_ptr_0x68\":\"0x%" PRIx64 "\",\"raw_u16_0x78\":%u,\"raw_u16_0x7a\":%u,\"raw_u32_0x7c\":%" PRIu32 "}",
            storage_trace_u32(block_bytes, 0x00), storage_trace_u64(block_bytes, 0x18),
            storage_trace_u32(block_bytes, 0x20), storage_trace_u32(block_bytes, 0x24),
            storage_trace_u64(block_bytes, 0x08), storage_trace_u64(block_bytes, 0x10),
            storage_trace_u64(block_bytes, 0x28), storage_trace_u64(block_bytes, 0x30),
            storage_trace_u64(block_bytes, 0x58), (unsigned)storage_trace_u16(block_bytes, 0x68),
            (unsigned)storage_trace_u16(block_bytes, 0x6a), storage_trace_u32(block_bytes, 0x6c));
        if (length < 0 || (size_t)length >= sizeof block_fields) die("DM_STORAGE_LIFETIME_TRACE block JSON 超长");
    } else {
        if (observation != OBS_STORAGE_ALLOC_RETURN && observation != OBS_STORAGE_LEASE_BEGIN && observation != OBS_STORAGE_LEASE_BEGIN_INLINE)
            die("DM_STORAGE_LIFETIME_TRACE 释放观测门收到空 block obs=%d", observation);
        strcpy(block_fields, "null");
    }
    if (context) {
        storage_trace_read(storage_trace_address(context, 0x20, "ctx+0x20"), context_bytes, sizeof context_bytes, "ctx+0x20..0x38");
        storage_trace_read(storage_trace_address(context, 0xd0, "ctx+0xd0"), &context_flags, sizeof context_flags, "ctx+0xd0");
        int length = snprintf(context_fields, sizeof context_fields,
            "{\"raw_ptr_0x20\":\"0x%" PRIx64 "\",\"used_raw_u64_0x28\":%" PRIu64
            ",\"raw_ptr_0x30\":\"0x%" PRIx64 "\",\"flags_raw_u32_0xd0\":%" PRIu32 "}",
            storage_trace_u64(context_bytes, 0), storage_trace_u64(context_bytes, 8),
            storage_trace_u64(context_bytes, 16), context_flags);
        if (length < 0 || (size_t)length >= sizeof context_fields) die("DM_STORAGE_LIFETIME_TRACE ctx JSON 超长");
    } else {
        if (observation != OBS_STORAGE_ALLOC_RETURN && observation != OBS_STORAGE_LEASE_BEGIN && observation != OBS_STORAGE_LEASE_BEGIN_INLINE)
            die("DM_STORAGE_LIFETIME_TRACE 释放观测门收到空 ctx obs=%d", observation);
        strcpy(context_fields, "null");
    }

    // 门帧 r[101] 是保存的原始客体 LR；r[30] 可能已被离线门桩改写。
    // Gate-frame r[101] preserves the original guest LR; r[30] may already have been changed by the offline gate stub.
    uint64_t guest_lr = registers[101];
    module_t *module = module_of((const void *)(uintptr_t)guest_lr);
    int length;
    if (module) {
        char module_name[97]; storage_trace_module_name(module, module_name);
        length = snprintf(caller, sizeof caller,
            "{\"guest_lr\":\"0x%" PRIx64 "\",\"module\":\"%s\",\"offset\":\"0x%" PRIx64 "\"}",
            guest_lr, module_name, guest_lr - (uint64_t)(uintptr_t)module->base);
    } else length = snprintf(caller, sizeof caller,
        "{\"guest_lr\":\"0x%" PRIx64 "\",\"module\":null,\"offset\":null}", guest_lr);
    if (length < 0 || (size_t)length >= sizeof caller) die("DM_STORAGE_LIFETIME_TRACE caller JSON 超长");
    if (inline_fields) length = snprintf(inline_values, sizeof inline_values,
        "{\"texture_raw\":\"0x%" PRIx64 "\",\"owner_raw\":\"0x%" PRIx64 "\"}", texture, owner);
    else length = snprintf(inline_values, sizeof inline_values, "null");
    if (length < 0 || (size_t)length >= sizeof inline_values) die("DM_STORAGE_LIFETIME_TRACE inline JSON 超长");
    if (lease) length = snprintf(lease_fields, sizeof lease_fields,
        "{\"nvn_texture\":\"0x%" PRIx64 "\",\"pool\":\"0x%" PRIx64 "\",\"off\":%" PRId64
        ",\"size\":%" PRIu64 ",\"serial\":%" PRIu64 ",\"target\":%d,\"format\":%d,\"flags\":%d,\"samples\":%d"
        ",\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"depth\":%" PRIu32 ",\"levels\":%" PRIu32
        ",\"stride\":%" PRIu32 ",\"pkg_block_h\":%d,\"has_layout\":%u,\"set_swz\":%u,\"swz\":%u}",
        nvn_texture, (uint64_t)(uintptr_t)lease->pool, lease->off, lease->L.size, lease->serial,
        lease->d.target, lease->d.format, lease->d.flags, lease->d.samples,
        lease->d.width, lease->d.height, lease->d.depth, lease->d.levels, lease->d.stride,
        lease->d.pkg_block_h, (unsigned)lease->has_layout, (unsigned)lease->set_swz, (unsigned)lease->swz);
    else length = snprintf(lease_fields, sizeof lease_fields, "null");
    if (length < 0 || (size_t)length >= sizeof lease_fields) die("DM_STORAGE_LIFETIME_TRACE lease JSON 超长");
    length = snprintf(line, sizeof line,
        "{\"kind\":\"storage_lifetime\",\"event\":\"%s\",\"seq\":%" PRIu64
        ",\"time_ns\":%" PRIu64 ",\"frame\":%" PRIu64 ",\"thread\":%" PRIu64 ",\"observation\":%d,\"source\":\"main+0x%" PRIx64
        "\",\"ctx\":\"0x%" PRIx64 "\",\"block\":\"0x%" PRIx64 "\",\"block_fields\":%s,\"ctx_fields\":%s,\"inline_registers\":%s,\"lease\":%s,\"caller\":%s}\n",
        event, sequence, time, dm_nvn_frame_count(), g_storage_trace_thread, observation, source_offset,
        context, block, block_fields, context_fields, inline_values, lease_fields, caller);
    if (length < 0 || (size_t)length >= sizeof line) die("DM_STORAGE_LIFETIME_TRACE event JSON 超长");
    ssize_t written = write(STDERR_FILENO, line, (size_t)length);
    if (written != length) {
        int error = errno;
        die("DM_STORAGE_LIFETIME_TRACE 日志写入不完整 seq=%" PRIu64 " expected=%d written=%zd errno=%d", sequence, length, written, error);
    }
    pthread_mutex_unlock(&g_storage_trace_mu);
}

static void storage_trace_release(const uint64_t *r, int observation, const char *event,
                                  uint64_t source, uint64_t context, uint64_t block,
                                  int inline_fields, uint64_t texture, uint64_t owner) {
    if (g_storage_trace_enabled)
        storage_trace_event(r, observation, event, source, context, block, inline_fields, texture, owner, NULL, 0);
    if (!g_storage_reclaim_enabled) return;
    uint8_t bytes[0x40];
    storage_trace_read(storage_trace_address(block, 0x10, "release block+0x10"), bytes, sizeof bytes, "release block range/pool");
    uint64_t offset = storage_trace_u32(bytes, 0), size = storage_trace_u64(bytes, 0x18), pool = storage_trace_u64(bytes, 0x28);
    if (!pool || !size || size > UINT64_MAX - offset)
        die("DM_STORAGE_CACHE_RECLAIM 释放区间无效 obs=%d block=%#" PRIx64 " pool=%#" PRIx64 " off=%#" PRIx64 " size=%#" PRIx64,
            observation, block, pool, offset, size);
    // 门内只报告租借结束；命令引用检查与按 GPU fence 退役由图形层负责。
    // The gate reports a lease end only; graphics owns command-reference checks and retirement by GPU fence.
    mtl_storage_lease_end((const void *)(uintptr_t)pool, offset, size);
}

static void storage_trace_free(const uint64_t *r) {
    storage_trace_release(r, OBS_STORAGE_FREE, "free", 0x77d2c, r[0], r[1], 0, 0, 0);
}
static void storage_trace_free_inline_a(const uint64_t *r) {
    storage_trace_release(r, OBS_STORAGE_FREE_INLINE_A, "free_inline_a", 0x1778f8, r[0], r[9], 1, r[21], r[20]);
}
static void storage_trace_free_inline_b(const uint64_t *r) {
    storage_trace_release(r, OBS_STORAGE_FREE_INLINE_B, "free_inline_b", 0x1f0b4f0, r[0], r[22], 1, r[28], r[27]);
}
static void storage_trace_alloc_return(const uint64_t *r) {
    if (g_storage_trace_enabled)
        storage_trace_event(r, OBS_STORAGE_ALLOC_RETURN, "alloc_return", 0x1fe268, 0, r[8], 0, 0, 0, NULL, 0);
}

static void storage_trace_lease(const uint64_t *r, int observation, const char *event,
                                uint64_t source, uint64_t texture) {
    uint8_t object[0xb8], block_bytes[0x30], table_fields[0x74];
    uint64_t block = 0, nvn_texture = 0;
    tex_t lease;
    if (!texture) {
        if (g_storage_trace_enabled)
            storage_trace_event(r, observation, event, source, 0, 0, 1, 0, 0, NULL, 0);
        return;
    }
    storage_trace_read(texture, object, sizeof object, "lease game texture");
    block = storage_trace_u64(object, 0xb0);
    uint32_t token = storage_trace_u32(object, 0x58);
    if (!block) {
        // 空返回不开始新租借；非空对象仅在无存储且 token 无效时才表示分配失败。
        // A null return starts no lease; a nonnull object represents allocation failure only with no storage and an invalid token.
        if (storage_trace_u64(object, 8) || token != UINT32_MAX)
            die("DM_STORAGE_LIFETIME_TRACE 非空返回缺 block obs=%d texture=%#" PRIx64 " pool=%#" PRIx64 " token=%#" PRIx32,
                observation, texture, storage_trace_u64(object, 8), token);
        if (g_storage_trace_enabled)
            storage_trace_event(r, observation, event, source, 0, 0, 1, texture, 0, NULL, 0);
        return;
    }
    if (token == UINT32_MAX)
        die("DM_STORAGE_LIFETIME_TRACE 成功租借 token 无效 obs=%d texture=%#" PRIx64, observation, texture);
    uint64_t state;
    storage_trace_read(storage_trace_address(g_storage_main_base, 0x37cf490, "texture table state"), &state, sizeof state, "texture table state");
    storage_trace_read(storage_trace_address(state, 0x530, "texture table metadata"), table_fields, sizeof table_fields, "texture table metadata");
    uint32_t index = token - storage_trace_u16(table_fields, 0x72), count = storage_trace_u32(table_fields, 0);
    uint64_t table = storage_trace_u64(table_fields, 8);
    // 复用分支不会再次 Initialize；按游戏已核验的 token 表读取最终 NVN 对象，越界不得误读槽 0。
    // Cached reuse skips Initialize; resolve the final NVN object through the verified guest token table, never map an invalid index to slot zero.
    if (index >= count)
        die("DM_STORAGE_LIFETIME_TRACE 租借 token 越界 obs=%d texture=%#" PRIx64 " token=%#" PRIx32 " index=%" PRIu32 " count=%" PRIu32,
            observation, texture, token, index, count);
    uint64_t entry = storage_trace_address(table, (uint64_t)index * 0x20 + 0x18, "texture table entry");
    storage_trace_read(entry, &nvn_texture, sizeof nvn_texture, "NVN texture pointer");
    storage_trace_read(nvn_texture, &lease, sizeof lease, "NVN texture snapshot");
    storage_trace_read(storage_trace_address(block, 0x10, "lease block+0x10"), block_bytes, sizeof block_bytes, "lease block storage");
    uint64_t pool = storage_trace_u64(block_bytes, 0x28);
    uint64_t offset = storage_trace_u32(block_bytes, 0x20);
    if (lease.magic != MAGIC('T', 'E', 'X', 0) || !lease.serial || !pool || lease.off < 0 || !lease.L.size ||
        (uint64_t)(uintptr_t)lease.pool != pool || (uint64_t)lease.off != offset)
        die("DM_STORAGE_LIFETIME_TRACE 租借 NVN 对象与 block 不一致 obs=%d texture=%#" PRIx64 " nvn=%#" PRIx64
            " magic=%#" PRIx32 " serial=%" PRIu64 " pool=%p/%#" PRIx64 " off=%" PRId64 "/%" PRIu64 " size=%" PRIu64,
            observation, texture, nvn_texture, lease.magic, lease.serial, (void *)lease.pool, pool, lease.off, offset, lease.L.size);
    if (g_storage_trace_enabled)
        storage_trace_event(r, observation, event, source, 0, block, 1, texture, 0, &lease, nvn_texture);
    // 完成门位于两条分配 wrapper 的返回前，取消精确 key 的 dead 标记后调用方才可继续录制。
    // Both gates precede allocation-wrapper returns, cancelling the exact key's dead marker before the caller can resume recording.
    if (g_storage_reclaim_enabled) mtl_storage_lease_begin(&lease);
}

static void storage_trace_lease_begin(const uint64_t *r) {
    storage_trace_lease(r, OBS_STORAGE_LEASE_BEGIN, "lease_begin", 0x1fd42c, r[27]);
}
static void storage_trace_lease_begin_inline(const uint64_t *r) {
    storage_trace_lease(r, OBS_STORAGE_LEASE_BEGIN_INLINE, "lease_begin_inline", 0x75258, r[19]);
}

void storage_trace_install(void) {
    const char *trace = getenv("DM_STORAGE_LIFETIME_TRACE"), *reclaim = getenv("DM_STORAGE_CACHE_RECLAIM");
    g_storage_trace_enabled = trace && !strcmp(trace, "1");
    g_storage_reclaim_enabled = reclaim && !strcmp(reclaim, "1");
    if (!g_storage_trace_enabled && !g_storage_reclaim_enabled) return;
    if (strcmp(dm_main_build(), "ff773e90972d544eb79406eaa65396d5"))
        die("DM_STORAGE_LIFETIME_TRACE 未核对 main 构建 %s", dm_main_build());
    module_t *main = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) main = &g_mod[i];
    if (!main || !main->base) die("DM_STORAGE_LIFETIME_TRACE 缺 main 映像");
    const int observations[] = { OBS_STORAGE_FREE, OBS_STORAGE_FREE_INLINE_A, OBS_STORAGE_FREE_INLINE_B,
        OBS_STORAGE_ALLOC_RETURN, OBS_STORAGE_LEASE_BEGIN, OBS_STORAGE_LEASE_BEGIN_INLINE };
    for (size_t i = 0; i < sizeof observations / sizeof *observations; i++)
        if (!gate_obs_present(observations[i]))
            die("DM_STORAGE_LIFETIME_TRACE / DM_STORAGE_CACHE_RECLAIM 缺观测门 %d；请用对应开关重新改写 main", observations[i]);
    g_storage_main_base = (uint64_t)(uintptr_t)main->base;
    if (g_storage_reclaim_enabled) nvn_record_refs_enable();
    gate_obs_set(OBS_STORAGE_FREE, storage_trace_free);
    gate_obs_set(OBS_STORAGE_FREE_INLINE_A, storage_trace_free_inline_a);
    gate_obs_set(OBS_STORAGE_FREE_INLINE_B, storage_trace_free_inline_b);
    gate_obs_set(OBS_STORAGE_ALLOC_RETURN, storage_trace_alloc_return);
    gate_obs_set(OBS_STORAGE_LEASE_BEGIN, storage_trace_lease_begin);
    gate_obs_set(OBS_STORAGE_LEASE_BEGIN_INLINE, storage_trace_lease_begin_inline);
    LOG("子分配生命周期门：1.2.1 trace=%d reclaim=%d，TRACE JSON 写 stderr，main=%p",
        g_storage_trace_enabled, g_storage_reclaim_enabled, (void *)main->base);
}
