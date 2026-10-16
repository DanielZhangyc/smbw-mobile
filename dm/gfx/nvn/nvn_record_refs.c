// 记录地址和存储键分别建哈希表；4 KiB 逻辑页倒排索引避免每条命令扫描所有旧记录。
// Hash record addresses and storage keys separately; a reverse index of logical 4 KiB pages avoids scanning all old records for each command.
#include "nvn_record_refs.h"
#include "nvn_hmap.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct { const pool_t *pool; int64_t offset; nvn_texdesc descriptor; } storage_key;
typedef struct { storage_key key; size_t references; } storage_entry;
typedef struct record_entry record_entry;
typedef struct page_entry page_entry;
typedef struct page_link {
    record_entry *record;
    page_entry *page;
    struct page_link *previous, *next, *record_next;
} page_link;
struct page_entry { uintptr_t number; page_link *head; };
typedef struct record_texture {
    storage_entry *storage;
    struct record_texture *next;
} record_texture;
struct record_entry {
    uintptr_t begin, end;
    record_texture *textures;
    page_link *pages;
};

static _Atomic int g_enabled;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_records = { .klen = sizeof(uintptr_t) };
static nvn_hmap g_pages = { .klen = sizeof(uintptr_t) };
static nvn_hmap g_storage = { .klen = sizeof(storage_key) };
static size_t g_references;
static uint64_t g_page_lookups, g_record_checks;
enum { PAGE_SHIFT = 12 };

static void invalid(const char *reason) {
    fprintf(stderr, "[dm] 【红】NVN record refs: %s\n", reason);
    abort();
}
static void *allocate(size_t size) {
    void *value = calloc(1, size);
    if (!value) invalid("allocation failed");
    return value;
}
static uintptr_t range_end(const void *address, size_t size) {
    uintptr_t begin = (uintptr_t)address;
    if (!begin || !size || size > UINTPTR_MAX - begin) invalid("invalid byte range");
    return begin + size;
}
static storage_key make_key(const pool_t *pool, int64_t offset, const nvn_texdesc *descriptor) {
    storage_key key = {0};
    key.pool = pool; key.offset = offset; memcpy(&key.descriptor, descriptor, sizeof key.descriptor);
    return key;
}
void nvn_record_refs_enable(void) { atomic_store_explicit(&g_enabled, 1, memory_order_release); }

// 删除同时摘除所有页链和存储计数；索引不拥有没有记录引用的历史键。
// Remove all page links and storage counts together; indexes never own historical keys without record references.
static void remove_record(record_entry *record) {
    nvn_hm_del(&g_records, &record->begin);
    while (record->pages) {
        page_link *link = record->pages;
        page_entry *page = link->page;
        record->pages = link->record_next;
        if (link->previous) link->previous->next = link->next;
        else page->head = link->next;
        if (link->next) link->next->previous = link->previous;
        if (!page->head) { nvn_hm_del(&g_pages, &page->number); free(page); }
        free(link);
    }
    while (record->textures) {
        record_texture *texture = record->textures;
        storage_entry *storage = texture->storage;
        record->textures = texture->next;
        if (!--storage->references) { nvn_hm_del(&g_storage, &storage->key); free(storage); }
        g_references--;
        free(texture);
    }
    free(record);
}

void nvn_record_refs_add(const void *header, const tex_t *texture) {
    if (!atomic_load_explicit(&g_enabled, memory_order_acquire)) return;
    if (!texture) invalid("missing texture snapshot");
    range_end(header, sizeof(op_hdr));
    op_hdr record_header; memcpy(&record_header, header, sizeof record_header);
    if (record_header.size < sizeof record_header) invalid("invalid command record size");
    uintptr_t begin = (uintptr_t)header, end = range_end(header, record_header.size), snapshot = (uintptr_t)texture;
    if (snapshot < begin || snapshot > end || sizeof *texture > end - snapshot)
        invalid("texture snapshot lies outside its record");
    storage_key key = make_key(texture->pool, texture->off, &texture->d);
    pthread_mutex_lock(&g_mutex);
    record_entry *record = nvn_hm_get(&g_records, &begin);
    if (record && record->end != end) invalid("record changed without an overwrite notification");
    if (!record) {
        record = allocate(sizeof *record); record->begin = begin; record->end = end;
        nvn_hm_put(&g_records, &begin, record);
        uintptr_t first = begin >> PAGE_SHIFT, last = (end - 1) >> PAGE_SHIFT;
        for (uintptr_t number = first;; number++) {
            page_entry *page = nvn_hm_get(&g_pages, &number);
            if (!page) { page = allocate(sizeof *page); page->number = number; nvn_hm_put(&g_pages, &number, page); }
            page_link *link = allocate(sizeof *link);
            link->record = record; link->page = page; link->next = page->head;
            if (page->head) page->head->previous = link;
            page->head = link; link->record_next = record->pages; record->pages = link;
            if (number == last) break;
        }
    }
    storage_entry *storage = nvn_hm_get(&g_storage, &key);
    if (storage) for (record_texture *reference = record->textures; reference; reference = reference->next)
        if (reference->storage == storage) { pthread_mutex_unlock(&g_mutex); return; }
    if (!storage) {
        storage = allocate(sizeof *storage); storage->key = key;
        nvn_hm_put(&g_storage, &storage->key, storage);
    }
    if (storage->references == SIZE_MAX || g_references == SIZE_MAX) invalid("reference count overflow");
    record_texture *reference = allocate(sizeof *reference);
    reference->storage = storage; reference->next = record->textures; record->textures = reference;
    storage->references++; g_references++;
    pthread_mutex_unlock(&g_mutex);
}

void nvn_record_refs_overwrite(const void *address, size_t size) {
    if (!atomic_load_explicit(&g_enabled, memory_order_acquire) || !size) return;
    uintptr_t begin = (uintptr_t)address, end = range_end(address, size);
    pthread_mutex_lock(&g_mutex);
    if (g_records.n) {
        uintptr_t first = begin >> PAGE_SHIFT, last = (end - 1) >> PAGE_SHIFT;
        for (uintptr_t number = first;; number++) {
            g_page_lookups++;
            page_entry *page = nvn_hm_get(&g_pages, &number);
            page_link *link = page ? page->head : NULL;
            while (link) {
                record_entry *record = link->record;
                page_link *next = link->next;
                g_record_checks++;
                if (record->begin < end && record->end > begin) remove_record(record);
                link = next;
            }
            if (number == last) break;
        }
    }
    pthread_mutex_unlock(&g_mutex);
}

int nvn_record_refs_contains(const pool_t *pool, int64_t offset, const nvn_texdesc *descriptor) {
    if (!atomic_load_explicit(&g_enabled, memory_order_acquire)) return 0;
    if (!descriptor) invalid("missing texture descriptor");
    storage_key key = make_key(pool, offset, descriptor);
    pthread_mutex_lock(&g_mutex);
    int found = nvn_hm_get(&g_storage, &key) != NULL;
    pthread_mutex_unlock(&g_mutex);
    return found;
}
void nvn_record_refs_get_stats(nvn_record_ref_stats *out) {
    if (!out) invalid("missing statistics destination");
    pthread_mutex_lock(&g_mutex);
    *out = (nvn_record_ref_stats){ .records = g_records.n, .references = g_references,
        .storage_keys = g_storage.n, .pages = g_pages.n,
        .overwrite_page_lookups = g_page_lookups, .overwrite_record_checks = g_record_checks };
    pthread_mutex_unlock(&g_mutex);
}
