/**
 * Module B: Version manager (透明版本管理)
 *
 * 提供基础的版本创建、列表、访问与清理功能。
 * 设计原则：复用 module A 的数据结构与缓存，轻量实现以便快速集成。
 */

#include "version_manager.h"
#include "smartbackupfs.h"
#include "dedup.h"
#include "module_c/cache.h"
#include "module_c/storage_prediction.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

/* 函数原型（由 module_a 提供的实现） */
hash_table_t *hash_table_create(size_t size);
void hash_table_destroy(hash_table_t *table);
void *hash_table_get(hash_table_t *table, uint64_t key);
int hash_table_set(hash_table_t *table, uint64_t key, void *value);

lru_cache_t *lru_cache_create(size_t max_size);
void lru_cache_destroy(lru_cache_t *cache);
int lru_cache_put(lru_cache_t *cache, uint64_t key, void *value);
void *lru_cache_get(lru_cache_t *cache, uint64_t key);
int lru_cache_remove(lru_cache_t *cache, uint64_t key);

block_map_t *get_block_map(uint64_t file_ino);
file_metadata_t *lookup_inode(uint64_t ino);

/* 全局版本链表按文件ino组织 */
static hash_table_t *versions_by_file = NULL; /* key: file_ino -> value: version_chain_t* */
static pthread_mutex_t versions_mutex = PTHREAD_MUTEX_INITIALIZER;
static int cleaner_running = 0;


/* 向前声明：用于父子版本间的数据继承解析 */
static int snapshot_get_block_data(const version_node_t *vn, uint64_t block_index, const char **data, size_t *size);

/* 从链表中移除并释放一个版本节点（调用方持有 chain->lock） */
static version_node_t *version_remove_node_locked(version_chain_t *chain, version_node_t *del, file_metadata_t *meta, uint64_t *added_bytes)
{
    if (!chain || !del)
        return NULL;

    version_node_t *prev = del->prev;
    version_node_t *next = del->next;

    if (prev)
        prev->next = next;
    else
        chain->head = next;

    if (next)
        next->prev = prev;
    else
        chain->tail = prev;

    /* 修正仍在链上的子版本：补齐继承的数据并重定向父指针 */
    uint64_t newly_materialized = 0;
    for (version_node_t *iter = chain->head; iter; iter = iter->next)
    {
        if (iter->parent == del)
        {
            for (size_t i = 0; i < iter->snapshot_count; i++)
            {
                if (iter->snapshots[i].has_data)
                    continue;
                const char *data = NULL;
                size_t sz = 0;
                if (snapshot_get_block_data(del, i, &data, &sz) == 0 && data && sz > 0)
                {
                    char *copy = malloc(sz);
                    if (copy)
                    {
                        memcpy(copy, data, sz);
                        iter->snapshots[i].data = copy;
                        iter->snapshots[i].size = sz;
                        iter->snapshots[i].has_data = true;
                        iter->stored_bytes += sz;
                        newly_materialized += sz;
                    }
                }
            }
            iter->parent = del->parent;
            iter->parent_id = del->parent ? del->parent->version_id : 0;
        }
    }

    for (size_t i = 0; i < del->snapshot_count; i++)
    {
        if (del->snapshots[i].has_data)
            free(del->snapshots[i].data);
    }
    free(del->snapshots);
    free(del->diff_blocks);
    free(del->block_checksums);
    if (del->description)
        free(del->description);

    uint64_t key = (chain->file_ino << 32) | (del->version_id & 0xffffffffULL);
    if (fs_state.version_cache)
        lru_cache_remove(fs_state.version_cache, key);

    if (meta)
    {
        if (meta->version_count > 0)
            meta->version_count--;
        if (meta->latest_version_id == del->version_id)
            meta->latest_version_id = chain->head ? chain->head->version_id : 0;
    }

    if (added_bytes)
        *added_bytes += newly_materialized;

    free(del);
    chain->count--;
    return prev;
}

