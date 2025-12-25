#include "module_c/dedup_core.h"

#include <openssl/sha.h>
#include <string.h>

/* 由模块A提供的哈希表接口声明，避免头文件循环 */
hash_table_t *hash_table_create(size_t size);
void hash_table_destroy(hash_table_t *table);
int hash_table_set(hash_table_t *table, uint64_t key, void *value);
void *hash_table_get(hash_table_t *table, uint64_t key);
int hash_table_remove(hash_table_t *table, uint64_t key);

static uint64_t hash_key_from_hash(const uint8_t hash[32])
{
    uint64_t k = 0;
    memcpy(&k, hash, sizeof(uint64_t));
    return k;
}

void dedup_core_calculate_hash(data_block_t *block, uint8_t out_hash[32])
{
    if (!block)
        return;
    SHA256((const unsigned char *)block->data, block->size, out_hash);
    if (block->hash != out_hash)
        memcpy(block->hash, out_hash, 32);
}

data_block_t *dedup_core_find(global_dedup_state_t *state, const uint8_t hash[32])
{
    if (!state || !state->block_hash_index || !hash)
        return NULL;
    uint64_t key = hash_key_from_hash(hash);
    return (data_block_t *)hash_table_get(state->block_hash_index, key);
}

int dedup_core_index(global_dedup_state_t *state, data_block_t *block)
{
    if (!state || !state->block_hash_index || !block)
        return -1;
    uint64_t key = hash_key_from_hash(block->hash);
    return hash_table_set(state->block_hash_index, key, block);
}

int dedup_core_remove(global_dedup_state_t *state, const uint8_t hash[32])
{
    if (!state || !state->block_hash_index || !hash)
        return -1;
    uint64_t key = hash_key_from_hash(hash);
    return hash_table_remove(state->block_hash_index, key);
}

void dedup_core_inc_ref(data_block_t *block)
{
    if (!block)
        return;
    pthread_mutex_lock(&block->ref_lock);
    block->ref_count++;
    pthread_mutex_unlock(&block->ref_lock);
}

void dedup_core_dec_ref(data_block_t *block)
{
    if (!block)
        return;
    pthread_mutex_lock(&block->ref_lock);
    if (block->ref_count > 0)
        block->ref_count--;
    pthread_mutex_unlock(&block->ref_lock);
}
