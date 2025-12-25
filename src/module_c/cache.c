#include "module_c/cache.h"
#include "dedup.h"
#include "module_c/storage_monitor_basic.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

extern fs_state_t fs_state;

/* hash_table_* provided by module A */
hash_table_t *hash_table_create(size_t size);
void hash_table_destroy(hash_table_t *table);
int hash_table_set(hash_table_t *table, uint64_t key, void *value);
void *hash_table_get(hash_table_t *table, uint64_t key);
int hash_table_remove(hash_table_t *table, uint64_t key);
size_t hash_table_size(hash_table_t *table);

static multi_level_cache_t g_cache;
typedef struct l3_entry
{
    uint64_t block_id;
    size_t size;
    time_t last_access;
} l3_entry_t;

typedef struct l1_entry
{
    uint64_t block_id;
    size_t size;
    struct l1_entry *next;
} l1_entry_t;

static void *cache_flush_thread_fn(void *arg);

static pthread_t g_flush_thread;
static pthread_mutex_t g_flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_flush_cond = PTHREAD_COND_INITIALIZER;
static int g_flush_thread_running = 0;
static int g_flush_wakeup = 0;

static data_block_t *l2_alloc_block(uint64_t block_id, size_t size)
{
    data_block_t *blk = calloc(1, sizeof(data_block_t));
    if (!blk)
        return NULL;
    blk->block_id = block_id;
    blk->data = malloc(size);
    if (!blk->data)
    {
        free(blk);
        return NULL;
    }
    blk->size = size;
    blk->compressed_size = 0;
    blk->compression = COMPRESSION_NONE;
    blk->ref_count = 1;
    pthread_mutex_init(&blk->ref_lock, NULL);
    return blk;
}

static void l2_free_block(data_block_t *blk)
{
    if (!blk)
        return;
    if (blk->data)
        free(blk->data);
    pthread_mutex_destroy(&blk->ref_lock);
    free(blk);
}

static size_t l1_block_size(data_block_t *blk)
{
    if (!blk)
        return 0;
    if (blk->compressed_size > 0 && blk->compression != COMPRESSION_NONE)
        return blk->compressed_size;
    return blk->size;
}