/* 简单滚动哈希（多项式）用于块级变化检测 */
static uint32_t rolling_checksum(const char *data, size_t size)
{
    if (!data || size == 0)
        return 0;
    const uint32_t base = 1315423911u;
    uint32_t hash = 0;
    for (size_t i = 0; i < size; i++)
    {
        hash ^= ((hash << 5) + (uint8_t)data[i] + (hash >> 2)) + base;
    }
    return hash;
}

/* 递归/迭代获取块快照数据：如果当前版本未持有数据，则向父版本继承 */
static int snapshot_get_block_data(const version_node_t *vn, uint64_t block_index, const char **data, size_t *size)
{
    const version_node_t *cur = vn;
    while (cur)
    {
        if (block_index < cur->snapshot_count)
        {
            const version_block_snapshot_t *snap = &cur->snapshots[block_index];
            if (snap->has_data && snap->data)
            {
                if (data)
                    *data = snap->data;
                if (size)
                    *size = snap->size;
                return 0;
            }
        }
        cur = cur->parent;
    }
    return -ENOENT;
}

/* 在持有 chain->lock 的情况下执行保留策略（数量/时间/容量） */
static void version_apply_retention_locked(version_chain_t *chain, file_metadata_t *meta, time_t now)
{
    if (!chain)
        return;

    if (meta && meta->version_pinned)
        return;

    size_t keep = fs_state.version_max_versions ? fs_state.version_max_versions : fs_state.version_retention_count;
    uint32_t expire_days = fs_state.version_expire_days ? fs_state.version_expire_days : fs_state.version_retention_days;
    uint64_t size_limit = fs_state.version_retention_size_mb ? (fs_state.version_retention_size_mb * 1024ULL * 1024ULL) : 0ULL;

    /* 预计算总占用（仅增量存储的差异块大小） */
    uint64_t total_bytes = 0;
    for (version_node_t *cur = chain->head; cur; cur = cur->next)
        total_bytes += cur->stored_bytes;

    version_node_t *cur = chain->tail; /* 从最旧开始清理 */
    while (cur)
    {
        if (cur->is_important)
        {
            cur = cur->prev;
            continue;
        }

        int remove = 0;
        if (chain->count > keep && (now - cur->create_time) > (time_t)(expire_days * 24 * 3600))
            remove = 1;
        if (!remove && size_limit && total_bytes > size_limit && chain->count > 1)
            remove = 1;

        if (remove)
        {
            version_node_t *prev = cur->prev;
            total_bytes = (total_bytes > cur->stored_bytes) ? (total_bytes - cur->stored_bytes) : 0;
            uint64_t added = 0;
            version_remove_node_locked(chain, cur, meta, &added);
            total_bytes += added;
            cur = prev;
            continue;
        }

        cur = cur->prev;
    }
}

/* 内部帮助函数：查找或创建version_chain */
static version_chain_t *get_or_create_chain(uint64_t file_ino)
{
    pthread_mutex_lock(&versions_mutex);
    version_chain_t *chain = hash_table_get(versions_by_file, file_ino);
    if (!chain)
    {
        chain = calloc(1, sizeof(version_chain_t));
        if (!chain)
        {
            pthread_mutex_unlock(&versions_mutex);
            return NULL;
        }
        chain->file_ino = file_ino;
        pthread_rwlock_init(&chain->lock, NULL);
        hash_table_set(versions_by_file, file_ino, chain);
    }
    pthread_mutex_unlock(&versions_mutex);
    return chain;
}

