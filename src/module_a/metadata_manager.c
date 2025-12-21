/**
 * 元数据管理模块
 */

#include "smartbackupfs.h"
#include "version_manager.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

// 哈希表辅助函数
static uint64_t hash_function(uint64_t key, size_t size)
{
    // 使用FNV-1a哈希算法的变体
    uint64_t hash = 14695981039346656037ULL;
    key ^= 14695981039346656037ULL;
    for (int i = 0; i < 8; i++)
    {
        hash *= 1099511628211ULL;
        hash ^= (key >> (i * 8)) & 0xFF;
    }
    return hash % size;
}

// 创建哈希表
hash_table_t *hash_table_create(size_t size)
{
    hash_table_t *table = malloc(sizeof(hash_table_t));
    if (!table)
        return NULL;

    table->buckets = calloc(size, sizeof(hash_node_t *));
    if (!table->buckets)
    {
        free(table);
        return NULL;
    }

    table->size = size;
    table->count = 0;
    pthread_rwlock_init(&table->lock, NULL);

    return table;
}

// 销毁哈希表
void hash_table_destroy(hash_table_t *table)
{
    if (!table)
        return;

    pthread_rwlock_wrlock(&table->lock);

    for (size_t i = 0; i < table->size; i++)
    {
        hash_node_t *node = table->buckets[i];
        while (node)
        {
            hash_node_t *next = node->next;
            free(node);
            node = next;
        }
    }

    free(table->buckets);
    pthread_rwlock_unlock(&table->lock);
    pthread_rwlock_destroy(&table->lock);
    free(table);
}

// 设置哈希表值
int hash_table_set(hash_table_t *table, uint64_t key, void *value)
{
    if (!table || !value)
        return -EINVAL;

    pthread_rwlock_wrlock(&table->lock);

    uint64_t index = hash_function(key, table->size);
    hash_node_t *node = table->buckets[index];

    // 检查是否已存在
    while (node)
    {
        if (node->key == key)
        {
            node->value = value;
            node->access_time = time(NULL);
            pthread_rwlock_unlock(&table->lock);
            return 0;
        }
        node = node->next;
    }

    // 创建新节点
    hash_node_t *new_node = malloc(sizeof(hash_node_t));
    if (!new_node)
    {
        pthread_rwlock_unlock(&table->lock);
        return -ENOMEM;
    }

    new_node->key = key;
    new_node->value = value;
    new_node->next = table->buckets[index];
    new_node->access_time = time(NULL);

    table->buckets[index] = new_node;
    table->count++;

    pthread_rwlock_unlock(&table->lock);
    return 0;
}

// 获取哈希表值
void *hash_table_get(hash_table_t *table, uint64_t key)
{
    if (!table)
        return NULL;

    pthread_rwlock_rdlock(&table->lock);

    uint64_t index = hash_function(key, table->size);
    hash_node_t *node = table->buckets[index];

    while (node)
    {
        if (node->key == key)
        {
            node->access_time = time(NULL);
            void *value = node->value;
            pthread_rwlock_unlock(&table->lock);
            return value;
        }
        node = node->next;
    }

    pthread_rwlock_unlock(&table->lock);
    return NULL;
}

// 移除哈希表键值对
int hash_table_remove(hash_table_t *table, uint64_t key)
{
    if (!table)
        return -EINVAL;

    pthread_rwlock_wrlock(&table->lock);

    uint64_t index = hash_function(key, table->size);
    hash_node_t **prev = &table->buckets[index];
    hash_node_t *node = *prev;

    while (node)
    {
        if (node->key == key)
        {
            *prev = node->next;
            free(node);
            table->count--;
            pthread_rwlock_unlock(&table->lock);
            return 0;
        }
        prev = &node->next;
        node = node->next;
    }

    pthread_rwlock_unlock(&table->lock);
    return -ENOENT;
}

// 清空哈希表
void hash_table_clear(hash_table_t *table)
{
    if (!table)
        return;

    pthread_rwlock_wrlock(&table->lock);

    for (size_t i = 0; i < table->size; i++)
    {
        hash_node_t *node = table->buckets[i];
        while (node)
        {
            hash_node_t *next = node->next;
            free(node);
            node = next;
        }
        table->buckets[i] = NULL;
    }

    table->count = 0;
    pthread_rwlock_unlock(&table->lock);
}

// 获取哈希表大小
size_t hash_table_size(hash_table_t *table)
{
    if (!table)
        return 0;
    return table->count;
}

