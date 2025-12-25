// 模块C：去重与压缩实现

#include "dedup.h"
#include "version_manager.h"
#include "module_c/dedup_core.h"
#include "module_c/adaptive_compress.h"
#include "module_c/storage_monitor_basic.h"
#include "module_c/cache.h"
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if HAVE_ZLIB
#include <zlib.h>
#endif
#include <lz4.h>
#include <zstd.h>
#include <errno.h>

#ifndef HAVE_ZLIB
#define HAVE_ZLIB 0
#endif

static global_dedup_state_t g_dedup;
static dedup_config_t g_config;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static const char *DEDUP_CFG_PATH = "/tmp/smartbackupfs_dedup.conf";

/* hash_table_* 由模块A实现，这里声明以避免头文件循环 */
hash_table_t *hash_table_create(size_t size);
void hash_table_destroy(hash_table_t *table);
int hash_table_set(hash_table_t *table, uint64_t key, void *value);
void *hash_table_get(hash_table_t *table, uint64_t key);
int hash_table_remove(hash_table_t *table, uint64_t key);

typedef struct
{
    compress_func_t compress;
    decompress_func_t decompress;
} compressor_entry_t;

static compressor_entry_t g_compressors[COMPRESSION_GZIP + 1];

static void dedup_validate_config(dedup_config_t *cfg)
{
    if (!cfg)
        return;
    if (cfg->compression_level < 1)
        cfg->compression_level = 1;
    if (cfg->compression_level > 9)
        cfg->compression_level = 9;
    if (cfg->min_compress_size < 512)
        cfg->min_compress_size = 512;
    if (cfg->algo < COMPRESSION_NONE || cfg->algo > COMPRESSION_GZIP)
        cfg->algo = COMPRESSION_LZ4;
    cfg->enable_compression = cfg->enable_compression && (cfg->algo != COMPRESSION_NONE);
}

static void dedup_apply_config(const dedup_config_t *cfg)
{
    if (!cfg)
        return;
    g_config = *cfg;
    dedup_config = *cfg;
}

static void dedup_persist_config(void)
{
    FILE *fp = fopen(DEDUP_CFG_PATH, "w");
    if (!fp)
        return;
    fprintf(fp, "dedup=%d\ncomp=%d\nalgo=%d\nlevel=%d\nmin=%zu\n",
            g_config.enable_deduplication ? 1 : 0,
            g_config.enable_compression ? 1 : 0,
            g_config.algo,
            g_config.compression_level,
            g_config.min_compress_size);
    fclose(fp);
}

static void dedup_try_load_config(void)
{
    FILE *fp = fopen(DEDUP_CFG_PATH, "r");
    if (!fp)
        return;
    dedup_config_t cfg = g_config;
    int dedup_on = 0, comp_on = 0, algo = 0, level = 0;
    size_t minsz = 0;
    if (fscanf(fp, "dedup=%d\ncomp=%d\nalgo=%d\nlevel=%d\nmin=%zu",
               &dedup_on, &comp_on, &algo, &level, &minsz) == 5)
    {
        cfg.enable_deduplication = (dedup_on != 0);
        cfg.enable_compression = (comp_on != 0);
        cfg.algo = (compression_algorithm_t)algo;
        cfg.compression_level = level;
        cfg.min_compress_size = minsz;
        dedup_validate_config(&cfg);
        dedup_apply_config(&cfg);
    }
    fclose(fp);
}

static int copy_on_write(data_block_t **slot)
{
    if (!slot || !*slot)
        return -EINVAL;
    data_block_t *blk = *slot;
    pthread_mutex_lock(&blk->ref_lock);
    uint32_t refs = blk->ref_count;
    pthread_mutex_unlock(&blk->ref_lock);
    if (refs <= 1)
        return 0;

    data_block_t *newb = allocate_block(blk->size);
    if (!newb)
        return -ENOMEM;

    size_t plain_size = blk->size;
    const char *plain = blk->data;
    char *owned = NULL;
    if (blk->compressed_size > 0 && blk->compression != COMPRESSION_NONE)
    {
        if (block_decompress(blk, &owned, &plain_size) == 0 && owned)
            plain = owned;
    }
    memcpy(newb->data, plain, plain_size);
    newb->size = plain_size;
    newb->compressed_size = 0;
    newb->compression = COMPRESSION_NONE;
    newb->file_type = blk->file_type;
    block_compute_hash(newb);

    dedup_release_block(blk);
    *slot = newb;
    if (owned)
        free(owned);
    return 0;
}

