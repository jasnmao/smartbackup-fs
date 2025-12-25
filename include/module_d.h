/**
 * 模块D：数据完整性与恢复机制
 * 功能要求：
 * 1. 数据完整性保护
 * 2. 事务日志系统
 * 3. 备份与恢复工具
 * 4. 系统健康监控
 */

#ifndef MODULE_D_H
#define MODULE_D_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include "smartbackupfs.h"
#include "module_c/module_d_adapter.h"

// 数据完整性保护相关定义
#define CHECKSUM_SIZE 4
#define MAX_INTEGRITY_SCAN_THREADS 4

// 校验和算法类型
typedef enum {
    CHECKSUM_CRC32 = 1,
    CHECKSUM_SHA256_PARTIAL,
    CHECKSUM_ADLER32
} checksum_type_t;

// 数据块完整性状态
typedef enum {
    BLOCK_INTEGRITY_OK = 0,
    BLOCK_INTEGRITY_CORRUPTED,
    BLOCK_INTEGRITY_REPAIRED,
    BLOCK_INTEGRITY_UNVERIFIED
} block_integrity_status_t;

// 事务日志相关定义
#define MAX_TRANSACTION_SIZE (1024 * 1024) // 1MB
#define WAL_SEGMENT_SIZE (16 * 1024 * 1024) // 16MB
#define MAX_WAL_SEGMENTS 32

// 事务类型
typedef enum {
    TX_CREATE_FILE = 1,
    TX_WRITE_DATA,
    TX_DELETE_FILE,
    TX_METADATA_UPDATE,
    TX_BLOCK_ALLOCATION,
    TX_BLOCK_FREE
} transaction_type_t;

// 事务状态
typedef enum {
    TX_PENDING = 0,
    TX_COMMITTED,
    TX_ROLLED_BACK,
    TX_ABORTED
} transaction_state_t;

// 事务日志条目
typedef struct transaction_entry {
    uint64_t tx_id;
    transaction_type_t type;
    transaction_state_t state;
    time_t timestamp;
    uint64_t ino;
    uint64_t block_id;
    size_t data_size;
    char *data; // 事务数据
    struct transaction_entry *next;
} transaction_entry_t;

// 事务头部结构
#pragma pack(push, 1)
typedef struct transaction_header {
    uint64_t tx_id;
    transaction_type_t type;
    transaction_state_t state;
    time_t timestamp;
    uint64_t ino;
    uint64_t block_id;
    size_t data_size;
    uint32_t checksum;
} transaction_header_t;
#pragma pack(pop)

// WAL段结构
typedef struct wal_segment {
    uint64_t segment_id;
    char *data;
    size_t size;
    size_t capacity;
    time_t created_time;
    bool active;
    bool file_written;
    struct wal_segment *next;
} wal_segment_t;

// 备份类型
typedef enum {
    BACKUP_FULL = 1,
    BACKUP_INCREMENTAL,
    BACKUP_DIFFERENTIAL
} backup_type_t;

// 备份状态
typedef enum {
    BACKUP_PENDING = 0,
    BACKUP_RUNNING,
    BACKUP_COMPLETED,
    BACKUP_FAILED,
    BACKUP_VERIFIED
} backup_state_t;

// 备份元数据
typedef struct backup_metadata {
    uint64_t backup_id;
    backup_type_t type;
    backup_state_t state;
    time_t start_time;
    time_t end_time;
    uint64_t total_size;
    uint64_t file_count;
    char *backup_path;
    char *checksum;
    struct backup_metadata *base_backup; // 增量备份的基础备份
    struct backup_metadata *next;
} backup_metadata_t;

// 恢复选项
typedef struct recovery_options {
    bool verify_integrity;
    bool preserve_metadata;
    bool overwrite_existing;
    uint64_t target_backup_id;
    char *target_path;
} recovery_options_t;

// 系统健康状态
typedef struct system_health {
    bool integrity_scan_running;
    bool backup_in_progress;
    bool recovery_in_progress;
    uint64_t corrupted_blocks;
    uint64_t repaired_blocks;
    uint64_t pending_transactions;
    double system_uptime;
    time_t last_health_check;
} system_health_t;

// 预警级别
typedef enum {
    ALERT_INFO = 0,
    ALERT_WARNING,
    ALERT_ERROR,
    ALERT_CRITICAL
} alert_level_t;

// 预警信息
typedef struct alert_info {
    alert_level_t level;
    time_t timestamp;
    char *message;
    char *component;
    bool acknowledged;
    struct alert_info *next;
} alert_info_t;

