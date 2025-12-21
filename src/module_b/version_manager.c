/**
 * Module B: Version manager (透明版本管理)
 *
 * 提供基础的版本创建、列表、访问与清理功能。
 * 设计原则：复用 module A 的数据结构与缓存，轻量实现以便快速集成。
 */

#include "version_manager.h"
#include "smartbackupfs.h"
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

block_map_t *get_block_map(uint64_t file_ino);
file_metadata_t *lookup_inode(uint64_t ino);

/* 全局版本链表按文件ino组织 */
static hash_table_t *versions_by_file = NULL; /* key: file_ino -> value: version_chain_t* */
static pthread_mutex_t versions_mutex = PTHREAD_MUTEX_INITIALIZER;
static int cleaner_running = 0;

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
        return now - 24 * 3600;
    char unit = expr[len - 1];
    char buf[32] = {0};
    if (len - 1 < sizeof(buf))
        memcpy(buf, expr, len - 1);
    long val = strtol(buf, NULL, 10);
    if (val <= 0)
        return 0;
    if (unit == 'h')
        return now - val * 3600;
    if (unit == 'd')
        return now - val * 24 * 3600;
    return 0;
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
    vn->version_id = (uint64_t)(chain->count + 1);
    vn->parent_version = chain->head ? chain->head->version_id : 0;
    vn->create_time = time(NULL);

    /* 记录当前块校验和快照（滚动哈希） */
    pthread_rwlock_rdlock(&map->lock);
    vn->block_count = map->block_count;
    if (vn->block_count)
    {
        vn->block_checksums = calloc(vn->block_count, sizeof(uint32_t));
        if (!vn->block_checksums)
        {
            pthread_rwlock_unlock(&map->lock);
            pthread_rwlock_unlock(&chain->lock);
            free(vn);
            return -ENOMEM;
        }

        for (size_t i = 0; i < vn->block_count; i++)
        {
            data_block_t *b = map->blocks[i];
            if (b && b->data)
                vn->block_checksums[i] = rolling_checksum(b->data, b->size);
            else
                vn->block_checksums[i] = 0;
        }
    }
    pthread_rwlock_unlock(&map->lock);

    /* 计算与父版本的差异块索引 */
    if (vn->parent_version && chain->head && chain->head->block_checksums)
    {
        /* 最多记录全部块索引，实际会更小 */
        vn->diff_blocks = calloc(vn->block_count ? vn->block_count : 1, sizeof(uint64_t));
        if (!vn->diff_blocks)
        {
            free(vn->block_checksums);
            pthread_rwlock_unlock(&chain->lock);
            free(vn);
            return -ENOMEM;
        }

        for (size_t i = 0; i < vn->block_count; i++)
        {
            uint32_t prev = 0;
            if (i < chain->head->block_count)
                prev = chain->head->block_checksums[i];
            uint32_t cur = vn->block_checksums[i];
            if (cur != prev)
            {
                vn->diff_blocks[vn->diff_count++] = i;
            }
        }
    }
    else
    {
        /* 没有父版本，全部视为diff */
        if (vn->block_count)
        {
            vn->diff_blocks = calloc(vn->block_count, sizeof(uint64_t));
            if (vn->diff_blocks)
            {
                for (size_t i = 0; i < vn->block_count; i++)
                {
                    vn->diff_blocks[vn->diff_count++] = i;
                }
            }
        }
    }

    /* 将新版本插入链表头部 */
    vn->next = chain->head;
    chain->head = vn;
    chain->count++;

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
        lru_cache_put(fs_state.version_cache, key, vmeta);
    }

    pthread_rwlock_unlock(&chain->lock);

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
        uint32_t cur = (b && b->data) ? rolling_checksum(b->data, b->size) : 0;
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
        if (endptr && *endptr == '\0' && v > 0)
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

    if (!vn && target_time)
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
        char buf[64];
        snprintf(buf, sizeof(buf), "v%llu", (unsigned long long)vn->version_id);
        list[i] = strdup(buf);
        i++;
        vn = vn->next;
    }

    pthread_rwlock_unlock(&chain->lock);

    *out_list = list;
    *out_count = n;
    return 0;
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
                    /* 若文件被 pinned，跳过清理与周期创建 */
                    file_metadata_t *meta = lookup_inode(chain->file_ino);
                    if (meta && meta->version_pinned)
                    {
                        node = node->next;
                        continue;
                    }

                    /* 周期性创建版本 */
                    if (meta)
                        version_manager_create_periodic(meta, "periodic");

                    pthread_rwlock_wrlock(&chain->lock);
                    /* 保留策略：保留最近 N 个版本以及指定天数内的版本 */
                    size_t keep = fs_state.version_retention_count;
                    version_node_t *prev = NULL;
                    version_node_t *cur = chain->head;
                    size_t idx = 0;
                    while (cur)
                    {
                        int remove = 0;
                        if (idx >= keep)
                        {
                            if ((now - cur->create_time) > (time_t)(fs_state.version_retention_days * 24 * 3600))
                                remove = 1;
                        }

                        if (remove)
                        {
                            version_node_t *del = cur;
                            if (prev)
                                prev->next = cur->next;
                            else
                                chain->head = cur->next;
                            cur = cur->next;
                            free(del->diff_blocks);
                            free(del->block_checksums);
                            free(del);
                            chain->count--;
                        }
                        else
                        {
                            prev = cur;
                            cur = cur->next;
                            idx++;
                        }
                    }
                    pthread_rwlock_unlock(&chain->lock);
                }
                node = node->next;
            }
        }
        pthread_mutex_unlock(&versions_mutex);

        /* 休眠一段时间后继续 */
        sleep(fs_state.version_time_interval ? fs_state.version_time_interval : 3600);
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
