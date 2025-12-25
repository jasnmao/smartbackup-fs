/**
 * 元数据管理模块头文件
 */

#ifndef METADATA_H
#define METADATA_H

#include <stdint.h>
#include <time.h>
#include <sys/types.h>
#include <pthread.h>

// 前向声明
typedef struct file_metadata file_metadata_t;
typedef struct data_block data_block_t;
typedef struct block_map block_map_t;

// 完整的结构定义需要包含在smartbackupfs.h中，这里只做前向声明

// 哈希表节点
typedef struct hash_node {
    uint64_t key;
    void *value;
    struct hash_node *next;
    time_t access_time;
} hash_node_t;

// 哈希表结构
typedef struct hash_table {
    hash_node_t **buckets;
    size_t size;
    size_t count;
    pthread_rwlock_t lock;
} hash_table_t;

// 哈希表操作函数
hash_table_t *hash_table_create(size_t size);
void hash_table_destroy(hash_table_t *table);
int hash_table_set(hash_table_t *table, uint64_t key, void *value);
void *hash_table_get(hash_table_t *table, uint64_t key);
int hash_table_remove(hash_table_t *table, uint64_t key);
void hash_table_clear(hash_table_t *table);
size_t hash_table_size(hash_table_t *table);

// LRU缓存结构
typedef struct {
    hash_table_t *table;
    size_t max_size;
    size_t current_size;
    pthread_mutex_t mutex;
} lru_cache_t;

// LRU缓存操作
lru_cache_t *lru_cache_create(size_t max_size);
void lru_cache_destroy(lru_cache_t *cache);
int lru_cache_put(lru_cache_t *cache, uint64_t key, void *value);
void *lru_cache_get(lru_cache_t *cache, uint64_t key);
int lru_cache_remove(lru_cache_t *cache, uint64_t key);
void lru_cache_clear(lru_cache_t *cache);

// 块映射管理函数
block_map_t *create_block_map(uint64_t file_ino);
void destroy_block_map(block_map_t *map);
block_map_t *get_block_map(uint64_t file_ino);
int block_map_diff(block_map_t *old_map, block_map_t *new_map, hash_table_t *diff_blocks);

// 高性能文件操作
int smart_read_file(file_metadata_t *meta, char *buf, size_t size, off_t offset);
int smart_write_file(file_metadata_t *meta, const char *buf, size_t size, off_t offset);

#endif // METADATA_H