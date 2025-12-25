#ifndef MODULE_C_CACHE_H
#define MODULE_C_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include "smartbackupfs.h"

/* L1: in-memory hash (block_id -> data_block_t*) */
typedef struct l1_cache
{
    hash_table_t *table;
    size_t max_bytes;
    size_t current_bytes;
    struct l1_entry *head;
    struct l1_entry *tail;
    pthread_rwlock_t lock;
} l1_cache_t;

/* L2/L3 placeholders for future mmap/SSD tiers */
typedef struct l2_cache
{
    size_t capacity_bytes;
    size_t slot_size;
    size_t slots;
    int fd;
    void *map;
    uint64_t *slot_ids;
    data_block_t **slot_blocks;
    uint8_t *dirty_flags; /* per-slot dirty marker for msync scheduling */
    hash_table_t *index; /* block_id -> slot (stored as uintptr_t) */
    pthread_rwlock_t lock;
    int enabled;
    char backing_path[256];
} l2_cache_t;

typedef struct l3_cache
{
    size_t capacity_bytes;
    size_t slot_size;
    size_t max_entries;
    size_t expire_seconds;
    size_t current_bytes;
    char cache_dir[256];
    hash_table_t *index; /* block_id -> l3_entry_t* */
    pthread_rwlock_t lock;
} l3_cache_t;

typedef struct
{
    l1_cache_t l1;
    l2_cache_t l2;
    l3_cache_t l3;
} multi_level_cache_t;

int cache_system_init(size_t l1_bytes, size_t l2_bytes, size_t l3_bytes);
void cache_system_shutdown(void);

data_block_t *cache_get_block(uint64_t block_id);
void cache_put_block(data_block_t *block);
void cache_invalidate_block(uint64_t block_id);
void cache_flush_l2_dirty(void);
void cache_invalidate_block_level(uint64_t block_id, int level_mask);
void cache_prefetch(uint64_t *block_ids, size_t count);
void cache_flush_request(void);
void multi_level_cache_manage(void);

#endif /* MODULE_C_CACHE_H */
