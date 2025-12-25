/**
 * 模块D集成接口
 * 提供模块D与文件系统其他模块的集成接口
 */

#ifndef MODULE_D_INTEGRATION_H
#define MODULE_D_INTEGRATION_H

#include "smartbackupfs.h"
#include "module_d.h"

// 模块D集成状态
typedef struct module_d_integration_state {
    bool integrity_protection_enabled;
    bool transaction_logging_enabled;
    bool backup_system_enabled;
    bool health_monitoring_enabled;
    
    // 集成统计
    uint64_t integrity_checks_performed;
    uint64_t transactions_logged;
    uint64_t backups_created;
    uint64_t health_checks_performed;
} module_d_integration_state_t;

// 全局集成状态
extern module_d_integration_state_t module_d_integration_state;

// ================================
// 数据完整性保护集成接口
// ================================

/**
 * 在写入数据块时集成完整性保护
 */
int md_integrated_write_block(data_block_t *block, const void *data, size_t size, off_t offset);

/**
 * 在读取数据块时集成完整性验证
 */
int md_integrated_read_block(data_block_t *block, char *buf, size_t size, off_t offset);

/**
 * 在分配新数据块时集成完整性保护
 */
data_block_t *md_integrated_allocate_block(size_t size);

/**
 * 在释放数据块时集成完整性检查
 */
void md_integrated_free_block(data_block_t *block);

// ================================
// 事务日志集成接口
// ================================

/**
 * 在文件创建时记录事务
 */
int md_log_file_creation(uint64_t ino, const char *path, mode_t mode);

/**
 * 在文件写入时记录事务
 */
int md_log_file_write(uint64_t ino, uint64_t block_id, size_t size);

/**
 * 在文件删除时记录事务
 */
int md_log_file_deletion(uint64_t ino, const char *path);

/**
 * 在元数据更新时记录事务
 */
int md_log_metadata_update(uint64_t ino, const char *attribute, const void *old_value, const void *new_value);

// ================================
// 备份恢复集成接口
// ================================

/**
 * 自动创建定期备份
 */
int md_schedule_automatic_backups(uint32_t interval_seconds);

/**
 * 在文件系统挂载时检查是否需要恢复
 */
int md_check_recovery_needed(void);

/**
 * 集成备份验证到文件系统操作中
 */
int md_integrated_backup_verification(uint64_t backup_id);

// ================================
// 系统健康监控集成接口
// ================================

/**
 * 监控文件系统操作性能
 */
int md_monitor_operation_performance(const char *operation, uint64_t duration_ns);

/**
 * 监控存储使用情况
 */
int md_monitor_storage_usage(uint64_t used_bytes, uint64_t total_bytes);

/**
 * 监控缓存命中率
 */
int md_monitor_cache_performance(double hit_ratio);

/**
 * 监控数据完整性状态
 */
int md_monitor_integrity_status(uint64_t total_blocks, uint64_t corrupted_blocks);

// ================================
// 模块D集成管理接口
// ================================

/**
 * 初始化模块D集成
 */
int module_d_integration_init(void);

/**
 * 销毁模块D集成
 */
void module_d_integration_destroy(void);

/**
 * 启用/禁用模块D功能
 */
int module_d_set_feature_enabled(const char *feature_name, bool enabled);

/**
 * 获取模块D集成状态
 */
module_d_integration_state_t module_d_get_integration_status(void);

/**
 * 生成模块D集成报告
 */
int module_d_generate_integration_report(const char *report_path);

#endif // MODULE_D_INTEGRATION_H