time_t version_manager_parse_time_expr(const char *expr)
{
    if (!expr)
        return 0;
    time_t now = time(NULL);
    size_t len = strlen(expr);
    if (len == 0)
        return 0;
    if (strcmp(expr, "yesterday") == 0)
    {
        struct tm tmv;
        localtime_r(&now, &tmv);
        tmv.tm_mday -= 1;
        tmv.tm_hour = 0;
        tmv.tm_min = 0;
        tmv.tm_sec = 0;
        return mktime(&tmv);
    }
    if (strcmp(expr, "today") == 0)
    {
        struct tm tmv;
        localtime_r(&now, &tmv);
        tmv.tm_hour = 0;
        tmv.tm_min = 0;
        tmv.tm_sec = 0;
        return mktime(&tmv);
    }
    char unit = expr[len - 1];
    char buf[32] = {0};
    if (len - 1 < sizeof(buf))
        memcpy(buf, expr, len - 1);
    long val = strtol(buf, NULL, 10);
    if (val <= 0)
        return 0;
    if (unit == 's')
        return now - val;
    if (unit == 'h')
        return now - val * 3600;
    if (unit == 'd')
        return now - val * 24 * 3600;
    if (unit == 'w')
        return now - val * 7 * 24 * 3600;
    return 0;
}

size_t version_manager_collect_samples(version_history_sample_t *buf, size_t max_samples)
{
    if (!versions_by_file)
        return 0;
    size_t count = 0;

    pthread_mutex_lock(&versions_mutex);
    for (size_t i = 0; i < versions_by_file->size; i++)
    {
        hash_node_t *node = versions_by_file->buckets[i];
        while (node)
        {
            version_chain_t *chain = node->value;
            if (chain)
            {
                pthread_rwlock_rdlock(&chain->lock);
                for (version_node_t *vn = chain->head; vn; vn = vn->next)
                {
                    if (buf && count < max_samples)
                    {
                        buf[count].create_time = vn->create_time;
                        buf[count].file_size = vn->file_size;
                    }
                    count++;
                    if (buf && count >= max_samples)
                        break;
                }
                pthread_rwlock_unlock(&chain->lock);
            }
            if (buf && count >= max_samples)
                break;
            node = node->next;
        }
        if (buf && count >= max_samples)
            break;
    }
    pthread_mutex_unlock(&versions_mutex);
    return count;
}

int version_manager_init(void)
{
    versions_by_file = hash_table_create(4096);
    if (!versions_by_file)
        return -ENOMEM;

    /* 确保fs_state的版本缓存存在（若尚未初始化） */
    if (!fs_state.version_cache)
    {
        fs_state.version_cache = lru_cache_create(2000); /* 2k条版本元数据缓存 */
    }

    /* 默认配置 */
    if (fs_state.version_time_interval == 0)
        fs_state.version_time_interval = 3600; /* 1小时 */
    if (fs_state.version_retention_count == 0)
        fs_state.version_retention_count = 10;
    if (fs_state.version_retention_days == 0)
        fs_state.version_retention_days = 30;
    if (fs_state.version_max_versions == 0)
        fs_state.version_max_versions = fs_state.version_retention_count;
    if (fs_state.version_expire_days == 0)
        fs_state.version_expire_days = fs_state.version_retention_days;
    if (fs_state.version_retention_size_mb == 0)
        fs_state.version_retention_size_mb = 1024; /* 默认 1GB 上限，可通过 xattr 调整 */
    if (fs_state.version_clean_interval == 0)
        fs_state.version_clean_interval = fs_state.version_time_interval;

    /* 同步配置别名给模块C使用 */
    fs_state.max_versions = fs_state.version_max_versions;
    fs_state.expire_days = fs_state.version_expire_days;

    return 0;
}