// 创建LRU缓存
lru_cache_t *lru_cache_create(size_t max_size)
{
    lru_cache_t *cache = malloc(sizeof(lru_cache_t));
    if (!cache)
        return NULL;

    cache->table = hash_table_create(max_size * 2); // 使用2倍大小减少冲突
    if (!cache->table)
    {
        free(cache);
        return NULL;
    }

    cache->max_size = max_size;
    cache->current_size = 0;
    pthread_mutex_init(&cache->mutex, NULL);

    return cache;
}

// 销毁LRU缓存
void lru_cache_destroy(lru_cache_t *cache)
{
    if (!cache)
        return;

    hash_table_destroy(cache->table);
    pthread_mutex_destroy(&cache->mutex);
    free(cache);
}

// LRU缓存放入
int lru_cache_put(lru_cache_t *cache, uint64_t key, void *value)
{
    if (!cache || !value)
        return -EINVAL;

    pthread_mutex_lock(&cache->mutex);

    // 检查是否需要淘汰
    if (hash_table_size(cache->table) >= cache->max_size)
    {
        // 简化实现：随机淘汰一个项
        for (size_t i = 0; i < cache->table->size; i++)
        {
            hash_node_t *node = cache->table->buckets[i];
            if (node)
            {
                hash_table_remove(cache->table, node->key);
                break;
            }
        }
    }

    int result = hash_table_set(cache->table, key, value);
    pthread_mutex_unlock(&cache->mutex);

    return result;
}

// LRU缓存获取
void *lru_cache_get(lru_cache_t *cache, uint64_t key)
{
    if (!cache)
        return NULL;

    pthread_mutex_lock(&cache->mutex);
    void *value = hash_table_get(cache->table, key);
    pthread_mutex_unlock(&cache->mutex);

    return value;
}

// LRU缓存移除
int lru_cache_remove(lru_cache_t *cache, uint64_t key)
{
    if (!cache)
        return -EINVAL;

    pthread_mutex_lock(&cache->mutex);
    int result = hash_table_remove(cache->table, key);
    pthread_mutex_unlock(&cache->mutex);

    return result;
}

// LRU缓存清空
void lru_cache_clear(lru_cache_t *cache)
{
    if (!cache)
        return;

    pthread_mutex_lock(&cache->mutex);
    hash_table_clear(cache->table);
    pthread_mutex_unlock(&cache->mutex);
}

// 全局文件系统状态
fs_state_t fs_state;

// 块映射管理
hash_table_t *block_maps = NULL;
pthread_mutex_t block_maps_mutex = PTHREAD_MUTEX_INITIALIZER;

// 静态缓存变量
static lru_cache_t *inode_cache = NULL;
static lru_cache_t *block_cache = NULL;
static pthread_once_t cache_once = PTHREAD_ONCE_INIT;

// 缓存初始化函数
static void init_caches(void)
{
    inode_cache = lru_cache_create(10000); // 10K个inode缓存
    block_cache = lru_cache_create(5000);  // 5K个数据块缓存
}

// 初始化文件系统
void fs_init(void)
{
    memset(&fs_state, 0, sizeof(fs_state_t));

    // 初始化锁
    pthread_mutex_init(&fs_state.ino_mutex, NULL);
    pthread_rwlock_init(&fs_state.cache_lock, NULL);

    // 初始化块映射管理
    block_maps = hash_table_create(10000);

    // 设置配置
    fs_state.block_size = DEFAULT_BLOCK_SIZE;
    fs_state.max_cache_size = MAX_CACHE_SIZE;
    fs_state.enable_compression = false;   // 初始关闭
    fs_state.enable_deduplication = false; // 初始关闭

    // 创建根目录
    fs_state.root = calloc(1, sizeof(directory_t));
    if (!fs_state.root)
    {
        perror("Failed to allocate root directory");
        exit(EXIT_FAILURE);
    }

    fs_state.root->meta.ino = 1;
    fs_state.root->meta.type = FT_DIRECTORY;
    fs_state.root->meta.mode = S_IFDIR | 0755;
    fs_state.root->meta.nlink = 2;
    fs_state.root->meta.uid = getuid();
    fs_state.root->meta.gid = getgid();
    fs_state.root->meta.size = DEFAULT_BLOCK_SIZE;
    fs_state.root->meta.blocks = 1;
    fs_state.root->meta.version = 1;

    clock_gettime(CLOCK_REALTIME, &fs_state.root->meta.atime);
    fs_state.root->meta.mtime = fs_state.root->meta.atime;
    fs_state.root->meta.ctime = fs_state.root->meta.atime;

    pthread_rwlock_init(&fs_state.root->lock, NULL);
    fs_state.root->entries = NULL;
    fs_state.root->entry_count = 0;

    fs_state.next_ino = 2;
    fs_state.total_dirs = 1;
    fs_state.total_files = 0;
    fs_state.total_blocks = 0;
    fs_state.used_blocks = 0;

    /* 初始化模块B：版本管理 */
    version_manager_init();
    /* 启动版本清理后台线程 */
    version_manager_start_cleaner();
}

