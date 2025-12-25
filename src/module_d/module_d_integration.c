/**
 * 模块D集成实现
 */

#include "module_d_integration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h> // 添加sleep函数声明

// 声明模块D初始化函数（在module_d.c中定义）
int module_d_init(void);
void module_d_destroy(void);

// 全局集成状态
module_d_integration_state_t module_d_integration_state;

// ================================
// 数据完整性保护集成实现
// ================================

int md_integrated_write_block(data_block_t *block, const void *data, size_t size, off_t offset) {
    if (!module_d_integration_state.integrity_protection_enabled) {
        // 如果不启用完整性保护，直接写入
        return write_block(block, data, size, offset);
    }
    
    // 使用模块D的完整性保护写入
    int result = md_write_with_verification(block, data, size);
    if (result == 0) {
        module_d_integration_state.integrity_checks_performed++;
    }
    
    return result;
}

int md_integrated_read_block(data_block_t *block, char *buf, size_t size, off_t offset) {
    if (!module_d_integration_state.integrity_protection_enabled) {
        // 如果不启用完整性保护，直接读取
        return read_block(block, buf, size, offset);
    }
    
    // 先读取数据
    int result = read_block(block, buf, size, offset);
    if (result != 0) {
        return result;
    }
    
    // 验证数据完整性
    result = md_verify_block_integrity(block);
    if (result != 0) {
        printf("数据块完整性验证失败，块ID: %lu\n", block->block_id);
        md_add_alert(ALERT_ERROR, "数据完整性", "读取时发现损坏的数据块");
    }
    
    module_d_integration_state.integrity_checks_performed++;
    return result;
}

data_block_t *md_integrated_allocate_block(size_t size) {
    data_block_t *block = allocate_block(size);
    if (!block) {
        return NULL;
    }
    
    if (module_d_integration_state.integrity_protection_enabled) {
        // 初始化校验和
        if (block->data && size > 0) {
            uint32_t checksum = md_calculate_checksum(block->data, size);
            memcpy(block->hash, &checksum, sizeof(checksum));
        }
    }
    
    return block;
}

void md_integrated_free_block(data_block_t *block) {
    if (!block) {
        return;
    }
    
    if (module_d_integration_state.integrity_protection_enabled) {
        // 在释放前进行最后的完整性检查
        int integrity_result = md_verify_block_integrity(block);
        if (integrity_result != 0) {
            printf("释放前检测到损坏的数据块，块ID: %lu\n", block->block_id);
            md_add_alert(ALERT_WARNING, "数据完整性", "释放时发现损坏的数据块");
        }
    }
    
    free_block(block);
}

// ================================
// 事务日志集成实现
// ================================

int md_log_file_creation(uint64_t ino, const char *path, mode_t mode) {
    if (!module_d_integration_state.transaction_logging_enabled) {
        return 0;
    }
    
    uint64_t tx_id = md_transaction_begin(TX_CREATE_FILE);
    if (tx_id == 0) {
        return -1;
    }
    
    // 记录文件创建事务
    struct {
        uint64_t ino;
        char path[256];
        mode_t mode;
    } tx_data;
    
    tx_data.ino = ino;
    strncpy(tx_data.path, path, sizeof(tx_data.path) - 1);
    tx_data.path[sizeof(tx_data.path) - 1] = '\0';
    tx_data.mode = mode;
    
    int result = md_transaction_log(tx_id, &tx_data, sizeof(tx_data));
    if (result == 0) {
        result = md_transaction_commit(tx_id);
    } else {
        md_transaction_rollback(tx_id);
    }
    
    if (result == 0) {
        module_d_integration_state.transactions_logged++;
    }
    
    return result;
}

int md_log_file_write(uint64_t ino, uint64_t block_id, size_t size) {
    if (!module_d_integration_state.transaction_logging_enabled) {
        return 0;
    }
    
    uint64_t tx_id = md_transaction_begin(TX_WRITE_DATA);
    if (tx_id == 0) {
        return -1;
    }
    
    // 记录文件写入事务
    struct {
        uint64_t ino;
        uint64_t block_id;
        size_t size;
    } tx_data;
    
    tx_data.ino = ino;
    tx_data.block_id = block_id;
    tx_data.size = size;
    
    int result = md_transaction_log(tx_id, &tx_data, sizeof(tx_data));
    if (result == 0) {
        result = md_transaction_commit(tx_id);
    } else {
        md_transaction_rollback(tx_id);
    }
    
    if (result == 0) {
        module_d_integration_state.transactions_logged++;
    }
    
    return result;
}

int md_log_file_deletion(uint64_t ino, const char *path) {
    if (!module_d_integration_state.transaction_logging_enabled) {
        return 0;
    }
    
    uint64_t tx_id = md_transaction_begin(TX_DELETE_FILE);
    if (tx_id == 0) {
        return -1;
    }
    
    // 记录文件删除事务
    struct {
        uint64_t ino;
        char path[256];
    } tx_data;
    
    tx_data.ino = ino;
    strncpy(tx_data.path, path, sizeof(tx_data.path) - 1);
    tx_data.path[sizeof(tx_data.path) - 1] = '\0';
    
    int result = md_transaction_log(tx_id, &tx_data, sizeof(tx_data));
    if (result == 0) {
        result = md_transaction_commit(tx_id);
    } else {
        md_transaction_rollback(tx_id);
    }
    
    if (result == 0) {
        module_d_integration_state.transactions_logged++;
    }
    
    return result;
}

