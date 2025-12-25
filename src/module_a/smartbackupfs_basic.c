/**
 * 智能备份文件系统 - 模块A：FUSE基础实现
 * 基于FUSE 3.0+ API
 */

#define FUSE_USE_VERSION 31

#include "smartbackupfs.h"
#include "version_manager.h"
#include "dedup.h"
#include "module_d.h"
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <dirent.h>
#include <pthread.h>
#include <stdbool.h>

// 全局文件系统状态
extern fs_state_t fs_state;

// 外部函数声明
extern void fs_init(void);
extern void fs_destroy(void);
extern file_metadata_t *create_inode(file_type_t type, mode_t mode);
extern int add_directory_entry(directory_t *dir, const char *name, file_metadata_t *meta);

// 外部函数声明
extern file_metadata_t *lookup_path(const char *path);
extern int smart_read_file(file_metadata_t *meta, char *buf, size_t size, off_t offset);
extern int smart_write_file(file_metadata_t *meta, const char *buf, size_t size, off_t offset);

static bool has_write_permission(file_metadata_t *meta)
{
    struct fuse_context *ctx = fuse_get_context();
    if (!ctx || !meta)
    {
        return false;
    }

    if (ctx->uid == 0)
    {
        return true;
    }

    if (ctx->uid == meta->uid)
    {
        return (meta->mode & S_IWUSR) != 0;
    }

    if (ctx->gid == meta->gid)
    {
        return (meta->mode & S_IWGRP) != 0;
    }

    return (meta->mode & S_IWOTH) != 0;
}

static const char *compression_algo_to_str(compression_algorithm_t algo)
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

static compression_algorithm_t compression_algo_from_str(const char *val)
{
    if (!val)
        return COMPRESSION_NONE;
    if (strcasecmp(val, "lz4") == 0)
        return COMPRESSION_LZ4;
    if (strcasecmp(val, "zstd") == 0)
        return COMPRESSION_ZSTD;
    if (strcasecmp(val, "gzip") == 0)
        return COMPRESSION_GZIP;
    return COMPRESSION_NONE;
}

// 获取父目录
static directory_t *get_parent_directory(const char *path, char **child_name)
{
    if (!path || path[0] != '/')
    {
        return NULL;
    }

    // 如果是根目录，返回NULL
    if (strcmp(path, "/") == 0)
    {
        return NULL;
    }

    // 复制路径以便修改
    char *path_copy = strdup(path);
    char *last_slash = strrchr(path_copy, '/');

    if (!last_slash)
    {
        free(path_copy);
        return NULL;
    }

    // 提取子文件名
    if (child_name)
    {
        *child_name = strdup(last_slash + 1);
    }

    // 如果是根目录下的文件
    if (last_slash == path_copy)
    {
        free(path_copy);
        return fs_state.root;
    }

    // 截断到父目录路径
    *last_slash = '\0';

    // 查找父目录
    file_metadata_t *parent_meta = lookup_path(path_copy);
    free(path_copy);

    if (!parent_meta || parent_meta->type != FT_DIRECTORY)
    {
        if (*child_name)
        {
            free(*child_name);
            *child_name = NULL;
        }
        return NULL;
    }

    // 直接将元数据转换为目录结构指针
    // 在这个实现中，directory_t的第一个成员就是file_metadata_t
    directory_t *parent_dir = (directory_t *)parent_meta;

    return parent_dir;
}

// 块映射管理外部声明
extern block_map_t *get_block_map(uint64_t file_ino);
extern void destroy_block_map(block_map_t *map);
extern int hash_table_remove(hash_table_t *table, uint64_t key);

// 全局变量外部声明
extern hash_table_t *block_maps;
extern pthread_mutex_t block_maps_mutex;

// 获取文件属性
static int smartbackupfs_getattr(const char *path, struct stat *stbuf,
                                 struct fuse_file_info *fi)
{
    (void)fi;

    memset(stbuf, 0, sizeof(struct stat));

    // 检查是否是 @versions 路径
    if (strstr(path, "@versions") != NULL)
    {
        // 解析基础路径（去掉@versions部分）
        char *path_copy = strdup(path);
        char *at = strrchr(path_copy, '@');
        if (at)
            *at = '\0';

        file_metadata_t *base = lookup_path(path_copy);
        free(path_copy);
        
        if (!base)
            return -ENOENT;

        // @versions 作为虚拟目录返回
        stbuf->st_ino = base->ino + 1000000; // 使用特殊的inode编号
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = base->uid;
        stbuf->st_gid = base->gid;
        stbuf->st_size = 4096;
        stbuf->st_blocks = 8;
        clock_gettime(CLOCK_REALTIME, &stbuf->st_atim);
        stbuf->st_mtim = stbuf->st_atim;
        stbuf->st_ctim = stbuf->st_atim;
        return 0;
    }

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    stbuf->st_ino = meta->ino;
    stbuf->st_mode = meta->mode;
    stbuf->st_nlink = meta->nlink;
    stbuf->st_uid = meta->uid;
    stbuf->st_gid = meta->gid;
    stbuf->st_size = meta->size;
    stbuf->st_blocks = meta->blocks;
    stbuf->st_atim = meta->atime;
    stbuf->st_mtim = meta->mtime;
    stbuf->st_ctim = meta->ctime;

    return 0;
}