void version_manager_destroy(void)
{
    /* 停止清理线程 */
    version_manager_stop_cleaner();

    if (!versions_by_file)
        return;

    /* 释放所有chains */
    pthread_rwlock_wrlock(&versions_by_file->lock);
    for (size_t i = 0; i < versions_by_file->size; i++)
    {
        hash_node_t *node = versions_by_file->buckets[i];
        while (node)
        {
            version_chain_t *chain = node->value;
            if (chain)
            {
                pthread_rwlock_wrlock(&chain->lock);
                version_node_t *vn = chain->head;
                while (vn)
                {
                    version_node_t *next = vn->next;
                    if (vn->snapshots)
                    {
                        for (size_t i = 0; i < vn->snapshot_count; i++)
                        {
                            if (vn->snapshots[i].has_data)
                                free(vn->snapshots[i].data);
                        }
                        free(vn->snapshots);
                    }
                    if (vn->description)
                        free(vn->description);
                    free(vn->diff_blocks);
                    free(vn->block_checksums);
                    free(vn);
                    vn = next;
                }
                pthread_rwlock_unlock(&chain->lock);
                pthread_rwlock_destroy(&chain->lock);
                free(chain);
            }
            node = node->next;
        }
    }
    pthread_rwlock_unlock(&versions_by_file->lock);

    hash_table_destroy(versions_by_file);
    versions_by_file = NULL;

    if (fs_state.version_cache)
    {
        lru_cache_destroy(fs_state.version_cache);
        fs_state.version_cache = NULL;
    }
}

/* 创建版本：基于块校验和计算差异，仅保存差异块的索引和每个块的校验和快照 */
int version_manager_create_version(file_metadata_t *meta, const char *reason)
{
    if (!meta)
        return -EINVAL;

    block_map_t *map = get_block_map(meta->ino);
    if (!map)
        return -ENOMEM;

    version_chain_t *chain = get_or_create_chain(meta->ino);
    if (!chain)
        return -ENOMEM;

    version_node_t *vn = calloc(1, sizeof(version_node_t));
    if (!vn)
        return -ENOMEM;

    /* 生成顺序版本号（每个文件内部递增） */
    pthread_rwlock_wrlock(&chain->lock);
    uint64_t next_vid = chain->head ? (chain->head->version_id + 1) : 1;
    vn->version_id = next_vid;
    vn->parent_id = chain->head ? chain->head->version_id : 0;
    vn->parent = chain->head;
    vn->create_time = time(NULL);
    vn->description = reason ? strdup(reason) : NULL;
    vn->block_map = map;
    map->version_id = vn->version_id;

    /* 记录当前块校验和与数据快照 */
    pthread_rwlock_rdlock(&map->lock);
    vn->block_count = map->block_count;
    vn->snapshot_count = map->block_count;
    vn->file_size = meta->size;
    vn->blocks = meta->blocks;
    if (vn->block_count)
    {
        vn->block_checksums = calloc(vn->block_count, sizeof(uint32_t));
        vn->snapshots = calloc(vn->block_count, sizeof(version_block_snapshot_t));
        vn->diff_blocks = calloc(vn->block_count ? vn->block_count : 1, sizeof(uint64_t));
        if (!vn->block_checksums || !vn->snapshots || !vn->diff_blocks)
        {
            pthread_rwlock_unlock(&map->lock);
            pthread_rwlock_unlock(&chain->lock);
            free(vn->block_checksums);
            free(vn->snapshots);
            free(vn->diff_blocks);
            if (vn->description)
                free(vn->description);
            free(vn);
            return -ENOMEM;
        }

        for (size_t i = 0; i < vn->block_count; i++)
        {
            data_block_t *b = map->blocks[i];
            uint32_t prev = (vn->parent && i < vn->parent->block_count) ? vn->parent->block_checksums[i] : 0;
            uint32_t cur = 0;
            size_t plain_size = 0;
            const char *plain = NULL;

            if (b && b->data)
            {
                char *owned = NULL;
                plain_size = b->size;
                plain = b->data;
                if (b->compressed_size > 0 && b->compression != COMPRESSION_NONE)
                {
                    if (block_decompress(b, &owned, &plain_size) == 0 && owned)
                        plain = owned;
                    else
                        plain_size = b->size;
                }

                cur = rolling_checksum(plain, plain_size);

                if (cur != prev || !vn->parent)
                {
                    vn->snapshots[i].size = plain_size;
                    vn->snapshots[i].data = malloc(plain_size);
                    if (vn->snapshots[i].data)
                    {
                        memcpy(vn->snapshots[i].data, plain, plain_size);
                        vn->snapshots[i].has_data = true;
                        vn->stored_bytes += plain_size;
                    }
                    vn->diff_blocks[vn->diff_count++] = i;
                }
                else
                {
                    vn->snapshots[i].size = 0;
                    vn->snapshots[i].data = NULL;
                    vn->snapshots[i].has_data = false;
                }

                if (owned)
                    free(owned);
            }
            else
            {
                cur = 0;
                vn->snapshots[i].size = 0;
                vn->snapshots[i].data = NULL;
                vn->snapshots[i].has_data = false;
            }

            vn->block_checksums[i] = cur;
        }
    }
    pthread_rwlock_unlock(&map->lock);

    /* 将新版本插入链表头部 */
    vn->next = chain->head;
    vn->prev = NULL;
    if (chain->head)
        chain->head->prev = vn;
    chain->head = vn;
    if (!chain->tail)
        chain->tail = vn;
    chain->count++;

    /* 即刻应用保留策略，避免后台清理延迟（含容量限制） */
    version_apply_retention_locked(chain, meta, time(NULL));

    /* 更新文件元数据中的版本计数 */
    meta->version_count++;
    meta->latest_version_id = vn->version_id;
    meta->last_version_time = vn->create_time;
    /* 将版本元数据缓存到 fs_state.version_cache 以便快速访问 */
    file_metadata_t *vmeta = malloc(sizeof(file_metadata_t));
    if (vmeta)
    {
        memcpy(vmeta, meta, sizeof(file_metadata_t));
        vmeta->type = FT_VERSIONED;
        /* 组合key：高32位为ino，低32位为version_id */
        uint64_t key = (meta->ino << 32) | (vn->version_id & 0xffffffffULL);
        vmeta->version_handle = vn;
        lru_cache_put(fs_state.version_cache, key, vmeta);
    }

    pthread_rwlock_unlock(&chain->lock);

    predict_storage_usage_internal(7, NULL);

    return 0;
}

