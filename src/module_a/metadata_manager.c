/**
 * 元数据管理模块
 */

#include "smartbackupfs.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>

// 全局文件系统状态
fs_state_t fs_state;

// 初始化文件系统
void fs_init(void) {
    memset(&fs_state, 0, sizeof(fs_state_t));
    
    // 初始化锁
    pthread_mutex_init(&fs_state.ino_mutex, NULL);
    pthread_rwlock_init(&fs_state.cache_lock, NULL);
    
    // 设置配置
    fs_state.block_size = DEFAULT_BLOCK_SIZE;
    fs_state.max_cache_size = MAX_CACHE_SIZE;
    fs_state.enable_compression = false;  // 初始关闭
    fs_state.enable_deduplication = false; // 初始关闭
    
    // 创建根目录
    fs_state.root = calloc(1, sizeof(directory_t));
    if (!fs_state.root) {
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
    
    // 初始化缓存
    // TODO: 实现哈希表缓存
    
    fs_state.next_ino = 2;
    fs_state.total_dirs = 1;
    fs_state.total_files = 0;
    fs_state.total_blocks = 0;
    fs_state.used_blocks = 0;
}

// 销毁文件系统
void fs_destroy(void) {
    // 清理根目录
    if (fs_state.root) {
        pthread_rwlock_wrlock(&fs_state.root->lock);
        
        dir_entry_t *entry = fs_state.root->entries;
        while (entry) {
            dir_entry_t *next = entry->next;
            free(entry->name);
            
            // 清理文件数据
            if (entry->meta->type == FT_REGULAR) {
                // TODO: 清理数据块
            }
            
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
    
    // 销毁锁
    pthread_mutex_destroy(&fs_state.ino_mutex);
    pthread_rwlock_destroy(&fs_state.cache_lock);
}

// 创建新的inode
file_metadata_t *create_inode(file_type_t type, mode_t mode) {
    pthread_mutex_lock(&fs_state.ino_mutex);
    
    file_metadata_t *meta = calloc(1, sizeof(file_metadata_t));
    if (!meta) {
        pthread_mutex_unlock(&fs_state.ino_mutex);
        return NULL;
    }
    
    meta->ino = fs_state.next_ino++;
    meta->type = type;
    
    switch (type) {
        case FT_DIRECTORY:
            meta->mode = S_IFDIR | (mode & 07777);
            meta->nlink = 2;  // '.' 和父目录
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
void free_inode(file_metadata_t *meta) {
    if (!meta) return;
    
    // 从缓存移除
    cache_remove(meta->ino);
    
    // 清理扩展属性
    if (meta->xattr) {
        free(meta->xattr);
    }
    
    free(meta);
}

// 根据路径查找inode
file_metadata_t *lookup_path(const char *path) {
    if (!path || path[0] != '/') {
        return NULL;
    }
    
    if (strcmp(path, "/") == 0) {
        return &fs_state.root->meta;
    }
    
    // 简化路径解析（实际需要更复杂的实现）
    char *path_copy = strdup(path);
    if (!path_copy) {
        return NULL;
    }
    
    directory_t *current_dir = fs_state.root;
    file_metadata_t *result = NULL;
    
    char *token = strtok(path_copy + 1, "/");
    while (token) {
        pthread_rwlock_rdlock(&current_dir->lock);
        
        dir_entry_t *entry = find_directory_entry(current_dir, token);
        if (!entry) {
            pthread_rwlock_unlock(&current_dir->lock);
            break;
        }
        
        char *next_token = strtok(NULL, "/");
        if (!next_token) {
            // 找到目标
            result = entry->meta;
            pthread_rwlock_unlock(&current_dir->lock);
            break;
        }
        
        if (entry->meta->type != FT_DIRECTORY) {
            pthread_rwlock_unlock(&current_dir->lock);
            break;
        }
        
        current_dir = (directory_t *)entry->meta;
        token = next_token;
        pthread_rwlock_unlock(&current_dir->lock);
    }
    
    free(path_copy);
    return result;
}

// 根据inode编号查找
file_metadata_t *lookup_inode(uint64_t ino) {
    return (file_metadata_t *)cache_get(ino);
}

// 添加目录项
int add_directory_entry(directory_t *dir, const char *name, file_metadata_t *meta) {
    if (!dir || !name || !meta) {
        return -EINVAL;
    }
    
    // 检查是否已存在
    if (find_directory_entry(dir, name)) {
        return -EEXIST;
    }
    
    pthread_rwlock_wrlock(&dir->lock);
    
    dir_entry_t *entry = malloc(sizeof(dir_entry_t));
    if (!entry) {
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
int remove_directory_entry(directory_t *dir, const char *name) {
    if (!dir || !name) {
        return -EINVAL;
    }
    
    pthread_rwlock_wrlock(&dir->lock);
    
    dir_entry_t **prev = &dir->entries;
    dir_entry_t *entry = dir->entries;
    
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
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
dir_entry_t *find_directory_entry(directory_t *dir, const char *name) {
    if (!dir || !name) {
        return NULL;
    }
    
    dir_entry_t *entry = dir->entries;
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// 分配数据块
data_block_t *allocate_block(size_t size) {
    data_block_t *block = malloc(sizeof(data_block_t));
    if (!block) {
        return NULL;
    }
    
    block->data = malloc(size);
    if (!block->data) {
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
void free_block(data_block_t *block) {
    if (!block) return;
    
    if (block->data) {
        free(block->data);
    }
    
    fs_state.used_blocks--;
    free(block);
}

// 读取数据块
int read_block(data_block_t *block, char *buf, size_t size, off_t offset) {
    if (!block || !buf) {
        return -EINVAL;
    }
    
    if (offset < 0 || (size_t)offset >= block->size) {
        return 0;  // EOF
    }
    
    size_t to_read = size;
    if (offset + to_read > block->size) {
        to_read = block->size - offset;
    }
    
    memcpy(buf, block->data + offset, to_read);
    return to_read;
}

// 写入数据块
int write_block(data_block_t *block, const char *buf, size_t size, off_t offset) {
    if (!block || !buf) {
        return -EINVAL;
    }
    
    if (offset < 0 || (size_t)offset >= block->size) {
        return -ENOSPC;
    }
    
    size_t to_write = size;
    if (offset + to_write > block->size) {
        to_write = block->size - offset;
    }
    
    memcpy(block->data + offset, buf, to_write);
    
    // 更新校验和（简单实现）
    block->checksum = 0;
    for (size_t i = 0; i < block->size; i++) {
        block->checksum += block->data[i];
    }
    
    return to_write;
}

// 缓存操作（简化实现）
void cache_set(uint64_t key, void *value) {
    // 简化实现，实际应该使用真正的哈希表
}

void *cache_get(uint64_t key) {
    // 简化实现，实际应该使用真正的哈希表
    return NULL;
}

void cache_remove(uint64_t key) {
    // 简化实现，实际应该使用真正的哈希表
}

void cache_clear(void) {
    // 简化实现，实际应该使用真正的哈希表
}