static void dedup_cow_version_blocks(version_node_t *version)
{
    if (!version || !version->block_map)
        return;
    block_map_t *map = version->block_map;
    pthread_rwlock_wrlock(&map->lock);
    for (uint64_t i = 0; i < map->block_count; i++)
    {
        if (map->blocks && map->blocks[i])
            copy_on_write(&map->blocks[i]);
    }
    pthread_rwlock_unlock(&map->lock);
}

static const char *algo_name(compression_algorithm_t algo)
{
    switch (algo)
    {
    case COMPRESSION_LZ4:
        return "lz4";
    case COMPRESSION_ZSTD:
        return "zstd";
    case COMPRESSION_GZIP:
        return "gzip";
    case COMPRESSION_NONE:
    default:
        return "none";
    }
}

static int compress_copy(const char *in, size_t in_size, char *out, size_t *out_size, int level)
{
    (void)level;
    if (!in || !out || !out_size || *out_size < in_size)
        return -1;
    memcpy(out, in, in_size);
    *out_size = in_size;
    return 0;
}

static int decompress_copy(const char *in, size_t in_size, char *out, size_t *out_size)
{
    if (!in || !out || !out_size || *out_size < in_size)
        return -1;
    memcpy(out, in, in_size);
    *out_size = in_size;
    return 0;
}

static int compress_lz4(const char *in, size_t in_size, char *out, size_t *out_size, int level)
{
    (void)level;
    int bound = LZ4_compressBound((int)in_size);
    if (bound <= 0 || !out_size || (int)*out_size < bound)
        return -1;
    int written = LZ4_compress_default(in, out, (int)in_size, (int)*out_size);
    if (written <= 0)
        return -1;
    *out_size = (size_t)written;
    return 0;
}

static int decompress_lz4(const char *in, size_t in_size, char *out, size_t *out_size)
{
    if (!out_size)
        return -1;
    int res = LZ4_decompress_safe(in, out, (int)in_size, (int)*out_size);
    if (res < 0)
        return -1;
    *out_size = (size_t)res;
    return 0;
}

static int compress_zstd(const char *in, size_t in_size, char *out, size_t *out_size, int level)
{
    size_t written = ZSTD_compress(out, *out_size, in, in_size, level);
    if (ZSTD_isError(written))
        return -1;
    *out_size = written;
    return 0;
}

static int decompress_zstd(const char *in, size_t in_size, char *out, size_t *out_size)
{
    size_t res = ZSTD_decompress(out, *out_size, in, in_size);
    if (ZSTD_isError(res))
        return -1;
    *out_size = res;
    return 0;
}

static int compress_gzip(const char *in, size_t in_size, char *out, size_t *out_size, int level)
{
#if HAVE_ZLIB
    uLongf dest_len = (uLongf)*out_size;
    int ret = compress2((Bytef *)out, &dest_len, (const Bytef *)in, (uLongf)in_size, level);
    if (ret != Z_OK)
        return -1;
    *out_size = (size_t)dest_len;
    return 0;
#else
    (void)in;
    (void)in_size;
    (void)out;
    (void)out_size;
    (void)level;
    return -1;
#endif
}

static int decompress_gzip(const char *in, size_t in_size, char *out, size_t *out_size)
{
#if HAVE_ZLIB
    uLongf dest_len = (uLongf)*out_size;
    int ret = uncompress((Bytef *)out, &dest_len, (const Bytef *)in, (uLongf)in_size);
    if (ret != Z_OK)
        return -1;
    *out_size = (size_t)dest_len;
    return 0;
#else
    (void)in;
    (void)in_size;
    (void)out;
    (void)out_size;
    return -1;
#endif
}

static void register_default_compressors(void)
{
    g_compressors[COMPRESSION_NONE].compress = compress_copy;
    g_compressors[COMPRESSION_NONE].decompress = decompress_copy;

    g_compressors[COMPRESSION_LZ4].compress = compress_lz4;
    g_compressors[COMPRESSION_LZ4].decompress = decompress_lz4;

    g_compressors[COMPRESSION_ZSTD].compress = compress_zstd;
    g_compressors[COMPRESSION_ZSTD].decompress = decompress_zstd;

    g_compressors[COMPRESSION_GZIP].compress = compress_gzip;
    g_compressors[COMPRESSION_GZIP].decompress = decompress_gzip;
}