int version_manager_create_manual(file_metadata_t *meta, const char *reason)
{
    return version_manager_create_version(meta, reason ? reason : "manual");
}

/* 基于当前文件与最新版本的块差异（>10%）自动建版 */
int version_manager_maybe_change_snapshot(file_metadata_t *meta)
{
    if (!meta)
        return -EINVAL;

    version_chain_t *chain = hash_table_get(versions_by_file, meta->ino);
    if (!chain || !chain->head)
        return 0; /* 没有历史版本，不触发 */

    block_map_t *map = get_block_map(meta->ino);
    if (!map)
        return -ENOMEM;

    pthread_rwlock_rdlock(&map->lock);
    size_t block_count = map->block_count;
    if (block_count == 0)
    {
        pthread_rwlock_unlock(&map->lock);
        return 0;
    }

    size_t diffcnt = 0;
    for (size_t i = 0; i < block_count; i++)
    {
        data_block_t *b = map->blocks[i];
        uint32_t cur = 0;
        if (b && b->data)
        {
            char *owned = NULL;
            size_t plain_size = b->size;
            const char *plain = b->data;
            if (b->compressed_size > 0 && b->compression != COMPRESSION_NONE)
            {
                if (block_decompress(b, &owned, &plain_size) == 0 && owned)
                    plain = owned;
                else
                    plain_size = b->size;
            }
            cur = rolling_checksum(plain, plain_size);
            if (owned)
                free(owned);
        }
        uint32_t prev = (i < chain->head->block_count) ? chain->head->block_checksums[i] : 0;
        if (cur != prev)
            diffcnt++;
    }
    pthread_rwlock_unlock(&map->lock);

    double ratio = (double)diffcnt / (double)block_count;
    if (ratio > 0.10)
    {
        return version_manager_create_version(meta, "content-change");
    }
    return 0;
}