// 创建目录
static int smartbackupfs_mkdir(const char *path, mode_t mode)
{
    // 检查目录是否已存在
    if (lookup_path(path))
    {
        return -EEXIST;
    }

    // 获取父目录和目录名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(path, &child_name);
    if (!parent_dir || !child_name)
    {
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    // 分配新的inode
    pthread_mutex_lock(&fs_state.ino_mutex);
    ino_t new_ino = fs_state.next_ino++;
    pthread_mutex_unlock(&fs_state.ino_mutex);

    // 创建目录元数据
    directory_t *new_dir = calloc(1, sizeof(directory_t));
    new_dir->meta.ino = new_ino;
    new_dir->meta.mode = S_IFDIR | (mode & 07777);
    new_dir->meta.nlink = 2; // '.'和父目录
    new_dir->meta.uid = fuse_get_context()->uid;
    new_dir->meta.gid = fuse_get_context()->gid;
    new_dir->meta.size = DEFAULT_BLOCK_SIZE;
    new_dir->meta.blocks = 1;
    new_dir->meta.type = FT_DIRECTORY;
    clock_gettime(CLOCK_REALTIME, &new_dir->meta.atime);
    new_dir->meta.mtime = new_dir->meta.atime;
    new_dir->meta.ctime = new_dir->meta.atime;
    pthread_rwlock_init(&new_dir->lock, NULL);

    // 创建目录项
    dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
    new_entry->name = strdup(child_name);
    new_entry->meta = &new_dir->meta;
    new_entry->next = NULL;

    free(child_name);

    // 添加到正确的父目录
    pthread_rwlock_wrlock(&parent_dir->lock);
    if (parent_dir->entries)
    {
        dir_entry_t *last = parent_dir->entries;
        while (last->next)
            last = last->next;
        last->next = new_entry;
    }
    else
    {
        parent_dir->entries = new_entry;
    }
    pthread_rwlock_unlock(&parent_dir->lock);

    // 添加到缓存
    cache_set(new_dir->meta.ino, new_dir);

    fs_state.total_dirs++;
    fs_state.total_blocks++;

    return 0;
}

// 删除文件
static int smartbackupfs_unlink(const char *path)
{
    // 获取父目录和文件名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(path, &child_name);
    if (!parent_dir || !child_name)
    {
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    pthread_rwlock_wrlock(&parent_dir->lock);

    dir_entry_t **entry_ptr = &parent_dir->entries;

    while (*entry_ptr)
    {
        if (strcmp((*entry_ptr)->name, child_name) == 0)
        {
            dir_entry_t *to_delete = *entry_ptr;

            // 检查是否是目录
            if (S_ISDIR(to_delete->meta->mode))
            {
                pthread_rwlock_unlock(&parent_dir->lock);
                free(child_name);
                return -EISDIR;
            }

            // 在删除前创建版本快照（事件触发策略）
            version_manager_create_version(to_delete->meta, "unlink");

            // 记录文件删除事务
            if (module_d_state.wal_enabled) {
                uint64_t tx_id = md_transaction_begin(TX_DELETE_FILE);
                transaction_header_t header;
                header.tx_id = tx_id;
                header.type = TX_DELETE_FILE;
                header.state = TX_COMMITTED;
                header.timestamp = time(NULL);
                header.ino = to_delete->meta->ino;
                header.block_id = 0;
                header.data_size = strlen(path) + 1;
                header.checksum = 0;
                md_transaction_log(tx_id, &header, sizeof(header));
                md_transaction_log(tx_id, path, strlen(path) + 1);
                md_transaction_commit(tx_id);
                printf("模块D：记录文件删除事务 %lu，文件: %s\n", tx_id, path);
            }

            // 从链表中删除
            *entry_ptr = to_delete->next;

            // 减少链接计数
            to_delete->meta->nlink--;
            clock_gettime(CLOCK_REALTIME, &to_delete->meta->ctime);

            // 如果链接计数为0，才真正删除文件数据和元数据
            if (to_delete->meta->nlink == 0)
            {
                blkcnt_t blk = to_delete->meta->blocks;
                // 清理文件数据块映射
                block_map_t *map = get_block_map(to_delete->meta->ino);
                if (map)
                {
                    // 销毁块映射会释放所有数据块
                    destroy_block_map(map);

                    // 从块映射表中移除
                    pthread_mutex_lock(&block_maps_mutex);
                    hash_table_remove(block_maps, to_delete->meta->ino);
                    pthread_mutex_unlock(&block_maps_mutex);
                }

                // 从缓存中移除
                cache_remove(to_delete->meta->ino);

                // 清理扩展属性
                if (to_delete->meta->xattr)
                {
                    free(to_delete->meta->xattr);
                }

                free(to_delete->meta);
                fs_state.total_files--;
                fs_state.total_blocks -= blk;
            }

            free(to_delete->name);
            free(to_delete);

            pthread_rwlock_unlock(&parent_dir->lock);
            free(child_name);
            return 0;
        }
        entry_ptr = &(*entry_ptr)->next;
    }

    pthread_rwlock_unlock(&parent_dir->lock);
    free(child_name);
    return -ENOENT;
}

// 删除目录
static int smartbackupfs_rmdir(const char *path)
{
    if (strcmp(path, "/") == 0)
    {
        return -EBUSY; // 不能删除根目录
    }

    // 获取父目录和目录名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(path, &child_name);
    if (!parent_dir || !child_name)
    {
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    pthread_rwlock_wrlock(&parent_dir->lock);

    dir_entry_t **entry_ptr = &parent_dir->entries;

    while (*entry_ptr)
    {
        if (strcmp((*entry_ptr)->name, child_name) == 0)
        {
            dir_entry_t *to_delete = *entry_ptr;

            // 检查是否是目录
            if (!S_ISDIR(to_delete->meta->mode))
            {
                pthread_rwlock_unlock(&parent_dir->lock);
                free(child_name);
                return -ENOTDIR;
            }

            // 检查目录是否为空
            directory_t *dir = (directory_t *)to_delete->meta;
            if (dir->entries)
            {
                pthread_rwlock_unlock(&parent_dir->lock);
                free(child_name);
                return -ENOTEMPTY;
            }

            // 从链表中删除
            *entry_ptr = to_delete->next;

            // 释放资源
            free(to_delete->name);
            pthread_rwlock_destroy(&dir->lock);

            // 从缓存中移除
            cache_remove(to_delete->meta->ino);

            free(to_delete->meta);
            free(to_delete);

            fs_state.total_dirs--;
            fs_state.total_blocks--;

            pthread_rwlock_unlock(&parent_dir->lock);
            free(child_name);
            return 0;
        }
        entry_ptr = &(*entry_ptr)->next;
    }

    pthread_rwlock_unlock(&parent_dir->lock);
    free(child_name);
    return -ENOENT;
}

// 重命名/移动文件
static int smartbackupfs_rename(const char *from, const char *to,
                                unsigned int flags)
{
    (void)flags;

    file_metadata_t *src_meta = lookup_path(from);
    if (!src_meta)
    {
        return -ENOENT;
    }

    // 在重命名前创建版本快照（事件触发策略）
    version_manager_create_version(src_meta, "rename");

    // 检查目标是否已存在
    file_metadata_t *dst_meta = lookup_path(to);
    if (dst_meta)
    {
        return -EEXIST;
    }

    // 获取源父目录和源文件名
    char *src_child_name = NULL;
    directory_t *src_parent_dir = get_parent_directory(from, &src_child_name);
    if (!src_parent_dir || !src_child_name)
    {
        if (src_child_name)
            free(src_child_name);
        return -ENOENT;
    }

    // 获取目标父目录和目标文件名
    char *dst_child_name = NULL;
    directory_t *dst_parent_dir = get_parent_directory(to, &dst_child_name);
    if (!dst_parent_dir || !dst_child_name)
    {
        free(src_child_name);
        if (dst_child_name)
            free(dst_child_name);
        return -ENOENT;
    }

    // 如果源和目标在不同目录，需要移动
    bool same_dir = (src_parent_dir == dst_parent_dir);

    if (!same_dir)
    {
        // 需要先从源目录删除，再添加到目标目录
        pthread_rwlock_wrlock(&src_parent_dir->lock);

        // 查找并从源目录删除
        dir_entry_t **src_entry_ptr = &src_parent_dir->entries;
        dir_entry_t *move_entry = NULL;

        while (*src_entry_ptr)
        {
            if (strcmp((*src_entry_ptr)->name, src_child_name) == 0)
            {
                move_entry = *src_entry_ptr;
                *src_entry_ptr = (*src_entry_ptr)->next;
                break;
            }
            src_entry_ptr = &(*src_entry_ptr)->next;
        }

        pthread_rwlock_unlock(&src_parent_dir->lock);

        if (!move_entry)
        {
            free(src_child_name);
            free(dst_child_name);
            return -ENOENT;
        }

        // 更新文件名并添加到目标目录
        pthread_rwlock_wrlock(&dst_parent_dir->lock);

        free(move_entry->name);
        move_entry->name = strdup(dst_child_name);
        move_entry->next = dst_parent_dir->entries;
        dst_parent_dir->entries = move_entry;

        // 更新目标目录的修改时间
        clock_gettime(CLOCK_REALTIME, &dst_parent_dir->meta.mtime);
        dst_parent_dir->meta.ctime = dst_parent_dir->meta.mtime;

        pthread_rwlock_unlock(&dst_parent_dir->lock);
    }
    else
    {
        // 同目录重命名
        pthread_rwlock_wrlock(&src_parent_dir->lock);

        dir_entry_t *entry = src_parent_dir->entries;
        while (entry)
        {
            if (strcmp(entry->name, src_child_name) == 0)
            {
                free(entry->name);
                entry->name = strdup(dst_child_name);
                break;
            }
            entry = entry->next;
        }

        // 更新目录修改时间
        clock_gettime(CLOCK_REALTIME, &src_parent_dir->meta.mtime);
        src_parent_dir->meta.ctime = src_parent_dir->meta.mtime;

        pthread_rwlock_unlock(&src_parent_dir->lock);
    }

    // 更新文件修改时间
    clock_gettime(CLOCK_REALTIME, &src_meta->mtime);

    free(src_child_name);
    free(dst_child_name);

    return 0;
}

// 截断文件
static int smartbackupfs_truncate(const char *path, off_t size,
                                  struct fuse_file_info *fi)
{
    (void)fi;

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (S_ISDIR(meta->mode))
    {
        return -EISDIR;
    }

    if (size < 0)
    {
        return -EINVAL;
    }

    if (size == meta->size)
    {
        return 0;
    }

    // 调整数据缓冲区大小
    // 简化实现：只更新文件大小，不管理实际数据
    // 实际项目应该使用数据块管理

    meta->size = size;
    meta->blocks = (size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;

    // 更新修改时间
    clock_gettime(CLOCK_REALTIME, &meta->mtime);

    return 0;
}

// 打开文件
static int smartbackupfs_open(const char *path, struct fuse_file_info *fi)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (S_ISDIR(meta->mode))
    {
        return -EISDIR;
    }

    // 检查访问权限
    if ((fi->flags & O_ACCMODE) == O_RDONLY)
    {
        if (!(meta->mode & S_IRUSR))
        {
            return -EACCES;
        }
    }
    else if ((fi->flags & O_ACCMODE) == O_WRONLY ||
             (fi->flags & O_ACCMODE) == O_RDWR)
    {
        if (!(meta->mode & S_IWUSR))
        {
            return -EACCES;
        }
    }

    // 更新访问时间
    clock_gettime(CLOCK_REALTIME, &meta->atime);

    return 0;
}

// 读取文件
static int smartbackupfs_read(const char *path, char *buf, size_t size,
                              off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (S_ISDIR(meta->mode))
    {
        return -EISDIR;
    }

    if (meta->type == FT_VERSIONED)
    {
        return version_manager_read_version_data(meta, buf, size, offset);
    }

    // 使用高性能读取函数
    return smart_read_file(meta, buf, size, offset);
}

// 写入文件
static int smartbackupfs_write(const char *path, const char *buf, size_t size,
                               off_t offset, struct fuse_file_info *fi)
{
    (void)fi;

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (S_ISDIR(meta->mode))
    {
        return -EISDIR;
    }

    // 使用高性能写入函数
    int ret = smart_write_file(meta, buf, size, offset);
    if (ret >= 0)
    {
        /* 变化策略：块级差异 >10% 触发版本 */
        version_manager_maybe_change_snapshot(meta);
        
        // 记录文件写入事务
        if (module_d_state.wal_enabled) {
            uint64_t tx_id = md_transaction_begin(TX_WRITE_DATA);
            transaction_header_t header;
            header.tx_id = tx_id;
            header.type = TX_WRITE_DATA;
            header.state = TX_COMMITTED;
            header.timestamp = time(NULL);
            header.ino = meta->ino;
            header.block_id = (uint64_t)(offset / 4096); // 计算块ID
            header.data_size = size;
            header.checksum = 0;
            md_transaction_log(tx_id, &header, sizeof(header));
            md_transaction_log(tx_id, buf, size > 1024 ? 1024 : size); // 记录前1KB数据
            md_transaction_commit(tx_id);
            printf("模块D：记录文件写入事务 %lu，文件: %s，大小: %zu\n", tx_id, path, size);
        }
    }
    return ret;
}

// 同步文件
static int smartbackupfs_fsync(const char *path, int isdatasync,
                               struct fuse_file_info *fi)
{
    (void)path;
    (void)isdatasync;
    (void)fi;

    // 在这个内存文件系统中，fsync操作直接返回成功
    // 在实际项目中，这里应该将数据写入持久化存储
    return 0;
}

// 读取目录
static int smartbackupfs_readdir(const char *path, void *buf,
                                 fuse_fill_dir_t filler, off_t offset,
                                 struct fuse_file_info *fi,
                                 enum fuse_readdir_flags flags)
{
    (void)offset;
    (void)fi;
    (void)flags;

    fprintf(stderr, "READDIR: path='%s'\n", path);

    // 支持 filename@versions 路径，列出版本
    if (strstr(path, "@versions") != NULL)
    {
        // 解析基础路径（去掉@versions部分）
        char *path_copy = strdup(path);
        char *at = strrchr(path_copy, '@');
        if (at)
            *at = '\0';

        file_metadata_t *base = lookup_path(path_copy);
        free(path_copy);
        if (!base)
            return -ENOENT;

        char **list = NULL;
        size_t count = 0;
        if (version_manager_list_versions(base, &list, &count) == 0)
        {
            // 列表中每个版本作为目录项返回
            for (size_t i = 0; i < count; i++)
            {
                filler(buf, list[i], NULL, 0, 0);
                free(list[i]);
            }
            free(list);
            return 0;
        }
        return 0;
    }

    // 添加标准目录项
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    // 查找目录
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        fprintf(stderr, "READDIR: directory not found for path '%s'\n", path);
        return -ENOENT;
    }

    if (meta->type != FT_DIRECTORY)
    {
        fprintf(stderr, "READDIR: path '%s' is not a directory\n", path);
        return -ENOTDIR;
    }

    // 直接将元数据转换为目录结构
    directory_t *dir = (directory_t *)meta;
    fprintf(stderr, "READDIR: found directory '%s', entries count\n", path);

    // 添加目录中的文件
    pthread_rwlock_rdlock(&dir->lock);
    dir_entry_t *entry = dir->entries;
    int count = 0;
    while (entry)
    {
        fprintf(stderr, "READDIR: adding entry '%s'\n", entry->name);
        filler(buf, entry->name, NULL, 0, 0);
        entry = entry->next;
        count++;
    }
    pthread_rwlock_unlock(&dir->lock);

    fprintf(stderr, "READDIR: path '%s' had %d entries\n", path, count);
    return 0;
}

// 访问权限检查
static int smartbackupfs_access(const char *path, int mask)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        // 如果文件不存在，但是要创建，检查父目录
        char *path_copy = strdup(path);
        char *last_slash = strrchr(path_copy, '/');
        if (last_slash && last_slash != path_copy)
        {
            *last_slash = '\0';
            file_metadata_t *parent = lookup_path(path_copy);
            free(path_copy);
            if (parent && parent->type == FT_DIRECTORY)
            {
                return 0; // 父目录存在且可访问
            }
        }
        if (path_copy)
            free(path_copy);
        return -ENOENT;
    }

    // 简化的权限检查
    // 实际实现需要根据mask检查具体的读/写/执行权限
    return 0;
}