// 销毁文件系统
void fs_destroy(void)
{
    // 清理根目录
    if (fs_state.root)
    {
        pthread_rwlock_wrlock(&fs_state.root->lock);

        dir_entry_t *entry = fs_state.root->entries;
        while (entry)
        {
            dir_entry_t *next = entry->next;
            free(entry->name);
            free_inode(entry->meta);
            free(entry);
            entry = next;
        }

        pthread_rwlock_unlock(&fs_state.root->lock);
        pthread_rwlock_destroy(&fs_state.root->lock);
        free(fs_state.root);
    }

    // 清理缓存
    cache_clear();

    /* 销毁版本管理模块 */
    version_manager_destroy();

    // 销毁锁
    pthread_mutex_destroy(&fs_state.ino_mutex);
    pthread_rwlock_destroy(&fs_state.cache_lock);
}

// 创建新的inode
file_metadata_t *create_inode(file_type_t type, mode_t mode)
{
    pthread_mutex_lock(&fs_state.ino_mutex);

    file_metadata_t *meta = calloc(1, sizeof(file_metadata_t));
    if (!meta)
    {
        pthread_mutex_unlock(&fs_state.ino_mutex);
        return NULL;
    }

    meta->ino = fs_state.next_ino++;
    meta->type = type;

    switch (type)
    {
    case FT_DIRECTORY:
        meta->mode = S_IFDIR | (mode & 07777);
        meta->nlink = 2; // '.' 和父目录
        fs_state.total_dirs++;
        break;
    case FT_REGULAR:
        meta->mode = S_IFREG | (mode & 07777);
        meta->nlink = 1;
        fs_state.total_files++;
        break;
    case FT_SYMLINK:
        meta->mode = S_IFLNK | (mode & 07777);
        meta->nlink = 1;
        break;
    default:
        meta->mode = mode;
        meta->nlink = 1;
        break;
    }

    meta->uid = fuse_get_context()->uid;
    meta->gid = fuse_get_context()->gid;
    meta->size = 0;
    meta->blocks = 0;
    meta->version = 1;

    clock_gettime(CLOCK_REALTIME, &meta->atime);
    meta->mtime = meta->atime;
    meta->ctime = meta->atime;

    meta->xattr = NULL;
    meta->xattr_size = 0;

    pthread_mutex_unlock(&fs_state.ino_mutex);

    // 添加到缓存
    cache_set(meta->ino, meta);

    return meta;
}

// 释放inode
void free_inode(file_metadata_t *meta)
{
    if (!meta)
        return;

    // 从缓存移除
    cache_remove(meta->ino);

    // 清理扩展属性
    if (meta->xattr)
    {
        free(meta->xattr);
    }

    free(meta);
}