/* 定时策略：若距离上次版本超过 interval，创建新版本 */
int version_manager_create_periodic(file_metadata_t *meta, const char *reason)
{
    if (!meta)
        return -EINVAL;
    time_t now = time(NULL);
    uint32_t interval = fs_state.version_time_interval ? fs_state.version_time_interval : 3600;
    if (meta->last_version_time == 0 || (now - meta->last_version_time) >= (time_t)interval)
    {
        return version_manager_create_version(meta, reason ? reason : "periodic");
    }
    return 0;
}

/* 根据版本字符串（v<num> / latest）返回版本元数据副本 */
file_metadata_t *version_manager_get_version_meta(file_metadata_t *meta, const char *verstr)
{
    if (!meta || !verstr)
        return NULL;

    version_chain_t *chain = hash_table_get(versions_by_file, meta->ino);
    if (!chain)
        return NULL;

    pthread_rwlock_rdlock(&chain->lock);

    uint64_t want = 0;
    time_t target_time = 0;
    if (strcmp(verstr, "latest") == 0)
    {
        if (chain->head)
            want = chain->head->version_id;
    }
    else if (verstr[0] == 'v')
    {
        char *endptr = NULL;
        long v = strtol(verstr + 1, &endptr, 10);
        if (v > 0)
            want = (uint64_t)v;
    }
    else
    {
        target_time = version_manager_parse_time_expr(verstr);
    }

    version_node_t *vn = chain->head;
    version_node_t *best_time = NULL;
    while (vn)
    {
        if (want && vn->version_id == want)
            break;
        if (target_time && vn->create_time <= target_time)
        {
            if (!best_time || vn->create_time > best_time->create_time)
                best_time = vn;
        }
        vn = vn->next;
    }

    if (!vn && best_time)
        vn = best_time;

    if (!vn)
    {
        pthread_rwlock_unlock(&chain->lock);
        return NULL;
    }

    /* 从缓存尝试取 */
    uint64_t key = (meta->ino << 32) | (vn->version_id & 0xffffffffULL);
    file_metadata_t *cached = (file_metadata_t *)lru_cache_get(fs_state.version_cache, key);
    if (cached)
    {
        /* 返回副本，避免外部修改缓存对象 */
        file_metadata_t *copy = malloc(sizeof(file_metadata_t));
        if (copy)
            memcpy(copy, cached, sizeof(file_metadata_t));
        pthread_rwlock_unlock(&chain->lock);
        return copy;
    }

    /* 否则，基于原始meta构造一个版本元数据副本 */
    file_metadata_t *vmeta = malloc(sizeof(file_metadata_t));
    if (!vmeta)
    {
        pthread_rwlock_unlock(&chain->lock);
        return NULL;
    }
    memcpy(vmeta, meta, sizeof(file_metadata_t));
    vmeta->type = FT_VERSIONED;
    vmeta->version = (uint32_t)vn->version_id;
    vmeta->size = vn->file_size;
    vmeta->blocks = vn->blocks;
    vmeta->version_handle = vn;
    /* 不改变 ino，使得读取流程简化（真实实现可分配独立ino） */

    /* 缓存版本元数据 */
    lru_cache_put(fs_state.version_cache, key, vmeta);

    pthread_rwlock_unlock(&chain->lock);
    return vmeta;
}