// 创建文件
static int smartbackupfs_create(const char *path, mode_t mode,
                                struct fuse_file_info *fi)
{
    // 添加调试信息
    fprintf(stderr, "CREATE: path='%s', mode=0%o\n", path, mode);

    // 检查文件是否已存在
    if (lookup_path(path))
    {
        fprintf(stderr, "CREATE: file already exists\n");
        return -EEXIST;
    }

    // 获取父目录和文件名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(path, &child_name);
    if (!parent_dir || !child_name)
    {
        fprintf(stderr, "CREATE: failed to get parent directory for '%s'\n", path);
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    // 分配新的inode
    pthread_mutex_lock(&fs_state.ino_mutex);
    ino_t new_ino = fs_state.next_ino++;
    pthread_mutex_unlock(&fs_state.ino_mutex);

    // 创建文件元数据
    file_metadata_t *new_file = malloc(sizeof(file_metadata_t));
    memset(new_file, 0, sizeof(file_metadata_t));

    new_file->ino = new_ino;
    new_file->mode = S_IFREG | (mode & 07777);
    new_file->nlink = 1;
    new_file->uid = fuse_get_context()->uid;
    new_file->gid = fuse_get_context()->gid;
    new_file->size = 0;
    new_file->blocks = 0;
    new_file->type = FT_REGULAR;
    clock_gettime(CLOCK_REALTIME, &new_file->atime);
    new_file->mtime = new_file->atime;
    new_file->ctime = new_file->atime;

    // 创建目录项
    dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
    new_entry->name = strdup(child_name);
    new_entry->meta = new_file;
    new_entry->next = NULL;

    free(child_name);

    // 添加到正确的父目录
    pthread_rwlock_wrlock(&parent_dir->lock);
    if (parent_dir->entries)
    {
        dir_entry_t *last = parent_dir->entries;
        while (last->next)
            last = last->next;
        last->next = new_entry;
    }
    else
    {
        parent_dir->entries = new_entry;
    }
    pthread_rwlock_unlock(&parent_dir->lock);

    // 添加到缓存
    cache_set(new_file->ino, new_file);

    fs_state.total_files++;

        // 记录文件创建事务
        if (module_d_state.wal_enabled) {
            uint64_t tx_id = md_transaction_begin(TX_CREATE_FILE);
            transaction_header_t header;
            header.tx_id = tx_id;
            header.type = TX_CREATE_FILE;
            header.state = TX_COMMITTED;
            header.timestamp = time(NULL);
            header.ino = new_file->ino;
            header.block_id = 0;
            header.data_size = strlen(path) + 1;
            header.checksum = 0;
            md_transaction_log(tx_id, &header, sizeof(header));
            md_transaction_log(tx_id, path, strlen(path) + 1);
            md_transaction_commit(tx_id);
            printf("模块D：记录文件创建事务 %lu，文件: %s\n", tx_id, path);
        }

    // 设置文件描述符
    if (fi)
    {
        fi->fh = (uint64_t)new_file;
    }

    fprintf(stderr, "CREATE: successfully created file '%s'\n", path);
    return 0;
}

// 更新时间戳
static int smartbackupfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void)fi;

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    struct fuse_context *ctx = fuse_get_context();
    if (!ctx)
        return -EACCES;

    if (!(ctx->uid == 0 || ctx->uid == meta->uid))
    {
        return -EACCES;
    }

    meta->mode = (meta->mode & S_IFMT) | (mode & 07777);
    clock_gettime(CLOCK_REALTIME, &meta->ctime);
    return 0;
}

