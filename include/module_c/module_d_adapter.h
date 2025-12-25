#ifndef MODULE_C_MODULE_D_ADAPTER_H
#define MODULE_C_MODULE_D_ADAPTER_H

#include <stdint.h>
#include "smartbackupfs.h"
#include "module_c/storage_monitor_basic.h"

typedef struct
{
    basic_storage_stats_t basic;
    cache_stats_t cache;
    storage_prediction_stats_t prediction;
} storage_stats_t;

uint64_t md_get_block_hash(data_block_t *block);
data_block_t *md_find_block_by_hash(const uint8_t hash[32]);
int md_cache_force_writeback(void);
int md_cache_prefetch_block(uint64_t block_id);
storage_stats_t md_get_current_storage_stats(void);

/* 数据完整性保护接口 */
uint32_t calculate_block_checksum(data_block_t *block);
int verify_block_integrity(data_block_t *block);
void start_integrity_scan(void);
void stop_integrity_scan(void);
int handle_corrupted_block(data_block_t *block);

#endif /* MODULE_C_MODULE_D_ADAPTER_H */