int version_manager_list_versions(file_metadata_t *meta, char ***out_list, size_t *out_count)
{
    if (!meta || !out_list || !out_count)
        return -EINVAL;

    version_chain_t *chain = hash_table_get(versions_by_file, meta->ino);
    if (!chain)
    {
        *out_list = NULL;
        *out_count = 0;
        return 0;
    }

    pthread_rwlock_rdlock(&chain->lock);
    size_t n = chain->count;
    char **list = calloc(n, sizeof(char *));
    if (!list)
    {
        pthread_rwlock_unlock(&chain->lock);
        return -ENOMEM;
    }

    version_node_t *vn = chain->head;
    size_t i = 0;
    while (vn && i < n)
    {
        char buf[128];
        char tbuf[32] = {0};
        struct tm tmv;
        localtime_r(&vn->create_time, &tmv);
        strftime(tbuf, sizeof(tbuf), "%F %T", &tmv);
        const char *desc = vn->description ? vn->description : "auto";
        snprintf(buf, sizeof(buf), "v%llu | %s | %s", (unsigned long long)vn->version_id, tbuf, desc);
        list[i] = strdup(buf);
        i++;
        vn = vn->next;
    }

    pthread_rwlock_unlock(&chain->lock);

    *out_list = list;
    *out_count = n;
    return 0;
}

uint64_t version_manager_get_version_by_time(uint64_t ino, time_t target_time)
{
    if (!target_time)
        return 0;

    version_chain_t *chain = hash_table_get(versions_by_file, ino);
    if (!chain)
        return 0;

    pthread_rwlock_rdlock(&chain->lock);
    version_node_t *vn = chain->head;
    uint64_t best = 0;
    time_t best_time = 0;
    while (vn)
    {
        if (vn->create_time <= target_time && vn->create_time >= best_time)
        {
            best = vn->version_id;
            best_time = vn->create_time;
        }
        vn = vn->next;
    }
    pthread_rwlock_unlock(&chain->lock);
    return best;
}

int version_manager_read_version_data(file_metadata_t *vmeta, char *buf, size_t size, off_t offset)
{
    if (!vmeta || !buf || !vmeta->version_handle)
        return -EINVAL;

    version_node_t *vn = (version_node_t *)vmeta->version_handle;
    size_t fsize = vn->file_size;
    if (offset < 0 || (size_t)offset >= fsize)
        return 0;
    size_t remaining = fsize - offset;
    if (size > remaining)
        size = remaining;

    size_t bytes_read = 0;
    size_t current_offset = offset;
    while (bytes_read < size)
    {
        uint64_t block_index = current_offset / fs_state.block_size;
        size_t block_offset = current_offset % fs_state.block_size;
        size_t bytes_to_read = fs_state.block_size - block_offset;
        if (bytes_to_read > (size - bytes_read))
            bytes_to_read = size - bytes_read;

        const char *blk = NULL;
        size_t avail = 0;
        if (block_index < vn->snapshot_count)
            snapshot_get_block_data(vn, block_index, &blk, &avail);

        if (!blk || block_offset >= avail)
        {
            memset(buf + bytes_read, 0, bytes_to_read);
        }
        else
        {
            size_t copy_len = bytes_to_read;
            if (block_offset + copy_len > avail)
                copy_len = avail - block_offset;
            memcpy(buf + bytes_read, blk + block_offset, copy_len);
            if (copy_len < bytes_to_read)
                memset(buf + bytes_read + copy_len, 0, bytes_to_read - copy_len);
        }

        bytes_read += bytes_to_read;
        current_offset += bytes_to_read;
    }

    return (int)bytes_read;
}

int version_manager_delete_version(uint64_t ino, uint64_t version_id)
{
    if (!version_id)
        return -EINVAL;

    version_chain_t *chain = hash_table_get(versions_by_file, ino);
    if (!chain)
        return -ENOENT;

    pthread_rwlock_wrlock(&chain->lock);
    version_node_t *cur = chain->head;
    while (cur)
    {
        if (cur->version_id == version_id)
            break;
        cur = cur->next;
    }

    if (!cur)
    {
        pthread_rwlock_unlock(&chain->lock);
        return -ENOENT;
    }

    if (cur->is_important)
    {
        pthread_rwlock_unlock(&chain->lock);
        return -EPERM;
    }

    file_metadata_t *meta = lookup_inode(ino);
    version_remove_node_locked(chain, cur, meta, NULL);
    pthread_rwlock_unlock(&chain->lock);
    return 0;
}

