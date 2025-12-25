#ifndef MODULE_C_DEDUP_CORE_H
#define MODULE_C_DEDUP_CORE_H

#include <stddef.h>
#include <stdint.h>
#include "smartbackupfs.h"
#include "dedup.h"

/* 基础哈希计算与索引管理 */
void dedup_core_calculate_hash(data_block_t *block, uint8_t out_hash[32]);
data_block_t *dedup_core_find(global_dedup_state_t *state, const uint8_t hash[32]);
int dedup_core_index(global_dedup_state_t *state, data_block_t *block);
int dedup_core_remove(global_dedup_state_t *state, const uint8_t hash[32]);

/* 引用计数管理 */
void dedup_core_inc_ref(data_block_t *block);
void dedup_core_dec_ref(data_block_t *block);

#endif /* MODULE_C_DEDUP_CORE_H */
