/**
 * 智能备份文件系统 - 主头文件
 */

#ifndef SMARTBACKUPFS_H
#define SMARTBACKUPFS_H

#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

// 配置常量
#define MAX_PATH_LEN 4096
#define DEFAULT_BLOCK_SIZE 4096
#define MAX_INODES 1000000
#define MAX_BLOCKS 10000000
#define MAX_CACHE_SIZE 128 * 1024 * 1024  // 128MB

// 文件类型
typedef enum {
    FT_REGULAR = 1,    // 普通文件
    FT_DIRECTORY,      // 目录
    FT_SYMLINK,        // 符号链接
    FT_VERSIONED,      // 版本文件
} file_type_t;

// 文件元数据
typedef struct {
    uint64_t ino;           // inode编号
    file_type_t type;       // 文件类型
    mode_t mode;            // 权限模式
    nlink_t nlink;          // 链接数
    uid_t uid;              // 用户ID
    gid_t gid;              // 组ID
    off_t size;             // 文件大小
    blkcnt_t blocks;        // 块数
    struct timespec atime;  // 访问时间
    struct timespec mtime;  // 修改时间
    struct timespec ctime;  // 状态改变时间
    uint32_t version;       // 版本号
    uint64_t parent_ino;    // 父目录inode
    char *xattr;           // 扩展属性
    size_t xattr_size;     // 扩展属性大小
} file_metadata_t;

// 文件数据块
typedef struct {
    uint64_t block_id;
    uint64_t file_ino;
    uint64_t offset;
    size_t size;
    char *data;
    uint32_t checksum;
    struct data_block *next;
} data_block_t;

// 目录项
typedef struct dir_entry {
    char *name;
    file_metadata_t *meta;
    struct dir_entry *next;
} dir_entry_t;

// 目录结构
typedef struct {
    file_metadata_t meta;
    dir_entry_t *entries;
    pthread_rwlock_t lock;
    uint64_t entry_count;
} directory_t;

// 文件系统状态
typedef struct {
    directory_t *root;
    uint64_t next_ino;
    pthread_mutex_t ino_mutex;
    
    // 缓存系统
    struct hash_table *inode_cache;
    struct hash_table *block_cache;
    pthread_rwlock_t cache_lock;
    
    // 统计信息
    uint64_t total_files;
    uint64_t total_dirs;
    uint64_t total_blocks;
    uint64_t used_blocks;
    
    // 配置
    size_t block_size;
    size_t max_cache_size;
    bool enable_compression;
    bool enable_deduplication;
} fs_state_t;

// 全局文件系统状态
extern fs_state_t fs_state;

// 初始化函数
void fs_init(void);
void fs_destroy(void);

// 元数据管理
file_metadata_t *create_inode(file_type_t type, mode_t mode);
void free_inode(file_metadata_t *meta);
file_metadata_t *lookup_path(const char *path);
file_metadata_t *lookup_inode(uint64_t ino);

// 目录操作
int add_directory_entry(directory_t *dir, const char *name, file_metadata_t *meta);
int remove_directory_entry(directory_t *dir, const char *name);
dir_entry_t *find_directory_entry(directory_t *dir, const char *name);

// 数据块操作
data_block_t *allocate_block(size_t size);
void free_block(data_block_t *block);
int read_block(data_block_t *block, char *buf, size_t size, off_t offset);
int write_block(data_block_t *block, const char *buf, size_t size, off_t offset);

// 缓存管理
void cache_set(uint64_t key, void *value);
void *cache_get(uint64_t key);
void cache_remove(uint64_t key);
void cache_clear(void);

#endif // SMARTBACKUPFS_H