int dedup_init(const dedup_config_t *config)
{
    memset(&g_dedup, 0, sizeof(g_dedup));
    pthread_rwlock_init(&g_dedup.global_lock, NULL);
    g_dedup.block_hash_index = hash_table_create(16384);
    if (!g_dedup.block_hash_index)
        return -1;
    register_default_compressors();
    if (config)
        g_config = *config;
    else
    {
        g_config.enable_deduplication = false;
        g_config.enable_compression = false;
        g_config.algo = COMPRESSION_NONE;
        g_config.compression_level = 1;
        g_config.min_compress_size = 1024;
    }
    dedup_validate_config(&g_config);
    dedup_apply_config(&g_config);
    dedup_try_load_config();
    return 0;
}

void dedup_shutdown(void)
{
    if (g_dedup.block_hash_index)
    {
        hash_table_destroy(g_dedup.block_hash_index);
        g_dedup.block_hash_index = NULL;
    }
    pthread_rwlock_destroy(&g_dedup.global_lock);
}

void block_compute_hash(data_block_t *block)
{
    if (!block || !block->data || block->size == 0)
        return;
    dedup_core_calculate_hash(block, block->hash);
}

data_block_t *dedup_find_duplicate(const uint8_t hash[32])
{
    if (!g_config.enable_deduplication || !g_dedup.block_hash_index)
        return NULL;
    pthread_rwlock_rdlock(&g_dedup.global_lock);
    data_block_t *cand = dedup_core_find(&g_dedup, hash);
    if (cand && memcmp(cand->hash, hash, 32) == 0)
    {
        dedup_core_inc_ref(cand);
    }
    else
    {
        cand = NULL;
    }
    pthread_rwlock_unlock(&g_dedup.global_lock);
    return cand;
}

int dedup_index_block(data_block_t *block)
{
    if (!g_config.enable_deduplication || !block)
        return 0;
    pthread_rwlock_wrlock(&g_dedup.global_lock);
    if (!dedup_core_find(&g_dedup, block->hash))
    {
        dedup_core_index(&g_dedup, block);
        g_dedup.total_unique_blocks++;
        smb_update_unique_block();
    }
    pthread_rwlock_unlock(&g_dedup.global_lock);
    return 0;
}

int dedup_remove_block(data_block_t *block)
{
    if (!block || !g_dedup.block_hash_index)
        return 0;
    pthread_rwlock_wrlock(&g_dedup.global_lock);
    data_block_t *cand = dedup_core_find(&g_dedup, block->hash);
    if (cand == block)
    {
        dedup_core_remove(&g_dedup, block->hash);
        if (g_dedup.total_unique_blocks > 0)
            g_dedup.total_unique_blocks--;
        smb_on_unique_block_removed();
    }
    pthread_rwlock_unlock(&g_dedup.global_lock);
    return 0;
}

void dedup_release_block(data_block_t *block)
{
    if (!block)
        return;
    int free_now = 0;
    pthread_mutex_lock(&block->ref_lock);
    if (block->ref_count > 0)
        block->ref_count--;
    free_now = (block->ref_count == 0);
    pthread_mutex_unlock(&block->ref_lock);

    if (free_now)
        free_block(block);
}

static size_t compression_bound(compression_algorithm_t algo, size_t in_size)
{
    switch (algo)
    {
    case COMPRESSION_LZ4:
        return (size_t)LZ4_compressBound((int)in_size);
    case COMPRESSION_ZSTD:
        return ZSTD_compressBound(in_size);
    case COMPRESSION_GZIP:
    {
#if HAVE_ZLIB
        return (size_t)compressBound((uLong)in_size);
#else
        return in_size;
#endif
    }
    case COMPRESSION_NONE:
    default:
        return in_size;
    }
}