int md_log_metadata_update(uint64_t ino, const char *attribute, const void *old_value, const void *new_value) {
    if (!module_d_integration_state.transaction_logging_enabled) {
        return 0;
    }
    
    uint64_t tx_id = md_transaction_begin(TX_METADATA_UPDATE);
    if (tx_id == 0) {
        return -1;
    }
    
    // 记录元数据更新事务（简化版本）
    struct {
        uint64_t ino;
        char attribute[64];
    } tx_data;
    
    tx_data.ino = ino;
    strncpy(tx_data.attribute, attribute, sizeof(tx_data.attribute) - 1);
    tx_data.attribute[sizeof(tx_data.attribute) - 1] = '\0';
    
    int result = md_transaction_log(tx_id, &tx_data, sizeof(tx_data));
    if (result == 0) {
        result = md_transaction_commit(tx_id);
    } else {
        md_transaction_rollback(tx_id);
    }
    
    if (result == 0) {
        module_d_integration_state.transactions_logged++;
    }
    
    return result;
}

// ================================
// 备份恢复集成实现
// ================================

static pthread_t automatic_backup_thread;
static bool automatic_backup_running = false;

/**
 * 自动备份线程函数
 */
static void* automatic_backup_thread_func(void *arg) {
    uint32_t interval = *(uint32_t*)arg;
    
    printf("自动备份线程启动，间隔: %u 秒\n", interval);
    
    while (automatic_backup_running) {
        sleep(interval);
        
        if (automatic_backup_running) {
            printf("执行自动备份...\n");
            uint64_t backup_id = md_create_full_backup("自动定期备份");
            if (backup_id > 0) {
                module_d_integration_state.backups_created++;
                printf("自动备份完成，备份ID: %lu\n", backup_id);
            } else {
                printf("自动备份失败\n");
                md_add_alert(ALERT_ERROR, "备份系统", "自动备份失败");
            }
        }
    }
    
    printf("自动备份线程停止\n");
    return NULL;
}

int md_schedule_automatic_backups(uint32_t interval_seconds) {
    if (!module_d_integration_state.backup_system_enabled) {
        printf("备份系统未启用，无法调度自动备份\n");
        return -1;
    }
    
    if (automatic_backup_running) {
        printf("自动备份已经在运行中\n");
        return 0;
    }
    
    automatic_backup_running = true;
    
    uint32_t *interval = malloc(sizeof(uint32_t));
    if (!interval) {
        automatic_backup_running = false;
        return -1;
    }
    *interval = interval_seconds;
    
    if (pthread_create(&automatic_backup_thread, NULL, 
                       automatic_backup_thread_func, interval) != 0) {
        free(interval);
        automatic_backup_running = false;
        printf("无法启动自动备份线程\n");
        return -1;
    }
    
    printf("自动备份已调度，间隔: %u 秒\n", interval_seconds);
    return 0;
}

int md_check_recovery_needed(void) {
    if (!module_d_integration_state.backup_system_enabled) {
        return 0;
    }
    
    printf("检查是否需要恢复...\n");
    
    // 这里实现恢复检查逻辑
    // 检查文件系统状态，判断是否需要从备份恢复
    
    return 0; // 0表示不需要恢复
}

int md_integrated_backup_verification(uint64_t backup_id) {
    if (!module_d_integration_state.backup_system_enabled) {
        return 0;
    }
    
    printf("验证备份完整性，备份ID: %lu\n", backup_id);
    
    int result = md_verify_backup(backup_id);
    if (result != 0) {
        md_add_alert(ALERT_ERROR, "备份系统", "备份验证失败");
    }
    
    return result;
}

// ================================
// 系统健康监控集成实现
// ================================

int md_monitor_operation_performance(const char *operation, uint64_t duration_ns) {
    if (!module_d_integration_state.health_monitoring_enabled) {
        return 0;
    }
    
    // 监控操作性能
    if (duration_ns > 1000000000) { // 超过1秒
        char message[256];
        snprintf(message, sizeof(message), "操作 '%s' 执行时间过长: %.2f 秒", 
                 operation, duration_ns / 1000000000.0);
        md_add_alert(ALERT_WARNING, "性能监控", message);
    }
    
    return 0;
}

int md_monitor_storage_usage(uint64_t used_bytes, uint64_t total_bytes) {
    if (!module_d_integration_state.health_monitoring_enabled) {
        return 0;
    }
    
    // 监控存储使用情况
    if (total_bytes > 0) {
        double usage_ratio = (double)used_bytes / total_bytes;
        if (usage_ratio > 0.9) { // 使用率超过90%
            char message[256];
            snprintf(message, sizeof(message), "存储使用率过高: %.1f%%", usage_ratio * 100);
            md_add_alert(ALERT_WARNING, "存储监控", message);
        }
    }
    
    return 0;
}