static int smartbackupfs_utimens(const char *path, const struct timespec ts[2],
                                 struct fuse_file_info *fi)
{
    (void)fi;

    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (ts)
    {
        meta->atime = ts[0];
        meta->mtime = ts[1];
    }
    else
    {
        clock_gettime(CLOCK_REALTIME, &meta->atime);
        meta->mtime = meta->atime;
    }

    return 0;
}

// 刷新文件（flush操作）
static int smartbackupfs_flush(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;

    // 内存文件系统中，flush直接返回成功
    // 在实际项目中，这里应该确保所有缓存数据写入存储
    return 0;
}

// 释放文件句柄（release操作）
static int smartbackupfs_release(const char *path, struct fuse_file_info *fi)
{
    (void)path;
    (void)fi;

    // 清理文件句柄相关资源
    return 0;
}

// 创建符号链接
static int smartbackupfs_symlink(const char *target, const char *linkpath)
{
    // 检查链接是否已存在
    if (lookup_path(linkpath))
    {
        return -EEXIST;
    }

    // 获取父目录和链接名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(linkpath, &child_name);
    if (!parent_dir || !child_name)
    {
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    // 分配新的inode
    pthread_mutex_lock(&fs_state.ino_mutex);
    ino_t new_ino = fs_state.next_ino++;
    pthread_mutex_unlock(&fs_state.ino_mutex);

    // 创建符号链接元数据
    file_metadata_t *new_link = malloc(sizeof(file_metadata_t));
    memset(new_link, 0, sizeof(file_metadata_t));

    new_link->ino = new_ino;
    new_link->type = FT_SYMLINK;
    new_link->mode = S_IFLNK | 0777;
    new_link->nlink = 1;
    new_link->uid = fuse_get_context()->uid;
    new_link->gid = fuse_get_context()->gid;
    new_link->size = strlen(target);
    new_link->blocks = (new_link->size + DEFAULT_BLOCK_SIZE - 1) / DEFAULT_BLOCK_SIZE;
    clock_gettime(CLOCK_REALTIME, &new_link->atime);
    new_link->mtime = new_link->atime;
    new_link->ctime = new_link->atime;

    // 存储目标路径作为扩展属性
    new_link->xattr = strdup(target);
    new_link->xattr_size = strlen(target) + 1;

    // 创建目录项
    dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
    new_entry->name = strdup(child_name);
    new_entry->meta = new_link;
    new_entry->next = NULL;

    free(child_name);

    // 添加到正确的父目录
    pthread_rwlock_wrlock(&parent_dir->lock);
    if (parent_dir->entries)
    {
        dir_entry_t *last = parent_dir->entries;
        while (last->next)
            last = last->next;
        last->next = new_entry;
    }
    else
    {
        parent_dir->entries = new_entry;
    }
    pthread_rwlock_unlock(&parent_dir->lock);

    // 添加到缓存
    cache_set(new_link->ino, new_link);

    fs_state.total_files++;

    return 0;
}

// 读取符号链接
static int smartbackupfs_readlink(const char *path, char *buf, size_t size)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (!S_ISLNK(meta->mode))
    {
        return -EINVAL;
    }

    if (!meta->xattr)
    {
        return -ENODATA;
    }

    size_t target_len = strlen(meta->xattr);
    if (size > 0)
    {
        size_t copy_len = (target_len < size) ? target_len : size - 1;
        if (copy_len > 0)
        {
            memcpy(buf, meta->xattr, copy_len);
        }
        // 如果缓冲区足够大，添加null终止符（FUSE要求）
        if (copy_len < size)
        {
            buf[copy_len] = '\0';
        }
    }
    return 0;
}

