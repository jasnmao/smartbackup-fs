#ifndef DEDUP_H
#define DEDUP_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "smartbackupfs.h"

/* 前向声明，避免循环依赖 */
typedef struct version_node version_node_t;

typedef enum {
    COMPRESSION_NONE = 0,
    COMPRESSION_LZ4,
    COMPRESSION_ZSTD,
    COMPRESSION_GZIP
} compression_algorithm_t;

typedef struct {
    bool enable_deduplication;
    bool enable_compression;
    compression_algorithm_t algo;
    int compression_level;
    size_t min_compress_size;
} dedup_config_t;

typedef struct {
    hash_table_t *block_hash_index;
    pthread_rwlock_t global_lock;
    size_t total_unique_blocks;
    size_t saved_space;
} global_dedup_state_t;

/* 全局配置实例（由元数据管理模块持有） */
extern dedup_config_t dedup_config;

/* 可扩展压缩回调（模块D预留） */
typedef int (*compress_func_t)(const char *in, size_t in_size, char *out, size_t *out_size, int level);
typedef int (*decompress_func_t)(const char *in, size_t in_size, char *out, size_t *out_size);

int dedup_init(const dedup_config_t *config);
void dedup_shutdown(void);

void block_compute_hash(data_block_t *block);
data_block_t *dedup_find_duplicate(const uint8_t hash[32]);
int dedup_index_block(data_block_t *block);
int dedup_remove_block(data_block_t *block);
void dedup_release_block(data_block_t *block);

int block_compress(data_block_t *block, dedup_config_t *config);
int block_decompress(data_block_t *block, char **out_data, size_t *out_size);
void dedup_set_compression(dedup_config_t *config, compression_algorithm_t algo, int level);

int dedup_process_block_on_write(data_block_t **slot, dedup_config_t *config);
int dedup_process_diff_blocks(hash_table_t *diff_blocks, dedup_config_t *config);
ssize_t dedup_read_version_data(version_node_t *version, char *buf, size_t size, off_t offset);

void dedup_get_stats(global_dedup_state_t *out);
int dedup_update_config(bool enable_dedup, bool enable_comp, compression_algorithm_t algo, int level, size_t min_size);
int dedup_format_stats(char *buf, size_t buf_size);

/* 模块D预留扩展接口 */
data_block_t *dedup_remote_find_duplicate(const uint8_t hash[32]);
size_t block_metadata_serialize(data_block_t *block, char *buf, size_t buf_size);
int block_metadata_deserialize(const char *buf, size_t buf_size, data_block_t *block);
int dedup_register_compressor(compression_algorithm_t algo, compress_func_t compress, decompress_func_t decompress);

#endif /* DEDUP_H */