static void l1_remove_entry(uint64_t block_id)
{
    l1_entry_t *prev = NULL;
    l1_entry_t *cur = g_cache.l1.head;
    while (cur)
    {
        if (cur->block_id == block_id)
        {
            if (prev)
                prev->next = cur->next;
            else
                g_cache.l1.head = cur->next;
            if (cur == g_cache.l1.tail)
                g_cache.l1.tail = prev;
            if (g_cache.l1.current_bytes >= cur->size)
                g_cache.l1.current_bytes -= cur->size;
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void l1_track_insert(uint64_t block_id, size_t size)
{
    l1_entry_t *ent = calloc(1, sizeof(l1_entry_t));
    if (!ent)
        return;
    ent->block_id = block_id;
    ent->size = size;
    if (g_cache.l1.tail)
        g_cache.l1.tail->next = ent;
    else
        g_cache.l1.head = ent;
    g_cache.l1.tail = ent;
    g_cache.l1.current_bytes += size;
}

static void l1_evict_until_fit(size_t incoming)
{
    while (g_cache.l1.max_bytes &&
           g_cache.l1.current_bytes + incoming > g_cache.l1.max_bytes &&
           g_cache.l1.head)
    {
        l1_entry_t *victim = g_cache.l1.head;
        g_cache.l1.head = victim->next;
        if (!g_cache.l1.head)
            g_cache.l1.tail = NULL;
        hash_table_remove(g_cache.l1.table, victim->block_id);
        if (g_cache.l1.current_bytes >= victim->size)
            g_cache.l1.current_bytes -= victim->size;
        free(victim);
    }
}

static void l1_clear(void)
{
    l1_entry_t *cur = g_cache.l1.head;
    while (cur)
    {
        l1_entry_t *next = cur->next;
        free(cur);
        cur = next;
    }
    g_cache.l1.head = g_cache.l1.tail = NULL;
    g_cache.l1.current_bytes = 0;
}

static void cache_stats_set_dirty(void)
{
    if (!g_cache.l2.dirty_flags)
    {
        smb_cache_set_l2_dirty(0, g_cache.l2.slots);
        return;
    }
    size_t dirty = 0;
    for (size_t i = 0; i < g_cache.l2.slots; i++)
    {
        if (g_cache.l2.dirty_flags[i])
            dirty++;
    }
    smb_cache_set_l2_dirty(dirty, g_cache.l2.slots);
}

static size_t l2_dirty_count(void)
{
    if (!g_cache.l2.dirty_flags)
        return 0;
    size_t dirty = 0;
    for (size_t i = 0; i < g_cache.l2.slots; i++)
        dirty += g_cache.l2.dirty_flags[i] ? 1 : 0;
    return dirty;
}

static void cache_stats_set_usage(void)
{
    uint64_t l1_bytes = g_cache.l1.current_bytes;
    uint64_t l2_bytes = (uint64_t)g_cache.l2.slots * (uint64_t)g_cache.l2.slot_size;
    uint64_t l3_bytes = g_cache.l3.current_bytes;
    smb_cache_set_usage(l1_bytes, l2_bytes, l3_bytes);
}

/* Copy a block into an L2 slot, decompressing if needed. */
static int l2_copy_into_slot(size_t slot, data_block_t *block)
{
    if (!block)
        return -EINVAL;

    char *src = block->data;
    char *tmp = NULL;
    size_t src_size = block->size;

    if (block->compressed_size > 0 && block->compression != COMPRESSION_NONE)
    {
        size_t plain_size = 0;
        if (block_decompress(block, &tmp, &plain_size) != 0)
            return -EIO;
        src = tmp;
        src_size = plain_size;
    }

    size_t copy_sz = src_size < g_cache.l2.slot_size ? src_size : g_cache.l2.slot_size;
    if (g_cache.l2.slot_blocks[slot] == NULL)
        g_cache.l2.slot_blocks[slot] = l2_alloc_block(block->block_id, g_cache.l2.slot_size);

    if (g_cache.l2.slot_blocks[slot])
    {
        data_block_t *dst = g_cache.l2.slot_blocks[slot];
        memcpy(dst->data, src, copy_sz);
        dst->size = copy_sz;
        dst->compressed_size = 0;
        dst->compression = COMPRESSION_NONE;
    }

    if (g_cache.l2.map && g_cache.l2.map != MAP_FAILED)
    {
        memcpy((char *)g_cache.l2.map + slot * g_cache.l2.slot_size, src, copy_sz);
    }

    if (tmp)
        free(tmp);

    return 0;
}

static int l2_init(size_t capacity)
{
    if (capacity == 0)
        return 0;
    size_t slot_size = fs_state.block_size ? fs_state.block_size : DEFAULT_BLOCK_SIZE;
    if (capacity < slot_size * 4)
        return 0; /* too small, treat as disabled */

    g_cache.l2.slot_size = slot_size;
    g_cache.l2.capacity_bytes = capacity;
    g_cache.l2.slots = capacity / slot_size;
    snprintf(g_cache.l2.backing_path, sizeof(g_cache.l2.backing_path), "/tmp/smartbackupfs_l2.cache");

    g_cache.l2.fd = open(g_cache.l2.backing_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (g_cache.l2.fd < 0)
        return -1;
    if (ftruncate(g_cache.l2.fd, (off_t)capacity) != 0)
    {
        close(g_cache.l2.fd);
        g_cache.l2.fd = -1;
        return -1;
    }
    g_cache.l2.map = mmap(NULL, capacity, PROT_READ | PROT_WRITE, MAP_SHARED, g_cache.l2.fd, 0);
    if (g_cache.l2.map == MAP_FAILED)
    {
        close(g_cache.l2.fd);
        g_cache.l2.fd = -1;
        g_cache.l2.map = NULL;
        return -1;
    }

    g_cache.l2.slot_ids = calloc(g_cache.l2.slots, sizeof(uint64_t));
    g_cache.l2.slot_blocks = calloc(g_cache.l2.slots, sizeof(data_block_t *));
    g_cache.l2.dirty_flags = calloc(g_cache.l2.slots, sizeof(uint8_t));
    g_cache.l2.index = hash_table_create(g_cache.l2.slots * 2);
    if (!g_cache.l2.slot_ids || !g_cache.l2.slot_blocks || !g_cache.l2.index || !g_cache.l2.dirty_flags)
        return -1;

    pthread_rwlock_init(&g_cache.l2.lock, NULL);
    g_cache.l2.enabled = 1;
    return 0;
}

static void l2_shutdown(void)
{
    if (g_cache.l2.slot_blocks)
    {
        for (size_t i = 0; i < g_cache.l2.slots; i++)
            l2_free_block(g_cache.l2.slot_blocks[i]);
        free(g_cache.l2.slot_blocks);
        g_cache.l2.slot_blocks = NULL;
    }
    if (g_cache.l2.dirty_flags)
    {
        free(g_cache.l2.dirty_flags);
        g_cache.l2.dirty_flags = NULL;
    }
    if (g_cache.l2.slot_ids)
    {
        free(g_cache.l2.slot_ids);
        g_cache.l2.slot_ids = NULL;
    }
    if (g_cache.l2.index)
    {
        hash_table_destroy(g_cache.l2.index);
        g_cache.l2.index = NULL;
    }
    if (g_cache.l2.map && g_cache.l2.map != MAP_FAILED)
    {
        munmap(g_cache.l2.map, g_cache.l2.capacity_bytes);
        g_cache.l2.map = NULL;
    }
    if (g_cache.l2.fd >= 0)
    {
        close(g_cache.l2.fd);
        g_cache.l2.fd = -1;
    }
    if (g_cache.l2.backing_path[0])
        unlink(g_cache.l2.backing_path);
    pthread_rwlock_destroy(&g_cache.l2.lock);
    g_cache.l2.enabled = 0;
}

static void l3_remove_entry(uint64_t block_id)
{
    if (!g_cache.l3.index)
        return;
    l3_entry_t *ent = (l3_entry_t *)hash_table_get(g_cache.l3.index, block_id);
    if (ent)
    {
        char path[512];
        snprintf(path, sizeof(path), "%s/%lu.bin", g_cache.l3.cache_dir, block_id);
        unlink(path);
        if (g_cache.l3.current_bytes >= ent->size)
            g_cache.l3.current_bytes -= ent->size;
        hash_table_remove(g_cache.l3.index, block_id);
        free(ent);
    }
}

static int l3_init(size_t capacity_bytes)
{
    if (capacity_bytes == 0)
        return 0;
    memset(&g_cache.l3, 0, sizeof(g_cache.l3));
    g_cache.l3.capacity_bytes = capacity_bytes;
    g_cache.l3.slot_size = fs_state.block_size ? fs_state.block_size : DEFAULT_BLOCK_SIZE;
    g_cache.l3.max_entries = capacity_bytes / g_cache.l3.slot_size;
    if (g_cache.l3.max_entries == 0)
        g_cache.l3.max_entries = 1;
    g_cache.l3.expire_seconds = 3600; /* default 1h */
    snprintf(g_cache.l3.cache_dir, sizeof(g_cache.l3.cache_dir), "/tmp/smartbackupfs_l3");
    mkdir(g_cache.l3.cache_dir, 0700);
    struct stat st = {0};
    if (stat(g_cache.l3.cache_dir, &st) != 0 || !S_ISDIR(st.st_mode))
        return -1;
    g_cache.l3.index = hash_table_create(g_cache.l3.max_entries * 2 + 1);
    if (!g_cache.l3.index)
        return -1;
    pthread_rwlock_init(&g_cache.l3.lock, NULL);
    return 0;
}

static void l3_shutdown(void)
{
    if (g_cache.l3.index)
    {
        /* remove files best-effort */
        pthread_rwlock_wrlock(&g_cache.l3.lock);
        for (size_t i = 0; i < g_cache.l3.index->size; i++)
        {
            hash_node_t *node = g_cache.l3.index->buckets[i];
            while (node)
            {
                l3_entry_t *ent = (l3_entry_t *)node->value;
                if (ent)
                {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/%lu.bin", g_cache.l3.cache_dir, ent->block_id);
                    unlink(path);
                    free(ent);
                }
                node = node->next;
            }
        }
        pthread_rwlock_unlock(&g_cache.l3.lock);
        hash_table_destroy(g_cache.l3.index);
        g_cache.l3.index = NULL;
        pthread_rwlock_destroy(&g_cache.l3.lock);
    }
    g_cache.l3.current_bytes = 0;
}

static data_block_t *l3_load_entry(l3_entry_t *ent)
{
    if (!ent)
        return NULL;
    char path[512];
    snprintf(path, sizeof(path), "%s/%lu.bin", g_cache.l3.cache_dir, ent->block_id);
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    data_block_t *blk = l2_alloc_block(ent->block_id, g_cache.l3.slot_size);
    if (!blk)
    {
        fclose(fp);
        return NULL;
    }
    size_t readn = fread(blk->data, 1, g_cache.l3.slot_size, fp);
    fclose(fp);
    blk->size = readn;
    blk->compressed_size = 0;
    blk->compression = COMPRESSION_NONE;
    return blk;
}

static void l3_evict_if_needed(size_t incoming_size)
{
    if (!g_cache.l3.index)
        return;
    while ((g_cache.l3.current_bytes + incoming_size > g_cache.l3.capacity_bytes) ||
           (hash_table_size(g_cache.l3.index) >= g_cache.l3.max_entries))
    {
        /* find oldest */
        l3_entry_t *oldest = NULL;
        pthread_rwlock_wrlock(&g_cache.l3.lock);
        for (size_t i = 0; i < g_cache.l3.index->size; i++)
        {
            hash_node_t *node = g_cache.l3.index->buckets[i];
            while (node)
            {
                l3_entry_t *ent = (l3_entry_t *)node->value;
                if (!oldest || (ent && ent->last_access < oldest->last_access))
                    oldest = ent;
                node = node->next;
            }
        }
        if (!oldest)
        {
            pthread_rwlock_unlock(&g_cache.l3.lock);
            break;
        }
        hash_table_remove(g_cache.l3.index, oldest->block_id);
        pthread_rwlock_unlock(&g_cache.l3.lock);
        l3_remove_entry(oldest->block_id);
    }
}

static data_block_t *l3_cache_get(uint64_t block_id)
{
    if (!g_cache.l3.index)
        return NULL;
    pthread_rwlock_wrlock(&g_cache.l3.lock);
    l3_entry_t *ent = (l3_entry_t *)hash_table_get(g_cache.l3.index, block_id);
    if (!ent)
    {
        pthread_rwlock_unlock(&g_cache.l3.lock);
        smb_cache_update_hits(3, 0);
        return NULL;
    }
    time_t now = time(NULL);
    if (g_cache.l3.expire_seconds && now - ent->last_access > (time_t)g_cache.l3.expire_seconds)
    {
        hash_table_remove(g_cache.l3.index, block_id);
        pthread_rwlock_unlock(&g_cache.l3.lock);
        l3_remove_entry(block_id);
        return NULL;
    }
    ent->last_access = now;
    pthread_rwlock_unlock(&g_cache.l3.lock);
    data_block_t *blk = l3_load_entry(ent);
    if (blk)
        smb_cache_update_hits(3, 1);
    return blk;
}

static int l3_cache_put(data_block_t *block)
{
    if (!block || !g_cache.l3.index || g_cache.l3.capacity_bytes == 0)
        return 0;
    size_t store_size = block->size < g_cache.l3.slot_size ? block->size : g_cache.l3.slot_size;
    l3_evict_if_needed(store_size);

    char path[512];
    snprintf(path, sizeof(path), "%s/%lu.bin", g_cache.l3.cache_dir, block->block_id);
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -errno;
    fwrite(block->data, 1, store_size, fp);
    fclose(fp);

    l3_entry_t *ent = (l3_entry_t *)hash_table_get(g_cache.l3.index, block->block_id);
    if (!ent)
    {
        ent = calloc(1, sizeof(l3_entry_t));
        if (!ent)
            return -ENOMEM;
        ent->block_id = block->block_id;
        hash_table_set(g_cache.l3.index, block->block_id, ent);
    }
    if (g_cache.l3.current_bytes >= ent->size)
        g_cache.l3.current_bytes -= ent->size;
    ent->size = store_size;
    ent->last_access = time(NULL);
    g_cache.l3.current_bytes += store_size;
    cache_stats_set_usage();
    return 0;
}

/* 清理过期的 L3 条目，避免陈旧缓存占用空间 */
static void l3_trim_expired(void)
{
    if (!g_cache.l3.index || g_cache.l3.expire_seconds == 0)
        return;

    size_t cap = hash_table_size(g_cache.l3.index);
    if (cap == 0)
        return;

    uint64_t *to_remove = calloc(cap, sizeof(uint64_t));
    if (!to_remove)
        return;

    size_t n = 0;
    time_t now = time(NULL);

    pthread_rwlock_rdlock(&g_cache.l3.lock);
    for (size_t i = 0; i < g_cache.l3.index->size; i++)
    {
        hash_node_t *node = g_cache.l3.index->buckets[i];
        while (node)
        {
            l3_entry_t *ent = (l3_entry_t *)node->value;
            if (ent && now - ent->last_access > (time_t)g_cache.l3.expire_seconds)
            {
                if (n < cap)
                    to_remove[n++] = ent->block_id;
            }
            node = node->next;
        }
    }
    pthread_rwlock_unlock(&g_cache.l3.lock);

    for (size_t i = 0; i < n; i++)
        l3_remove_entry(to_remove[i]);

    cache_stats_set_usage();
    free(to_remove);
}

int cache_system_init(size_t l1_bytes, size_t l2_bytes, size_t l3_bytes)
{
    g_cache.l1.max_bytes = l1_bytes ? l1_bytes : (64 * 1024 * 1024); /* default 64MB logical budget */
    g_cache.l1.table = hash_table_create(16384);
    if (!g_cache.l1.table)
        return -1;
    g_cache.l1.current_bytes = 0;
    g_cache.l1.head = NULL;
    g_cache.l1.tail = NULL;
    pthread_rwlock_init(&g_cache.l1.lock, NULL);
    g_cache.l3.capacity_bytes = l3_bytes;

    if (l2_init(l2_bytes) != 0)
        return -1;
    if (l3_init(l3_bytes) != 0)
        return -1;

    g_flush_thread_running = 1;
    if (pthread_create(&g_flush_thread, NULL, cache_flush_thread_fn, NULL) != 0)
    {
        g_flush_thread_running = 0;
    }
    cache_stats_set_usage();
    fs_state.l1_cache = &g_cache.l1;
    fs_state.l2_cache = &g_cache.l2;
    fs_state.l3_cache = &g_cache.l3;
    return 0;
}

void cache_system_shutdown(void)
{
    if (g_flush_thread_running)
    {
        g_flush_thread_running = 0;
        cache_flush_request();
        pthread_join(g_flush_thread, NULL);
    }
    if (g_cache.l1.table)
    {
        l1_clear();
        hash_table_destroy(g_cache.l1.table);
        g_cache.l1.table = NULL;
    }
    pthread_rwlock_destroy(&g_cache.l1.lock);

    l2_shutdown();
    l3_shutdown();
}

data_block_t *cache_get_block(uint64_t block_id)
{
    /* L1 */
    pthread_rwlock_rdlock(&g_cache.l1.lock);
    data_block_t *blk = (data_block_t *)hash_table_get(g_cache.l1.table, block_id);
    pthread_rwlock_unlock(&g_cache.l1.lock);
    if (blk)
    {
        smb_cache_update_hits(1, 1);
        return blk;
    }
    smb_cache_update_hits(1, 0);

    /* L2 */
    if (!g_cache.l2.enabled)
        return NULL;
    pthread_rwlock_rdlock(&g_cache.l2.lock);
    void *slot_ptr = hash_table_get(g_cache.l2.index, block_id);
    data_block_t *hit = NULL;
    if (slot_ptr)
    {
        size_t slot = (size_t)(uintptr_t)slot_ptr;
        if (slot < g_cache.l2.slots && g_cache.l2.slot_ids[slot] == block_id)
            hit = g_cache.l2.slot_blocks[slot];
    }
    pthread_rwlock_unlock(&g_cache.l2.lock);

    if (hit)
    {
        smb_cache_update_hits(2, 1);
        /* promote to L1 */
        cache_put_block(hit);
        return hit;
    }
    smb_cache_update_hits(2, 0);

    /* L3 */
    data_block_t *l3hit = l3_cache_get(block_id);
    if (l3hit)
    {
        cache_put_block(l3hit);
        return l3hit;
    }
    return NULL;
}

void cache_put_block(data_block_t *block)
{
    if (!block)
        return;
    size_t blk_sz = l1_block_size(block);
    pthread_rwlock_wrlock(&g_cache.l1.lock);
    l1_remove_entry(block->block_id);
    l1_evict_until_fit(blk_sz);
    hash_table_set(g_cache.l1.table, block->block_id, block);
    l1_track_insert(block->block_id, blk_sz);
    pthread_rwlock_unlock(&g_cache.l1.lock);

    /* L2 insert */
    if (!g_cache.l2.enabled)
        return;
    pthread_rwlock_wrlock(&g_cache.l2.lock);
    if (g_cache.l2.slots == 0)
    {
        pthread_rwlock_unlock(&g_cache.l2.lock);
        return;
    }
    size_t slot = block->block_id % g_cache.l2.slots;
    uint64_t old_id = g_cache.l2.slot_ids[slot];
    if (old_id)
    {
        hash_table_remove(g_cache.l2.index, old_id);
        if (g_cache.l2.slot_blocks[slot])
        {
            l2_free_block(g_cache.l2.slot_blocks[slot]);
            g_cache.l2.slot_blocks[slot] = NULL;
        }
        l3_remove_entry(old_id);
    }
    g_cache.l2.slot_ids[slot] = block->block_id;
    l2_copy_into_slot(slot, block);
    if (g_cache.l2.dirty_flags)
        g_cache.l2.dirty_flags[slot] = 1;
    hash_table_set(g_cache.l2.index, block->block_id, (void *)(uintptr_t)slot);
    pthread_rwlock_unlock(&g_cache.l2.lock);

    cache_stats_set_dirty();
    cache_stats_set_usage();
    l3_cache_put(block);
}

void cache_invalidate_block(uint64_t block_id)
{
    pthread_rwlock_wrlock(&g_cache.l1.lock);
    hash_table_remove(g_cache.l1.table, block_id);
    l1_remove_entry(block_id);
    pthread_rwlock_unlock(&g_cache.l1.lock);

    if (!g_cache.l2.enabled)
        return;
    pthread_rwlock_wrlock(&g_cache.l2.lock);
    void *slot_ptr = hash_table_get(g_cache.l2.index, block_id);
    if (slot_ptr)
    {
        size_t slot = (size_t)(uintptr_t)slot_ptr;
        hash_table_remove(g_cache.l2.index, block_id);
        g_cache.l2.slot_ids[slot] = 0;
        if (g_cache.l2.slot_blocks[slot])
        {
            l2_free_block(g_cache.l2.slot_blocks[slot]);
            g_cache.l2.slot_blocks[slot] = NULL;
        }
        if (g_cache.l2.dirty_flags)
            g_cache.l2.dirty_flags[slot] = 0;
    }
    pthread_rwlock_unlock(&g_cache.l2.lock);
    l3_remove_entry(block_id);
    cache_stats_set_dirty();
    cache_stats_set_usage();
}

/* Flush dirty L2 slots to backing file (msync). Best-effort; leaves dirty flag set on failure. */
void cache_flush_l2_dirty(void)
{
    if (!g_cache.l2.enabled || !g_cache.l2.map || g_cache.l2.map == MAP_FAILED)
        return;

    pthread_rwlock_wrlock(&g_cache.l2.lock);
    for (size_t i = 0; i < g_cache.l2.slots; i++)
    {
        if (!g_cache.l2.dirty_flags || !g_cache.l2.dirty_flags[i])
            continue;
        if (g_cache.l2.slot_ids[i] == 0)
        {
            g_cache.l2.dirty_flags[i] = 0;
            continue;
        }
        size_t offset = i * g_cache.l2.slot_size;
        if (msync((char *)g_cache.l2.map + offset, g_cache.l2.slot_size, MS_SYNC) == 0)
        {
            g_cache.l2.dirty_flags[i] = 0;
        }
    }
    pthread_rwlock_unlock(&g_cache.l2.lock);
    cache_stats_set_dirty();
    if (g_cache.l2.fd >= 0)
        fsync(g_cache.l2.fd);
}

static void *cache_flush_thread_fn(void *arg)
{
    (void)arg;
    const int base_interval = 30;
    while (g_flush_thread_running)
    {
        cache_flush_l2_dirty();
        cache_stats_set_dirty();
        l3_trim_expired();

        size_t dirty = l2_dirty_count();
        double ratio = g_cache.l2.slots ? (double)dirty / (double)g_cache.l2.slots : 0.0;
        int trigger_now = (ratio >= 0.2);
        if (trigger_now)
            cache_flush_l2_dirty();

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += base_interval;

        pthread_mutex_lock(&g_flush_mutex);
        if (!g_flush_thread_running)
        {
            pthread_mutex_unlock(&g_flush_mutex);
            break;
        }
        if (g_flush_wakeup)
        {
            g_flush_wakeup = 0;
            pthread_mutex_unlock(&g_flush_mutex);
            continue;
        }
        pthread_cond_timedwait(&g_flush_cond, &g_flush_mutex, &ts);
        pthread_mutex_unlock(&g_flush_mutex);
    }
    return NULL;
}

void cache_flush_request(void)
{
    pthread_mutex_lock(&g_flush_mutex);
    g_flush_wakeup = 1;
    pthread_cond_signal(&g_flush_cond);
    pthread_mutex_unlock(&g_flush_mutex);
}

void multi_level_cache_manage(void)
{
    cache_flush_l2_dirty();
    cache_stats_set_usage();
    l3_trim_expired();
}

void cache_invalidate_block_level(uint64_t block_id, int level_mask)
{
    /* level_mask: bit0=L1, bit1=L2, bit2=L3 */
    if (level_mask & 0x1)
    {
        pthread_rwlock_wrlock(&g_cache.l1.lock);
        hash_table_remove(g_cache.l1.table, block_id);
        l1_remove_entry(block_id);
        pthread_rwlock_unlock(&g_cache.l1.lock);
    }
    if (level_mask & 0x2)
        cache_invalidate_block(block_id);
    if (level_mask & 0x4)
        l3_remove_entry(block_id);
    cache_stats_set_dirty();
    cache_stats_set_usage();
}

void cache_prefetch(uint64_t *block_ids, size_t count)
{
    if (!block_ids || count == 0)
        return;
    for (size_t i = 0; i < count; i++)
    {
        cache_get_block(block_ids[i]);
    }
}