int block_compress(data_block_t *block, dedup_config_t *config)
{
    if (!block)
        return -1;

    /* 已压缩的块（来自去重复用）不再二次压缩，避免重压缩短缓冲导致数据损坏 */
    if (block->compressed_size > 0 && block->compression != COMPRESSION_NONE)
        return 0;

    dedup_config_t *cfg = config ? config : &g_config;
    compression_algorithm_t algo = cfg->algo;
    if (!cfg->enable_compression || algo == COMPRESSION_NONE || block->size < cfg->min_compress_size)
    {
        block->compressed_size = 0;
        block->compression = COMPRESSION_NONE;
        return 0;
    }

    if (algo == COMPRESSION_GZIP && !HAVE_ZLIB)
    {
        block->compressed_size = 0;
        block->compression = COMPRESSION_NONE;
        return 0;
    }

    size_t bound = compression_bound(algo, block->size);
    char *out = malloc(bound);
    if (!out)
        return -1;

    size_t out_size = bound;
    compress_func_t fn = g_compressors[algo].compress;
    if (!fn || fn(block->data, block->size, out, &out_size, cfg->compression_level) != 0)
    {
        free(out);
        return -1;
    }

    if (out_size >= block->size)
    {
        free(out);
        block->compressed_size = 0;
        block->compression = COMPRESSION_NONE;
        return 0;
    }

    char *old = block->data;
    block->data = out;
    block->compressed_size = out_size;
    block->compression = (uint8_t)algo;

    pthread_rwlock_wrlock(&g_dedup.global_lock);
    g_dedup.saved_space += (block->size - block->compressed_size);
    pthread_rwlock_unlock(&g_dedup.global_lock);

    free(old);
    return 0;
}

int block_decompress(data_block_t *block, char **out_data, size_t *out_size)
{
    if (!block || !out_data || !out_size)
        return -1;

    size_t expected = block->size;
    *out_data = malloc(expected);
    if (!*out_data)
        return -1;

    size_t buf_size = expected;
    if (block->compressed_size == 0 || block->compression == COMPRESSION_NONE)
    {
        memcpy(*out_data, block->data, expected);
        *out_size = expected;
        return 0;
    }

    decompress_func_t fn = g_compressors[block->compression].decompress;
    if (!fn || fn(block->data, block->compressed_size, *out_data, &buf_size) != 0)
    {
        free(*out_data);
        *out_data = NULL;
        return -1;
    }

    *out_size = buf_size;
    return 0;
}

void dedup_set_compression(dedup_config_t *config, compression_algorithm_t algo, int level)
{
    if (!config)
        return;
    config->algo = algo;
    config->compression_level = level;
    config->enable_compression = (algo != COMPRESSION_NONE);
}

int dedup_process_block_on_write(data_block_t **slot, dedup_config_t *config)
{
    if (!slot || !*slot)
        return -1;

    copy_on_write(slot);

    dedup_config_t *cfg = config ? config : &g_config;
    data_block_t *blk = *slot;

    block_compute_hash(blk);

    if (cfg->enable_deduplication)
    {
        data_block_t *dup = dedup_find_duplicate(blk->hash);
        if (dup && dup != blk)
        {
            dedup_release_block(blk);
            *slot = dup;
            pthread_rwlock_wrlock(&g_dedup.global_lock);
            g_dedup.saved_space += dup->size;
            pthread_rwlock_unlock(&g_dedup.global_lock);
            smb_update_dedup_on_hit(dup->size);
            blk = dup;
        }
        else if (!dup)
        {
            dedup_index_block(blk);
        }
    }

    if (cfg->enable_compression)
    {
        size_t before = blk->size;
        ac_adaptive_compress_block(blk, cfg);
        if (blk->compressed_size > 0 && blk->compressed_size < before)
            smb_update_compress(before, blk->compressed_size);
    }
    else
    {
        blk->compression = COMPRESSION_NONE;
        blk->compressed_size = 0;
    }

    return 0;
}

int dedup_process_diff_blocks(hash_table_t *diff_blocks, dedup_config_t *config)
{
    if (!diff_blocks)
        return -1;

    pthread_rwlock_wrlock(&diff_blocks->lock);
    for (size_t i = 0; i < diff_blocks->size; i++)
    {
        hash_node_t *node = diff_blocks->buckets[i];
        while (node)
        {
            data_block_t *b = (data_block_t *)node->value;
            if (b && b != (void *)1)
            {
                dedup_process_block_on_write(&b, config ? config : &g_config);
                node->value = b;
            }
            node = node->next;
        }
    }
    pthread_rwlock_unlock(&diff_blocks->lock);
    return 0;
}