int md_monitor_cache_performance(double hit_ratio) {
    if (!module_d_integration_state.health_monitoring_enabled) {
        return 0;
    }
    
    // 监控缓存性能
    if (hit_ratio < 0.5) { // 命中率低于50%
        char message[256];
        snprintf(message, sizeof(message), "缓存命中率过低: %.1f%%", hit_ratio * 100);
        md_add_alert(ALERT_WARNING, "缓存监控", message);
    }
    
    return 0;
}

int md_monitor_integrity_status(uint64_t total_blocks, uint64_t corrupted_blocks) {
    if (!module_d_integration_state.health_monitoring_enabled) {
        return 0;
    }
    
    // 监控数据完整性状态
    if (total_blocks > 0) {
        double corruption_ratio = (double)corrupted_blocks / total_blocks;
        if (corruption_ratio > 0.01) { // 损坏率超过1%
            char message[256];
            snprintf(message, sizeof(message), "数据损坏率过高: %.2f%%", corruption_ratio * 100);
            md_add_alert(ALERT_ERROR, "完整性监控", message);
        }
    }
    
    return 0;
}

// ================================
// 模块D集成管理实现
// ================================

int module_d_integration_init(void) {
    memset(&module_d_integration_state, 0, sizeof(module_d_integration_state));
    
    // 默认启用所有功能
    module_d_integration_state.integrity_protection_enabled = true;
    module_d_integration_state.transaction_logging_enabled = true;
    module_d_integration_state.backup_system_enabled = true;
    module_d_integration_state.health_monitoring_enabled = true;
    
    // 初始化模块D
    if (module_d_init() != 0) {
        printf("模块D初始化失败\n");
        return -1;
    }
    
    printf("模块D集成初始化完成\n");
    return 0;
}

void module_d_integration_destroy(void) {
    // 停止自动备份
    if (automatic_backup_running) {
        automatic_backup_running = false;
        pthread_join(automatic_backup_thread, NULL);
    }
    
    // 销毁模块D
    module_d_destroy();
    
    printf("模块D集成已销毁\n");
}

int module_d_set_feature_enabled(const char *feature_name, bool enabled) {
    if (!feature_name) {
        return -1;
    }
    
    if (strcmp(feature_name, "integrity_protection") == 0) {
        module_d_integration_state.integrity_protection_enabled = enabled;
    } else if (strcmp(feature_name, "transaction_logging") == 0) {
        module_d_integration_state.transaction_logging_enabled = enabled;
    } else if (strcmp(feature_name, "backup_system") == 0) {
        module_d_integration_state.backup_system_enabled = enabled;
    } else if (strcmp(feature_name, "health_monitoring") == 0) {
        module_d_integration_state.health_monitoring_enabled = enabled;
    } else {
        printf("未知功能: %s\n", feature_name);
        return -1;
    }
    
    printf("功能 '%s' %s\n", feature_name, enabled ? "已启用" : "已禁用");
    return 0;
}

module_d_integration_state_t module_d_get_integration_status(void) {
    return module_d_integration_state;
}

int module_d_generate_integration_report(const char *report_path) {
    if (!report_path) {
        return -1;
    }
    
    printf("生成模块D集成报告: %s\n", report_path);
    
    // 这里实现报告生成逻辑
    // 将集成状态和统计信息写入文件
    
    FILE *report_file = fopen(report_path, "w");
    if (!report_file) {
        printf("无法创建报告文件\n");
        return -1;
    }
    
    fprintf(report_file, "模块D集成报告\n");
    fprintf(report_file, "生成时间: %s", ctime(&(time_t){time(NULL)}));
    fprintf(report_file, "\n");
    
    fprintf(report_file, "功能状态:\n");
    fprintf(report_file, "- 数据完整性保护: %s\n", 
            module_d_integration_state.integrity_protection_enabled ? "启用" : "禁用");
    fprintf(report_file, "- 事务日志: %s\n", 
            module_d_integration_state.transaction_logging_enabled ? "启用" : "禁用");
    fprintf(report_file, "- 备份系统: %s\n", 
            module_d_integration_state.backup_system_enabled ? "启用" : "禁用");
    fprintf(report_file, "- 健康监控: %s\n", 
            module_d_integration_state.health_monitoring_enabled ? "启用" : "禁用");
    fprintf(report_file, "\n");
    
    fprintf(report_file, "统计信息:\n");
    fprintf(report_file, "- 完整性检查次数: %lu\n", 
            module_d_integration_state.integrity_checks_performed);
    fprintf(report_file, "- 事务日志记录数: %lu\n", 
            module_d_integration_state.transactions_logged);
    fprintf(report_file, "- 备份创建次数: %lu\n", 
            module_d_integration_state.backups_created);
    fprintf(report_file, "- 健康检查次数: %lu\n", 
            module_d_integration_state.health_checks_performed);
    
    fclose(report_file);
    
    printf("模块D集成报告生成完成\n");
    return 0;
}