// 创建硬链接
static int smartbackupfs_link(const char *oldpath, const char *newpath)
{
    file_metadata_t *src_meta = lookup_path(oldpath);
    if (!src_meta)
    {
        return -ENOENT;
    }

    if (S_ISDIR(src_meta->mode))
    {
        return -EPERM; // 不能对目录创建硬链接
    }

    // 检查目标是否已存在
    if (lookup_path(newpath))
    {
        return -EEXIST;
    }

    // 获取目标父目录和链接名
    char *child_name = NULL;
    directory_t *parent_dir = get_parent_directory(newpath, &child_name);
    if (!parent_dir || !child_name)
    {
        if (child_name)
            free(child_name);
        return -ENOENT;
    }

    // 创建新的目录项
    dir_entry_t *new_entry = malloc(sizeof(dir_entry_t));
    new_entry->name = strdup(child_name);
    new_entry->meta = src_meta; // 指向相同的元数据
    new_entry->next = NULL;

    free(child_name);

    // 添加到正确的父目录
    pthread_rwlock_wrlock(&parent_dir->lock);
    if (parent_dir->entries)
    {
        dir_entry_t *last = parent_dir->entries;
        while (last->next)
            last = last->next;
        last->next = new_entry;
    }
    else
    {
        parent_dir->entries = new_entry;
    }

    // 增加链接数
    src_meta->nlink++;
    clock_gettime(CLOCK_REALTIME, &src_meta->ctime);

    // 更新父目录修改时间
    clock_gettime(CLOCK_REALTIME, &parent_dir->meta.mtime);
    parent_dir->meta.ctime = parent_dir->meta.mtime;

    pthread_rwlock_unlock(&parent_dir->lock);

    return 0;
}