ssize_t dedup_read_version_data(version_node_t *version, char *buf, size_t size, off_t offset)
{
    if (!version || !buf)
        return -1;

    dedup_cow_version_blocks(version);

    file_metadata_t vmeta = {0};
    vmeta.version_handle = version;
    vmeta.size = version->file_size;
    vmeta.blocks = version->blocks;
    return version_manager_read_version_data(&vmeta, buf, size, offset);
}

void dedup_get_stats(global_dedup_state_t *out)
{
    if (!out)
        return;
    pthread_rwlock_rdlock(&g_dedup.global_lock);
    *out = g_dedup;
    pthread_rwlock_unlock(&g_dedup.global_lock);
}

int dedup_update_config(bool enable_dedup, bool enable_comp, compression_algorithm_t algo, int level, size_t min_size)
{
    pthread_mutex_lock(&g_cfg_lock);
    g_config.enable_deduplication = enable_dedup;
    g_config.enable_compression = enable_comp;
    g_config.algo = algo;
    g_config.compression_level = level;
    g_config.min_compress_size = min_size;
    dedup_validate_config(&g_config);
    dedup_apply_config(&g_config);
    pthread_mutex_unlock(&g_cfg_lock);
    dedup_persist_config();
    return 0;
}

int dedup_format_stats(char *buf, size_t buf_size)
{
    if (!buf || buf_size == 0)
        return -1;
    global_dedup_state_t snap;
    dedup_get_stats(&snap);
    int n = snprintf(buf, buf_size, "unique=%zu;saved=%zu;algo=%s;dedup=%s;comp=%s",
                     snap.total_unique_blocks, snap.saved_space, algo_name(g_config.algo),
                     g_config.enable_deduplication ? "on" : "off",
                     g_config.enable_compression ? "on" : "off");
    return (n >= 0 && (size_t)n < buf_size) ? n : -1;
}

data_block_t *dedup_remote_find_duplicate(const uint8_t hash[32])
{
    /* 预留：当前直接复用本地索引 */
    return dedup_find_duplicate(hash);
}

size_t block_metadata_serialize(data_block_t *block, char *buf, size_t buf_size)
{
    if (!block || !buf)
        return 0;

    size_t needed = sizeof(uint64_t) * 3 + sizeof(uint8_t) + 32;
    if (buf_size < needed)
        return 0;

    size_t offset = 0;
    memcpy(buf + offset, &block->block_id, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    memcpy(buf + offset, &block->size, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint64_t csz = (uint64_t)block->compressed_size;
    memcpy(buf + offset, &csz, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    memcpy(buf + offset, &block->compression, sizeof(uint8_t));
    offset += sizeof(uint8_t);
    memcpy(buf + offset, block->hash, 32);
    offset += 32;
    uint64_t refc = block->ref_count;
    memcpy(buf + offset, &refc, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    return offset;
}

int block_metadata_deserialize(const char *buf, size_t buf_size, data_block_t *block)
{
    if (!buf || !block)
        return -1;

    size_t needed = sizeof(uint64_t) * 3 + sizeof(uint8_t) + 32;
    if (buf_size < needed)
        return -1;

    size_t offset = 0;
    memcpy(&block->block_id, buf + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    memcpy(&block->size, buf + offset, sizeof(uint64_t));
    offset += sizeof(uint64_t);
    uint64_t csz = 0;
    memcpy(&csz, buf + offset, sizeof(uint64_t));
    block->compressed_size = (size_t)csz;
    offset += sizeof(uint64_t);
    memcpy(&block->compression, buf + offset, sizeof(uint8_t));
    offset += sizeof(uint8_t);
    memcpy(block->hash, buf + offset, 32);
    offset += 32;
    uint64_t refc = 0;
    memcpy(&refc, buf + offset, sizeof(uint64_t));
    block->ref_count = (uint32_t)refc;
    return 0;
}

int dedup_register_compressor(compression_algorithm_t algo, compress_func_t compress, decompress_func_t decompress)
{
    if (algo < COMPRESSION_NONE || algo > COMPRESSION_GZIP)
        return -EINVAL;
    g_compressors[algo].compress = compress;
    g_compressors[algo].decompress = decompress;
    return 0;
}
