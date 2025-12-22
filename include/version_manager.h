/**
 * 版本管理模块头文件（模块B）
 */

#ifndef VERSION_MANAGER_H
#define VERSION_MANAGER_H

#include "smartbackupfs.h"

typedef struct version_block_snapshot {
    size_t size;
    char *data;
} version_block_snapshot_t;

typedef struct version_node {
    uint64_t version_id;
    uint64_t parent_version;
    time_t create_time;
    uint64_t *diff_blocks; /* 动态数组，存储变更块索引 */
    size_t diff_count;
    uint32_t *block_checksums; /* 版本时记录的每个块校验和，用于后续差异计算 */
    size_t block_count;
    size_t file_size; /* 版本创建时的文件大小 */
    blkcnt_t blocks;  /* 版本创建时的块数 */
    bool is_important; /* 重要版本标记，清理时跳过 */
    version_block_snapshot_t *snapshots; /* 数据快照 */
    size_t snapshot_count;
    struct version_node *next;
} version_node_t;

typedef struct version_chain {
    uint64_t file_ino;
    version_node_t *head; /* 最新版本在head位置 */
    size_t count;
    pthread_rwlock_t lock;
} version_chain_t;

/* 初始化/销毁 */
int version_manager_init(void);
void version_manager_destroy(void);
int version_manager_start_cleaner(void);
void version_manager_stop_cleaner(void);

/* 定时策略触发：为指定文件创建周期版本（调用时机：后台线程） */
int version_manager_create_periodic(file_metadata_t *meta, const char *reason);

/* 写路径变化策略：当块级差异 >10% 时创建版本 */
int version_manager_maybe_change_snapshot(file_metadata_t *meta);

/* 手动快照：外部通过 xattr/CLI 触发 */
int version_manager_create_manual(file_metadata_t *meta, const char *reason);

/* 创建版本：在写入、重命名前/删除前调用 */
int version_manager_create_version(file_metadata_t *meta, const char *reason);

/* 获取某个文件的指定版本元数据（解析 v<num> / latest）
 * 返回新分配的 file_metadata_t*（由调用者通过 free_inode/相应接口释放）
 */
file_metadata_t *version_manager_get_version_meta(file_metadata_t *meta, const char *verstr);

/* 根据时间戳查找不晚于目标时间的最新版本ID，返回0表示不存在 */
uint64_t version_manager_get_version_by_time(uint64_t ino, time_t target_time);

/* 读取指定版本的快照数据 */
int version_manager_read_version_data(file_metadata_t *vmeta, char *buf, size_t size, off_t offset);

/* 删除指定版本 */
int version_manager_delete_version(uint64_t ino, uint64_t version_id);

/* 标记版本重要性 */
int version_manager_mark_important(uint64_t ino, uint64_t version_id, bool important);

/* 列出版本名称（v<ID>）
 * 返回动态数组，调用者负责 free 列表及其字符串
 */
int version_manager_list_versions(file_metadata_t *meta, char ***out_list, size_t *out_count);

/* 比较两个版本，输出简要差异描述（分配字符串由调用者释放） */
int version_manager_diff(file_metadata_t *meta, uint64_t v1, uint64_t v2, char **out_diff);

/* 时间表达式解析辅助：返回目标时间（秒）; 支持 2h / 1d / yesterday */
time_t version_manager_parse_time_expr(const char *expr);

#endif /* VERSION_MANAGER_H */