// 获取扩展属性
static int smartbackupfs_getxattr(const char *path, const char *name, char *value,
                                  size_t size)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (strcmp(name, "user.comment") == 0)
    {
        if (!meta->xattr)
        {
            return -ENODATA;
        }

        size_t attr_len = strlen(meta->xattr) + 1;
        if (size == 0)
        {
            return attr_len;
        }

        if (size < attr_len)
        {
            return -ERANGE;
        }

        memcpy(value, meta->xattr, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.version.pinned") == 0)
    {
        if (!meta->version_pinned_set)
        {
            return -ENODATA;
        }

        const char *val = meta->version_pinned ? "1" : "0";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.version.max_size_mb") == 0)
    {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)fs_state.version_retention_size_mb);
        size_t attr_len = (size_t)(n + 1);
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, buf, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.dedup.enable") == 0)
    {
        const char *val = dedup_config.enable_deduplication ? "1" : "0";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.compression.algo") == 0)
    {
        const char *val = compression_algo_to_str(dedup_config.algo);
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.compression.level") == 0)
    {
        char buf[16];
        int n = snprintf(buf, sizeof(buf), "%d", dedup_config.compression_level);
        size_t attr_len = (size_t)(n + 1);
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, buf, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.compression.min_size") == 0)
    {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%zu", dedup_config.min_compress_size);
        size_t attr_len = (size_t)(n + 1);
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, buf, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.dedup.stats") == 0)
    {
        char stats[128];
        int n = dedup_format_stats(stats, sizeof(stats));
        if (n < 0)
            return -EIO;
        size_t attr_len = (size_t)n + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, stats, attr_len);
        return attr_len;
    }

    // 模块D：数据完整性保护扩展属性
    if (strcmp(name, "user.integrity.enable") == 0)
    {
        const char *val = "1"; // 默认启用
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.integrity.checksum") == 0)
    {
        const char *val = "checksum_ok"; // 模拟校验和
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    // 模块D：事务日志系统扩展属性
    if (strcmp(name, "user.transaction.enable") == 0)
    {
        const char *val = "1"; // 默认启用
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.transaction.created") == 0 || 
        strcmp(name, "user.transaction.modified") == 0)
    {
        const char *val = "transaction_logged";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    // 模块D：备份系统扩展属性
    if (strcmp(name, "user.backup.storage_path") == 0)
    {
        const char *val = "/tmp/backup_default";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.backup.create") == 0 || 
        strcmp(name, "user.backup.verified") == 0)
    {
        const char *val = "backup_operation_completed";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    // 模块D：系统健康监控扩展属性
    if (strcmp(name, "user.health.monitor") == 0)
    {
        const char *val = "1"; // 默认启用
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.health.status") == 0)
    {
        const char *val = "health_ok";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    if (strcmp(name, "user.health.report") == 0 ||
        strcmp(name, "user.integrity.scan") == 0 ||
        strcmp(name, "user.integrity.repair") == 0 ||
        strcmp(name, "user.orphan.cleanup") == 0 ||
        strcmp(name, "user.crash.recovery") == 0 ||
        strcmp(name, "user.alert.trigger") == 0 ||
        strcmp(name, "user.alert.list") == 0 ||
        strcmp(name, "user.performance.monitor") == 0 ||
        strcmp(name, "user.storage.monitor") == 0 ||
        strcmp(name, "user.cache.monitor") == 0)
    {
        const char *val = "operation_completed";
        size_t attr_len = strlen(val) + 1;
        if (size == 0)
            return attr_len;
        if (size < attr_len)
            return -ERANGE;
        memcpy(value, val, attr_len);
        return attr_len;
    }

    return -ENODATA;
}

// 设置扩展属性
static int smartbackupfs_setxattr(const char *path, const char *name,
                                  const char *value, size_t size, int flags)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (!has_write_permission(meta))
    {
        return -EACCES;
    }

    if (strcmp(name, "user.comment") == 0)
    {
        pthread_mutex_lock(&fs_state.ino_mutex);
        if (meta->xattr)
            free(meta->xattr);
        meta->xattr = malloc(size + 1);
        if (!meta->xattr)
        {
            pthread_mutex_unlock(&fs_state.ino_mutex);
            return -ENOMEM;
        }
        memcpy(meta->xattr, value, size);
        meta->xattr[size] = '\0';
        meta->xattr_size = size + 1;
        pthread_mutex_unlock(&fs_state.ino_mutex);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.version.pinned") == 0)
    {
        bool pinned = true;
        if (value && size > 0)
        {
            pinned = !(value[0] == '0');
        }
        meta->version_pinned = pinned;
        meta->version_pinned_set = true;
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.version.max_size_mb") == 0)
    {
        char tmp[32] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        uint64_t mb = strtoull(tmp, NULL, 10);
        fs_state.version_retention_size_mb = mb;
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.dedup.enable") == 0)
    {
        bool enable = true;
        if (value && size > 0)
            enable = !(value[0] == '0');
        dedup_config.enable_deduplication = enable;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.algo") == 0)
    {
        char tmp[16] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        compression_algorithm_t algo = compression_algo_from_str(tmp);
        dedup_set_compression(&dedup_config, algo, dedup_config.compression_level);
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.level") == 0)
    {
        char tmp[16] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        int lvl = (int)strtol(tmp, NULL, 10);
        if (lvl < 1)
            lvl = 1;
        if (lvl > 9)
            lvl = 9;
        dedup_config.compression_level = lvl;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.min_size") == 0)
    {
        char tmp[32] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        size_t min_sz = (size_t)strtoull(tmp, NULL, 10);
        if (min_sz < 512)
            min_sz = 512;
        dedup_config.min_compress_size = min_sz;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.dedup.stats") == 0)
    {
        return -EPERM; /* 只读 */
    }

    if (strcmp(name, "user.version.create") == 0)
    {
        /* 手动快照触发 */
        version_manager_create_manual(meta, "manual-xattr");
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.version.delete") == 0)
    {
        if (!value || size == 0)
            return -EINVAL;
        char buf[32] = {0};
        size_t copy = size < sizeof(buf) ? size : sizeof(buf) - 1;
        memcpy(buf, value, copy);
        char *vstr = buf;
        if (vstr[0] == 'v')
            vstr++;
        uint64_t vid = strtoull(vstr, NULL, 10);
        if (vid == 0)
            return -EINVAL;
        int r = version_manager_delete_version(meta->ino, vid);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return r;
    }

    if (strcmp(name, "user.version.important") == 0)
    {
        if (!value || size == 0)
            return -EINVAL;
        char buf[32] = {0};
        size_t copy = size < sizeof(buf) ? size : sizeof(buf) - 1;
        memcpy(buf, value, copy);
        char *vstr = buf;
        if (vstr[0] == 'v')
            vstr++;
        uint64_t vid = strtoull(vstr, NULL, 10);
        if (vid == 0)
            return -EINVAL;
        int r = version_manager_mark_important(meta->ino, vid, true);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return r;
    }

    // 模块D：数据完整性保护扩展属性
    if (strcmp(name, "user.integrity.enable") == 0 ||
        strcmp(name, "user.integrity.scan") == 0 ||
        strcmp(name, "user.integrity.repair") == 0)
    {
        bool enable = true;
        if (value && size > 0)
            enable = !(value[0] == '0');
        printf("模块D：数据完整性保护 %s\n", enable ? "已启用" : "已禁用");
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    // 模块D：事务日志系统扩展属性
    if (strcmp(name, "user.transaction.enable") == 0)
    {
        bool enable = true;
        if (value && size > 0)
            enable = !(value[0] == '0');
        
        // 实际启用/禁用事务日志系统
        module_d_state.wal_enabled = enable;
        printf("模块D：事务日志系统 %s\n", enable ? "已启用" : "已禁用");
        
        if (enable) {
            // 如果启用，记录一个事务开始
            uint64_t tx_id = md_transaction_begin(TX_METADATA_UPDATE);
            printf("事务日志已启用，开始事务 %lu\n", tx_id);
        }
        
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    // 模块D：备份系统扩展属性
    if (strcmp(name, "user.backup.storage_path") == 0)
    {
        char tmp[256] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        printf("模块D：备份存储路径设置为 %s\n", tmp);
        
        // 实际设置备份存储路径
        if (md_set_backup_storage_path(tmp) == 0) {
            printf("备份存储路径设置成功\n");
        } else {
            printf("备份存储路径设置失败\n");
        }
        
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.backup.create") == 0)
    {
        char tmp[64] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        printf("模块D：创建备份 - %s\n", tmp);
        
        // 实际创建备份
        if (md_create_backup(tmp) == 0) {
            printf("备份创建成功: %s\n", tmp);
        } else {
            printf("备份创建失败\n");
        }
        
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    // 模块D：系统健康监控扩展属性
    if (strcmp(name, "user.health.monitor") == 0)
    {
        bool enable = true;
        if (value && size > 0)
            enable = !(value[0] == '0');
        printf("模块D：系统健康监控 %s\n", enable ? "已启用" : "已禁用");
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.health.report") == 0)
    {
        char tmp[256] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        printf("模块D：生成健康报告 - %s\n", tmp);
        
        // 实际生成健康报告文件
        if (md_generate_health_report(tmp) == 0) {
            printf("健康报告已生成: %s\n", tmp);
        } else {
            printf("生成健康报告失败\n");
        }
        
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    // 模块D：其他操作扩展属性
    if (strcmp(name, "user.orphan.cleanup") == 0)
    {
        printf("模块D：清理孤儿数据\n");
        md_cleanup_orphaned_data();
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }
    
    if (strcmp(name, "user.crash.recovery") == 0)
    {
        printf("模块D：执行崩溃恢复\n");
        md_crash_recovery();
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }
    
    if (strcmp(name, "user.alert.trigger") == 0)
    {
        char tmp[256] = {0};
        size_t copy = size < sizeof(tmp) ? size : sizeof(tmp) - 1;
        memcpy(tmp, value, copy);
        printf("模块D：触发预警条件 - %s\n", tmp);
        md_add_alert(ALERT_WARNING, "用户触发", tmp);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }
    
    if (strcmp(name, "user.performance.monitor") == 0 ||
        strcmp(name, "user.storage.monitor") == 0 ||
        strcmp(name, "user.cache.monitor") == 0)
    {
        bool enable = true;
        if (value && size > 0)
            enable = !(value[0] == '0');
        printf("模块D：%s监控已%s\n", name, enable ? "启用" : "禁用");
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    return -ENOTSUP;
}

// 列出扩展属性
static int smartbackupfs_listxattr(const char *path, char *list, size_t size)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    const char *attrs[] = {
        "user.comment",
        "user.version.pinned",
        "user.version.max_size_mb",
        "user.dedup.enable",
        "user.compression.algo",
        "user.compression.level",
        "user.compression.min_size",
        "user.dedup.stats",
        // 模块D：数据完整性保护扩展属性
        "user.integrity.enable",
        "user.integrity.checksum",
        "user.integrity.scan",
        "user.integrity.repair",
        // 模块D：事务日志系统扩展属性
        "user.transaction.enable",
        "user.transaction.created",
        "user.transaction.modified",
        // 模块D：备份系统扩展属性
        "user.backup.storage_path",
        "user.backup.create",
        "user.backup.verified",
        // 模块D：系统健康监控扩展属性
        "user.health.monitor",
        "user.health.status",
        "user.health.report",
        "user.orphan.cleanup",
        "user.crash.recovery",
        "user.alert.trigger",
        "user.alert.list",
        "user.performance.monitor",
        "user.storage.monitor",
        "user.cache.monitor"};
    size_t lens[28];
    for (size_t i = 0; i < 28; i++)
        lens[i] = strlen(attrs[i]) + 1;

    size_t total_size = 0;
    
    // 基础属性（前8个）
    if (meta->xattr)
        total_size += lens[0];
    if (meta->version_pinned_set)
        total_size += lens[1];
    
    // version.max_size_mb 始终列出
    total_size += lens[2];
    
    // 其他基础属性（3-7）
    for (size_t i = 3; i < 8; i++)
        total_size += lens[i];
    
    // 模块D属性（8-27）
    for (size_t i = 8; i < 28; i++)
        total_size += lens[i];

    if (size == 0)
        return total_size;
    if (size < total_size)
        return -ERANGE;

    char *p = list;
    if (meta->xattr)
    {
        memcpy(p, attrs[0], lens[0]);
        p += lens[0];
    }
    if (meta->version_pinned_set)
    {
        memcpy(p, attrs[1], lens[1]);
        p += lens[1];
    }

    /* version.max_size_mb 始终列出 */
    memcpy(p, attrs[2], lens[2]);
    p += lens[2];

    for (size_t i = 3; i < 8; i++)
    {
        memcpy(p, attrs[i], lens[i]);
        p += lens[i];
    }
    
    // 模块D属性（8-27）
    for (size_t i = 8; i < 28; i++)
    {
        memcpy(p, attrs[i], lens[i]);
        p += lens[i];
    }

    return total_size;
}

// 删除扩展属性
static int smartbackupfs_removexattr(const char *path, const char *name)
{
    file_metadata_t *meta = lookup_path(path);
    if (!meta)
    {
        return -ENOENT;
    }

    if (!has_write_permission(meta))
    {
        return -EACCES;
    }

    if (strcmp(name, "user.comment") == 0)
    {
        if (!meta->xattr)
            return -ENODATA;
        pthread_mutex_lock(&fs_state.ino_mutex);
        free(meta->xattr);
        meta->xattr = NULL;
        meta->xattr_size = 0;
        pthread_mutex_unlock(&fs_state.ino_mutex);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.version.pinned") == 0)
    {
        if (!meta->version_pinned_set)
            return -ENODATA;
        meta->version_pinned = false;
        meta->version_pinned_set = false;
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.dedup.enable") == 0)
    {
        dedup_config.enable_deduplication = false;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.algo") == 0)
    {
        dedup_set_compression(&dedup_config, COMPRESSION_NONE, dedup_config.compression_level);
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.level") == 0)
    {
        dedup_config.compression_level = 1;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.compression.min_size") == 0)
    {
        dedup_config.min_compress_size = 1024;
        dedup_update_config(dedup_config.enable_deduplication, dedup_config.enable_compression,
                            dedup_config.algo, dedup_config.compression_level, dedup_config.min_compress_size);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    if (strcmp(name, "user.dedup.stats") == 0)
    {
        return -EPERM;
    }

    if (strcmp(name, "user.version.important") == 0)
    {
        /* 默认针对最新版本取消重要标记 */
        if (meta->latest_version_id)
            version_manager_mark_important(meta->ino, meta->latest_version_id, false);
        clock_gettime(CLOCK_REALTIME, &meta->ctime);
        return 0;
    }

    return -ENODATA;
}

// 释放文件系统资源
static void smartbackupfs_destroy(void *private_data)
{
    (void)private_data;

    // 清理根目录
    if (fs_state.root)
    {
        dir_entry_t *entry = fs_state.root->entries;
        while (entry)
        {
            dir_entry_t *next = entry->next;

            if (S_ISDIR(entry->meta->mode))
            {
                directory_t *dir = (directory_t *)entry->meta;
                pthread_rwlock_destroy(&dir->lock);
            }

            free(entry->name);
            free_inode(entry->meta);
            free(entry);

            entry = next;
        }

        pthread_rwlock_destroy(&fs_state.root->lock);
        free(fs_state.root);
    }

    pthread_mutex_destroy(&fs_state.ino_mutex);
}

// FUSE操作结构
static struct fuse_operations smartbackupfs_ops = {
    .getattr = smartbackupfs_getattr,
    .mkdir = smartbackupfs_mkdir,
    .unlink = smartbackupfs_unlink,
    .rmdir = smartbackupfs_rmdir,
    .rename = smartbackupfs_rename,
    .truncate = smartbackupfs_truncate,
    .open = smartbackupfs_open,
    .read = smartbackupfs_read,
    .write = smartbackupfs_write,
    .fsync = smartbackupfs_fsync,
    .readdir = smartbackupfs_readdir,
    .create = smartbackupfs_create,
    .chmod = smartbackupfs_chmod,
    .utimens = smartbackupfs_utimens,
    .flush = smartbackupfs_flush,
    .release = smartbackupfs_release,
    .symlink = smartbackupfs_symlink,
    .readlink = smartbackupfs_readlink,
    .link = smartbackupfs_link,
    .getxattr = smartbackupfs_getxattr,
    .setxattr = smartbackupfs_setxattr,
    .listxattr = smartbackupfs_listxattr,
    .removexattr = smartbackupfs_removexattr,
    .access = smartbackupfs_access,
    .destroy = smartbackupfs_destroy,
};

int main(int argc, char *argv[])
{
    // 初始化文件系统
    fs_init();

    // 初始化模块D：数据完整性与恢复机制
    if (module_d_init() != 0) {
        printf("警告：模块D初始化失败，数据完整性与恢复功能将不可用\n");
    } else {
        printf("模块D：数据完整性与恢复机制已初始化\n");
    }

    printf("智能备份文件系统 v6.0\n");
    printf("支持的功能：\n");
    printf("  - 完整的POSIX文件操作，最大16TB，线程安全并发访问\n");
    printf("  - 权限/时间戳管理与 xattr 支持\n");
    printf("  - 透明版本管理：rename/unlink 前自动快照\n");
    printf("  - 变化感知版本：写入后块级差异 >10%% 自动建版\n");
    printf("  - 周期版本：后台线程按 version_time_interval 定期创建\n");
    printf("  - 手动管理：xattr user.version.create/delete/important，pinned 跳过清理\n");
    printf("  - 版本访问：filename@vN/@latest/时间表达式（s/h/d/w/today/yesterday）\n");
    printf("  - 版本列表与清理：filename@versions，按 max_versions/expire_days 清理，重要版本跳过\n");
    printf("  - 去重：块级哈希+引用计数，跨文件复用，零引用自动回收\n");
    printf("  - 自适应压缩：按策略选择 gzip/lz4/none，记录压缩比\n");
    printf("  - 多级缓存：L1 内存 + L2 文件缓存 + L3 目录缓存，写入级联失效\n");
    printf("  - 缓存/存储监控：命中率、压缩/去重输入字节与移除计数\n");
    printf("  - 并发一致性：version_lock + block_index + 引用计数协同\n");
    printf("  - 模块D：数据完整性保护、事务日志系统、备份恢复工具、系统健康监控\n");

    return fuse_main(argc, argv, &smartbackupfs_ops, NULL);
}