// 根据路径查找inode
file_metadata_t *lookup_path(const char *path)
{
    if (!path || path[0] != '/')
    {
        return NULL;
    }

    if (strcmp(path, "/") == 0)
    {
        return &fs_state.root->meta;
    }

    // 使用可重入的分词以避免并发问题
    char *path_copy = strdup(path);
    if (!path_copy)
    {
        return NULL;
    }

    char *saveptr = NULL;
    char *token = strtok_r(path_copy + 1, "/", &saveptr);

    directory_t *current_dir = fs_state.root;
    file_metadata_t *result = NULL;

    // 先锁住根目录再开始遍历
    pthread_rwlock_rdlock(&current_dir->lock);

    while (token)
    {
        /* 拆分 token 处理 @version/@versions 语法 */
        char *atpos = strchr(token, '@');
        char *base_token = NULL;
        char *ver_token = NULL;
        if (atpos)
        {
            base_token = strndup(token, atpos - token);
            ver_token = strdup(atpos + 1);
        }
        else
        {
            base_token = strdup(token);
        }

        dir_entry_t *entry = find_directory_entry(current_dir, base_token);
        free(base_token);
        if (!entry)
        {
            free(ver_token);
            pthread_rwlock_unlock(&current_dir->lock);
            result = NULL;
            break;
        }

        char *next_token = strtok_r(NULL, "/", &saveptr);
        if (!next_token)
        {
            if (ver_token)
            {
                if (strcmp(ver_token, "versions") == 0)
                {
                    /* 交由 readdir 处理，返回列表 */
                    free(ver_token);
                    result = entry->meta;
                    pthread_rwlock_unlock(&current_dir->lock);
                    break;
                }

                file_metadata_t *vmeta = version_manager_get_version_meta(entry->meta, ver_token);
                free(ver_token);
                result = vmeta;
                pthread_rwlock_unlock(&current_dir->lock);
                break;
            }

            /* 常规路径，直接返回 */
            result = entry->meta;
            pthread_rwlock_unlock(&current_dir->lock);
            break;
        }

        free(ver_token);

        // 需要继续向下，但目标不是目录，失败
        if (entry->meta->type != FT_DIRECTORY)
        {
            pthread_rwlock_unlock(&current_dir->lock);
            result = NULL;
            break;
        }

        // 切换到子目录：先锁住子目录，再释放父目录的锁，防止竞态
        directory_t *next_dir = (directory_t *)entry->meta;
        pthread_rwlock_rdlock(&next_dir->lock);
        pthread_rwlock_unlock(&current_dir->lock);

        current_dir = next_dir;
        token = next_token;
    }

    free(path_copy);
    return result;
}

// 根据inode编号查找
file_metadata_t *lookup_inode(uint64_t ino)
{
    return (file_metadata_t *)cache_get(ino);
}

