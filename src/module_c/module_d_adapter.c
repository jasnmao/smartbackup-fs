#include "module_c/module_d_adapter.h"

#include <string.h>
#include <stdio.h>
#include "module_c/cache.h"
#include "module_c/dedup_core.h"
#include "module_c/storage_monitor_basic.h"
#include "dedup.h"

uint64_t md_get_block_hash(data_block_t *block)
{
    if (!block)
        return 0;
    dedup_core_calculate_hash(block, block->hash);
    uint64_t h = 0;
    memcpy(&h, block->hash, sizeof(uint64_t));
    return h;
}

data_block_t *md_find_block_by_hash(const uint8_t hash[32])
{
    if (!hash)
        return NULL;
    return dedup_find_duplicate(hash);
}

int md_cache_force_writeback(void)
{
    cache_flush_l2_dirty();
    cache_flush_request();
    return 0;
}

int md_cache_prefetch_block(uint64_t block_id)
{
    cache_prefetch(&block_id, 1);
    return 0;
}

storage_stats_t md_get_current_storage_stats(void)
{
    storage_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    smb_get_stats(&stats.basic);
    smb_cache_get_stats(&stats.cache);
    smb_get_prediction(&stats.prediction);
    return stats;
}

uint32_t calculate_block_checksum(data_block_t *block)
{
    if (!block || !block->data)
        return 0;
    uint8_t hash[32] = {0};
    dedup_core_calculate_hash(block, hash);
    uint32_t csum = 0;
    memcpy(&csum, hash, sizeof(uint32_t));
    return csum;
}

int verify_block_integrity(data_block_t *block)
{
    if (!block)
        return -1;
    uint32_t current = calculate_block_checksum(block);
    uint32_t stored = 0;
    memcpy(&stored, block->hash, sizeof(uint32_t));
    return (current == stored) ? 0 : -1;
}

void start_integrity_scan(void)
{
    /* 预留：未来可调度异步扫描 */
}

void stop_integrity_scan(void)
{
    /* 预留：未来可取消扫描任务 */
}

int handle_corrupted_block(data_block_t *block)
{
    (void)block;
    fprintf(stderr, "corrupted block detected\n");
    return -1;
}