int version_manager_mark_important(uint64_t ino, uint64_t version_id, bool important)
{
    version_chain_t *chain = hash_table_get(versions_by_file, ino);
    if (!chain)
        return -ENOENT;
    pthread_rwlock_wrlock(&chain->lock);
    version_node_t *vn = chain->head;
    while (vn)
    {
        if (vn->version_id == version_id)
        {
            vn->is_important = important;
            pthread_rwlock_unlock(&chain->lock);
            return 0;
        }
        vn = vn->next;
    }
    pthread_rwlock_unlock(&chain->lock);
    return -ENOENT;
}

int version_manager_diff(file_metadata_t *meta, uint64_t v1, uint64_t v2, char **out_diff)
{
    if (!meta || !out_diff)
        return -EINVAL;

    version_chain_t *chain = hash_table_get(versions_by_file, meta->ino);
    if (!chain)
        return -ENOENT;

    pthread_rwlock_rdlock(&chain->lock);
    version_node_t *a = NULL, *b = NULL;
    version_node_t *vn = chain->head;
    while (vn)
    {
        if (vn->version_id == v1) a = vn;
        if (vn->version_id == v2) b = vn;
        vn = vn->next;
    }

    if (!a || !b)
    {
        pthread_rwlock_unlock(&chain->lock);
        return -ENOENT;
    }

    /* 简要统计不同块数量 */
    size_t maxb = a->block_count > b->block_count ? a->block_count : b->block_count;
    size_t diffcnt = 0;
    for (size_t i = 0; i < maxb; i++)
    {
        uint32_t ca = i < a->block_count ? a->block_checksums[i] : 0;
        uint32_t cb = i < b->block_count ? b->block_checksums[i] : 0;
        if (ca != cb)
            diffcnt++;
    }

    char *res = malloc(128);
    if (res)
        snprintf(res, 128, "diff_blocks=%zu (of %zu blocks)", diffcnt, maxb);

    *out_diff = res;
    pthread_rwlock_unlock(&chain->lock);
    return 0;
}

/* 后台清理线程函数 */
static void *version_cleaner_thread_fn(void *arg)
{
    (void)arg;
    cleaner_running = 1;
    while (cleaner_running)
    {
        /* 轮询所有文件的版本链，按保留策略删除过期版本 */
        time_t now = time(NULL);
        pthread_mutex_lock(&versions_mutex);
        for (size_t i = 0; i < versions_by_file->size; i++)
        {
            hash_node_t *node = versions_by_file->buckets[i];
            while (node)
            {
                version_chain_t *chain = node->value;
                if (chain)
                {
                    file_metadata_t *meta = lookup_inode(chain->file_ino);
                    if (meta && meta->version_pinned)
                    {
                        node = node->next;
                        continue;
                    }

                    if (meta)
                        version_manager_create_periodic(meta, "periodic");

                    pthread_rwlock_wrlock(&chain->lock);
                    version_apply_retention_locked(chain, meta, now);
                    pthread_rwlock_unlock(&chain->lock);
                }
                node = node->next;
            }
        }
        pthread_mutex_unlock(&versions_mutex);

        /* 刷新缓存脏页（复用清理调度周期） */
        cache_flush_l2_dirty();

        /* 休眠一段时间后继续 */
        sleep(fs_state.version_clean_interval ? fs_state.version_clean_interval : 3600);
    }
    return NULL;
}

int version_manager_start_cleaner(void)
{
    if (fs_state.version_cleaner_thread)
        return 0; /* 已启动 */

    if (pthread_create(&fs_state.version_cleaner_thread, NULL, version_cleaner_thread_fn, NULL) != 0)
    {
        return -errno;
    }
    return 0;
}

void version_manager_stop_cleaner(void)
{
    if (!fs_state.version_cleaner_thread)
        return;
    cleaner_running = 0;
    pthread_join(fs_state.version_cleaner_thread, NULL);
    fs_state.version_cleaner_thread = 0;
}