// 添加目录项
int add_directory_entry(directory_t *dir, const char *name, file_metadata_t *meta)
{
    if (!dir || !name || !meta)
    {
        return -EINVAL;
    }

    // 检查是否已存在
    if (find_directory_entry(dir, name))
    {
        return -EEXIST;
    }

    pthread_rwlock_wrlock(&dir->lock);

    dir_entry_t *entry = malloc(sizeof(dir_entry_t));
    if (!entry)
    {
        pthread_rwlock_unlock(&dir->lock);
        return -ENOMEM;
    }

    entry->name = strdup(name);
    entry->meta = meta;
    entry->next = dir->entries;
    dir->entries = entry;
    dir->entry_count++;

    // 更新目录大小
    dir->meta.size += strlen(name) + sizeof(dir_entry_t);
    dir->meta.blocks = (dir->meta.size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

    // 更新修改时间
    clock_gettime(CLOCK_REALTIME, &dir->meta.mtime);
    dir->meta.ctime = dir->meta.mtime;

    pthread_rwlock_unlock(&dir->lock);
    return 0;
}

// 移除目录项
int remove_directory_entry(directory_t *dir, const char *name)
{
    if (!dir || !name)
    {
        return -EINVAL;
    }

    pthread_rwlock_wrlock(&dir->lock);

    dir_entry_t **prev = &dir->entries;
    dir_entry_t *entry = dir->entries;

    while (entry)
    {
        if (strcmp(entry->name, name) == 0)
        {
            *prev = entry->next;
            dir->entry_count--;

            // 更新目录大小
            dir->meta.size -= strlen(name) + sizeof(dir_entry_t);
            dir->meta.blocks = (dir->meta.size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

            // 更新修改时间
            clock_gettime(CLOCK_REALTIME, &dir->meta.mtime);
            dir->meta.ctime = dir->meta.mtime;

            free(entry->name);
            free(entry);

            pthread_rwlock_unlock(&dir->lock);
            return 0;
        }

        prev = &entry->next;
        entry = entry->next;
    }

    pthread_rwlock_unlock(&dir->lock);
    return -ENOENT;
}

// 查找目录项
dir_entry_t *find_directory_entry(directory_t *dir, const char *name)
{
    if (!dir || !name)
    {
        return NULL;
    }

    dir_entry_t *entry = dir->entries;
    while (entry)
    {
        if (strcmp(entry->name, name) == 0)
        {
            return entry;
        }
        entry = entry->next;
    }

    return NULL;
}

// 分配数据块
data_block_t *allocate_block(size_t size)
{
    data_block_t *block = malloc(sizeof(data_block_t));
    if (!block)
    {
        return NULL;
    }

    block->data = malloc(size);
    if (!block->data)
    {
        free(block);
        return NULL;
    }

    block->block_id = fs_state.total_blocks++;
    block->file_ino = 0;
    block->offset = 0;
    block->size = size;
    block->checksum = 0;
    block->next = NULL;

    fs_state.used_blocks++;

    return block;
}

// 释放数据块
void free_block(data_block_t *block)
{
    if (!block)
        return;

    if (block->data)
    {
        free(block->data);
    }

    fs_state.used_blocks--;
    free(block);
}

// 读取数据块
int read_block(data_block_t *block, char *buf, size_t size, off_t offset)
{
    if (!block || !buf)
    {
        return -EINVAL;
    }

    if (offset < 0 || (size_t)offset >= block->size)
    {
        return 0; // EOF
    }

    size_t to_read = size;
    if (offset + to_read > block->size)
    {
        to_read = block->size - offset;
    }

    memcpy(buf, block->data + offset, to_read);
    return to_read;
}

// 写入数据块
int write_block(data_block_t *block, const char *buf, size_t size, off_t offset)
{
    if (!block || !buf)
    {
        return -EINVAL;
    }

    if (offset < 0 || (size_t)offset >= block->size)
    {
        return -ENOSPC;
    }

    size_t to_write = size;
    if (offset + to_write > block->size)
    {
        to_write = block->size - offset;
    }

    memcpy(block->data + offset, buf, to_write);

    // 更新校验和（简单实现）
    block->checksum = 0;
    for (size_t i = 0; i < block->size; i++)
    {
        block->checksum += block->data[i];
    }

    return to_write;
}

// 创建文件块映射
block_map_t *create_block_map(uint64_t file_ino)
{
    block_map_t *map = malloc(sizeof(block_map_t));
    if (!map)
        return NULL;

    map->file_ino = file_ino;
    map->block_count = 0;
    map->blocks = NULL;
    map->direct_blocks = 12; // 默认12个直接块
    map->indirect_blocks = 0;
    map->version_block_ids = NULL;
    map->version_block_capacity = 0;
    pthread_rwlock_init(&map->lock, NULL);

    return map;
}

// 销毁文件块映射
void destroy_block_map(block_map_t *map)
{
    if (!map)
        return;

    pthread_rwlock_wrlock(&map->lock);

    // 释放所有数据块
    for (uint64_t i = 0; i < map->block_count; i++)
    {
        if (map->blocks[i])
        {
            free_block(map->blocks[i]);
        }
    }

    free(map->blocks);
    pthread_rwlock_unlock(&map->lock);
    pthread_rwlock_destroy(&map->lock);
    free(map);
}

// 获取文件块映射
block_map_t *get_block_map(uint64_t file_ino)
{
    pthread_mutex_lock(&block_maps_mutex);
    block_map_t *map = hash_table_get(block_maps, file_ino);

    if (!map)
    {
        map = create_block_map(file_ino);
        if (map)
        {
            hash_table_set(block_maps, file_ino, map);
        }
    }

    pthread_mutex_unlock(&block_maps_mutex);
    return map;
}

// 高性能文件读取（支持大文件）
int smart_read_file(file_metadata_t *meta, char *buf, size_t size, off_t offset)
{
    if (!meta || !buf || !S_ISREG(meta->mode))
    {
        return -EINVAL;
    }

    if (offset < 0)
        return -EINVAL;
    if (offset >= meta->size)
        return 0;

    size_t remaining = meta->size - offset;
    if (size > remaining)
        size = remaining;

    block_map_t *map = get_block_map(meta->ino);
    if (!map)
        return -ENOMEM;

    pthread_rwlock_rdlock(&map->lock);

    size_t bytes_read = 0;
    size_t current_offset = offset;
    size_t remaining_bytes = size;

    while (remaining_bytes > 0)
    {
        uint64_t block_index = current_offset / fs_state.block_size;
        size_t block_offset = current_offset % fs_state.block_size;
        size_t bytes_to_read = fs_state.block_size - block_offset;

        if (bytes_to_read > remaining_bytes)
        {
            bytes_to_read = remaining_bytes;
        }

        if (block_index >= map->block_count)
        {
            // 读取空数据（稀疏文件支持）
            memset(buf + bytes_read, 0, bytes_to_read);
        }
        else
        {
            data_block_t *block = map->blocks[block_index];
            if (block)
            {
                int result = read_block(block, buf + bytes_read, bytes_to_read, block_offset);
                if (result < 0)
                {
                    pthread_rwlock_unlock(&map->lock);
                    return result;
                }
                bytes_read += result;
            }
            else
            {
                // 空块，填充零
                memset(buf + bytes_read, 0, bytes_to_read);
                bytes_read += bytes_to_read;
            }
        }

        current_offset += bytes_to_read;
        remaining_bytes -= bytes_to_read;
    }

    pthread_rwlock_unlock(&map->lock);

    // 更新访问时间
    clock_gettime(CLOCK_REALTIME, &meta->atime);

    return bytes_read;
}

// 高性能文件写入（支持大文件）
int smart_write_file(file_metadata_t *meta, const char *buf, size_t size, off_t offset)
{
    if (!meta || !buf || !S_ISREG(meta->mode))
    {
        return -EINVAL;
    }

    if (offset < 0)
        return -EINVAL;

    block_map_t *map = get_block_map(meta->ino);
    if (!map)
        return -ENOMEM;

    pthread_rwlock_wrlock(&map->lock);

    // 检查是否需要扩展文件
    off_t new_size = offset + size;
    if (new_size > meta->size)
    {
        meta->size = new_size;
    }

    size_t bytes_written = 0;
    size_t current_offset = offset;
    size_t remaining_bytes = size;

    while (remaining_bytes > 0)
    {
        uint64_t block_index = current_offset / fs_state.block_size;
        size_t block_offset = current_offset % fs_state.block_size;
        size_t bytes_to_write = fs_state.block_size - block_offset;

        if (bytes_to_write > remaining_bytes)
        {
            bytes_to_write = remaining_bytes;
        }

        // 扩展块映射数组
        if (block_index >= map->block_count)
        {
            size_t new_count = block_index + 1;
            data_block_t **new_blocks = realloc(map->blocks,
                                                new_count * sizeof(data_block_t *));
            if (!new_blocks)
            {
                pthread_rwlock_unlock(&map->lock);
                return -ENOMEM;
            }

            // 初始化新增的块指针
            for (size_t i = map->block_count; i < new_count; i++)
            {
                new_blocks[i] = NULL;
            }

            map->blocks = new_blocks;
            map->block_count = new_count;
        }

        // 分配数据块（如果需要）
        if (!map->blocks[block_index])
        {
            map->blocks[block_index] = allocate_block(fs_state.block_size);
            if (!map->blocks[block_index])
            {
                pthread_rwlock_unlock(&map->lock);
                return -ENOMEM;
            }
            map->blocks[block_index]->file_ino = meta->ino;
            map->blocks[block_index]->offset = block_index * fs_state.block_size;
        }

        // 写入数据块
        int result = write_block(map->blocks[block_index],
                                 buf + bytes_written, bytes_to_write, block_offset);
        if (result < 0)
        {
            pthread_rwlock_unlock(&map->lock);
            return result;
        }

        bytes_written += result;
        current_offset += result;
        remaining_bytes -= result;
    }

    // 更新文件块数
    meta->blocks = (meta->size + fs_state.block_size - 1) / fs_state.block_size;

    pthread_rwlock_unlock(&map->lock);

    // 更新修改时间
    clock_gettime(CLOCK_REALTIME, &meta->mtime);

    return bytes_written;
}

// 缓存操作
void cache_set(uint64_t key, void *value)
{
    pthread_once(&cache_once, init_caches);

    // 根据key范围判断缓存类型
    if (key < 0x100000000ULL)
    { // inode缓存
        lru_cache_put(inode_cache, key, value);
    }
    else
    { // 数据块缓存
        lru_cache_put(block_cache, key, value);
    }
}

void *cache_get(uint64_t key)
{
    pthread_once(&cache_once, init_caches);

    if (key < 0x100000000ULL)
    {
        return lru_cache_get(inode_cache, key);
    }
    else
    {
        return lru_cache_get(block_cache, key);
    }
}

void cache_remove(uint64_t key)
{
    pthread_once(&cache_once, init_caches);

    if (key < 0x100000000ULL)
    {
        lru_cache_remove(inode_cache, key);
    }
    else
    {
        lru_cache_remove(block_cache, key);
    }
}

void cache_clear(void)
{
    pthread_once(&cache_once, init_caches);

    lru_cache_clear(inode_cache);
    lru_cache_clear(block_cache);
}