// 模块D主结构
typedef struct module_d_state {
    // 数据完整性保护
    checksum_type_t checksum_algorithm;
    bool enable_write_verification;
    pthread_t integrity_scanner_threads[MAX_INTEGRITY_SCAN_THREADS];
    bool integrity_scan_running;
    uint64_t total_blocks_scanned;
    uint64_t corrupted_blocks_found;
    
    // 事务日志系统
    wal_segment_t *wal_segments;
    wal_segment_t *current_wal_segment;
    uint64_t next_tx_id;
    pthread_mutex_t wal_mutex;
    bool wal_enabled;
    
    // 备份恢复系统
    backup_metadata_t *backup_list;
    char *backup_storage_path;
    pthread_t backup_thread;
    bool backup_in_progress;
    
    // 系统健康监控
    system_health_t health_status;
    alert_info_t *alerts;
    pthread_mutex_t alert_mutex;
    time_t last_health_report;
    
    // 统计信息
    uint64_t total_transactions;
    uint64_t successful_backups;
    uint64_t failed_backups;
    uint64_t successful_recoveries;
} module_d_state_t;

// 全局模块D状态
extern module_d_state_t module_d_state;

// ================================
// 数据完整性保护接口
// ================================

/**
 * 初始化数据完整性保护系统
 */
int md_integrity_init(void);

/**
 * 销毁数据完整性保护系统
 */
void md_integrity_destroy(void);

/**
 * 计算数据块的校验和
 */
uint32_t md_calculate_checksum(const void *data, size_t size);

/**
 * 验证数据块完整性
 */
int md_verify_block_integrity(data_block_t *block);

/**
 * 写入时验证数据完整性
 */
int md_write_with_verification(data_block_t *block, const void *data, size_t size);

/**
 * 启动后台完整性扫描
 */
int md_start_integrity_scan(void);

/**
 * 停止后台完整性扫描
 */
void md_stop_integrity_scan(void);

/**
 * 处理损坏的数据块
 */
int md_handle_corrupted_block(data_block_t *block);

// ================================
// 事务日志系统接口
// ================================

/**
 * 初始化事务日志系统
 */
int md_transaction_init(void);

/**
 * 销毁事务日志系统
 */
void md_transaction_destroy(void);

/**
 * 开始新事务
 */
uint64_t md_transaction_begin(transaction_type_t type);

/**
 * 提交事务
 */
int md_transaction_commit(uint64_t tx_id);

/**
 * 回滚事务
 */
int md_transaction_rollback(uint64_t tx_id);

/**
 * 写入事务日志
 */
int md_transaction_log(uint64_t tx_id, const void *data, size_t size);

/**
 * 崩溃恢复
 */
int md_crash_recovery(void);

/**
 * 清理已提交的事务日志
 */
int md_cleanup_committed_transactions(void);

// ================================
// 备份恢复工具接口
// ================================

/**
 * 初始化备份恢复系统
 */
int md_backup_init(const char *storage_path);

/**
 * 销毁备份恢复系统
 */
void md_backup_destroy(void);

/**
 * 设置备份存储路径
 */
int md_set_backup_storage_path(const char *storage_path);

/**
 * 创建备份
 */
int md_create_backup(const char *description);

/**
 * 创建完整备份
 */
uint64_t md_create_full_backup(const char *description);

/**
 * 创建增量备份
 */
uint64_t md_create_incremental_backup(uint64_t base_backup_id, const char *description);

/**
 * 恢复文件系统
 */
int md_restore_filesystem(uint64_t backup_id, const recovery_options_t *options);

/**
 * 恢复单个文件
 */
int md_restore_file(uint64_t backup_id, const char *file_path, const char *target_path);

/**
 * 恢复目录
 */
int md_restore_directory(uint64_t backup_id, const char *dir_path, const char *target_path);

/**
 * 验证备份完整性
 */
int md_verify_backup(uint64_t backup_id);

/**
 * 列出可用备份
 */
int md_list_backups(backup_metadata_t **backup_list);

/**
 * 删除备份
 */
int md_delete_backup(uint64_t backup_id);

// ================================
// 系统健康监控接口
// ================================

/**
 * 初始化系统健康监控
 */
int md_health_monitor_init(void);

/**
 * 销毁系统健康监控
 */
void md_health_monitor_destroy(void);

/**
 * 获取系统健康状态
 */
system_health_t md_get_system_health(void);

/**
 * 添加预警信息
 */
int md_add_alert(alert_level_t level, const char *component, const char *message);

/**
 * 获取未处理的预警
 */
int md_get_pending_alerts(alert_info_t **alert_list);

/**
 * 确认预警
 */
int md_acknowledge_alert(uint64_t alert_id);

/**
 * 运行系统健康检查
 */
int md_run_health_check(void);

/**
 * 生成健康报告
 */
int md_generate_health_report(const char *report_path);

/**
 * 修复工具：修复损坏的数据
 */
int md_repair_corrupted_data(void);

/**
 * 修复工具：重建索引
 */
int md_rebuild_indexes(void);

/**
 * 修复工具：清理孤儿数据
 */
int md_cleanup_orphaned_data(void);

// ================================
// 模块D初始化函数
// ================================

/**
 * 初始化模块D
 */
int module_d_init(void);

/**
 * 销毁模块D
 */
void module_d_destroy(void);

/**
 * 设置备份存储路径
 */
int md_set_backup_storage_path(const char *storage_path);

/**
 * 创建备份
 */
int md_create_backup(const char *description);

#endif // MODULE_D_H