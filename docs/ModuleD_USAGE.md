# 模块D：数据完整性与恢复机制使用指南

## 概述

模块D为智能备份文件系统提供了完整的数据完整性与恢复机制，包括数据完整性保护、事务日志系统、备份恢复工具和系统健康监控四大功能模块。

## 功能特性

### 1. 数据完整性保护
- **校验和机制**：为每个数据块计算并存储校验和
- **写时验证**：写入时验证数据完整性
- **定期扫描**：后台定期扫描验证所有数据的完整性
- **错误检测**：自动检测静默数据损坏

### 2. 事务日志系统
- **WAL（Write-Ahead Logging）**：所有修改先记录日志，再应用到数据
- **崩溃恢复**：系统崩溃后能够从日志恢复到一致状态
- **日志管理**：自动清理已提交的事务日志

### 3. 备份与恢复工具
- **增量备份**：支持增量式备份到远程存储
- **选择性恢复**：支持文件、目录或整个文件系统的恢复
- **恢复验证**：恢复后自动验证数据完整性
- **灾难恢复**：支持从完全损坏中恢复文件系统

### 4. 系统健康监控
- **实时监控**：监控文件系统健康状态
- **预警系统**：在检测到潜在问题时提前预警
- **修复工具**：提供数据修复和系统修复工具

## 快速开始

### 初始化模块D

```c
#include "module_d_integration.h"

// 初始化模块D集成
if (module_d_integration_init() != 0) {
    printf("模块D初始化失败\n");
    return -1;
}
```

### 配置备份存储路径

```c
// 配置备份存储路径
if (md_backup_init("/path/to/backup/storage") != 0) {
    printf("备份系统初始化失败\n");
    return -1;
}
```

### 启用/禁用特定功能

```c
// 启用数据完整性保护
module_d_set_feature_enabled("integrity_protection", true);

// 禁用事务日志（如果需要）
module_d_set_feature_enabled("transaction_logging", false);
```

## API 使用示例

### 数据完整性保护

```c
// 使用完整性保护的块写入
data_block_t *block = md_integrated_allocate_block(4096);
if (block) {
    // 写入数据并验证完整性
    md_integrated_write_block(block, data, size, 0);
    
    // 读取数据并验证完整性
    md_integrated_read_block(block, buffer, size, 0);
    
    // 释放块
    md_integrated_free_block(block);
}

// 启动后台完整性扫描
md_start_integrity_scan();
```

### 事务日志系统

```c
// 记录文件创建事务
md_log_file_creation(ino, "/path/to/file", 0644);

// 记录文件写入事务
md_log_file_write(ino, block_id, size);

// 记录文件删除事务
md_log_file_deletion(ino, "/path/to/file");

// 执行崩溃恢复（通常在系统启动时调用）
md_crash_recovery();
```

### 备份恢复工具

```c
// 创建完整备份
uint64_t backup_id = md_create_full_backup("系统完整备份");

// 创建增量备份
uint64_t inc_backup_id = md_create_incremental_backup(base_backup_id, "增量备份");

// 验证备份完整性
md_verify_backup(backup_id);

// 恢复整个文件系统
recovery_options_t options = {
    .verify_integrity = true,
    .preserve_metadata = true,
    .overwrite_existing = false,
    .target_backup_id = backup_id,
    .target_path = "/restore/path"
};
md_restore_filesystem(backup_id, &options);

// 调度自动备份（每24小时）
md_schedule_automatic_backups(24 * 60 * 60);
```

### 系统健康监控

```c
// 获取系统健康状态
system_health_t health = md_get_system_health();
printf("系统运行时间: %.2f 小时\n", health.system_uptime / 3600.0);
printf("损坏块数量: %lu\n", health.corrupted_blocks);

// 运行健康检查
md_run_health_check();

// 获取未处理的预警
alert_info_t *alerts;
if (md_get_pending_alerts(&alerts) == 0) {
    alert_info_t *current = alerts;
    while (current) {
        printf("预警: [%s] %s: %s\n", 
               current->component, 
               ctime(&current->timestamp),
               current->message);
        current = current->next;
    }
}

// 生成健康报告
md_generate_health_report("/path/to/health_report.txt");
```

## 配置选项

### 数据完整性保护配置

```c
// 设置校验和算法
module_d_state.checksum_algorithm = CHECKSUM_SHA256_PARTIAL; // 默认
// 或者使用 CRC32
module_d_state.checksum_algorithm = CHECKSUM_CRC32;

// 启用/禁用写时验证
module_d_state.enable_write_verification = true; // 默认启用
```

### 事务日志系统配置

```c
// 启用/禁用WAL
module_d_state.wal_enabled = true; // 默认启用

// 设置WAL段大小（默认16MB）
#define WAL_SEGMENT_SIZE (16 * 1024 * 1024)
```

### 备份系统配置

```c
// 设置备份存储路径
module_d_state.backup_storage_path = "/path/to/backup/storage";
```

### 健康监控配置

```c
// 设置健康检查间隔（秒）
#define HEALTH_CHECK_INTERVAL 300 // 5分钟
```

## 集成到现有文件系统

### 替换原有的块操作

将原有的块读写操作替换为模块D的集成版本：

```c
// 原来的代码：
// data_block_t *block = allocate_block(size);
// write_block(block, data, size, offset);
// read_block(block, buf, size, offset);
// free_block(block);

// 替换为：
data_block_t *block = md_integrated_allocate_block(size);
md_integrated_write_block(block, data, size, offset);
md_integrated_read_block(block, buf, size, offset);
md_integrated_free_block(block);
```

### 在文件操作中添加事务日志

```c
// 在文件创建时：
md_log_file_creation(ino, path, mode);

// 在文件写入时：
md_log_file_write(ino, block_id, size);

// 在文件删除时：
md_log_file_deletion(ino, path);
```

## 故障排除

### 常见问题

1. **模块D初始化失败**
   - 检查依赖库（OpenSSL, zlib）是否正确安装
   - 确认有足够的权限创建必要的文件和目录

2. **备份创建失败**
   - 检查备份存储路径是否存在且有写权限
   - 确认存储空间充足

3. **完整性验证失败**
   - 检查数据块是否损坏
   - 验证校验和算法配置是否正确

4. **事务日志过大**
   - 定期调用 `md_cleanup_committed_transactions()` 清理已提交的事务
   - 考虑调整WAL段大小

### 调试信息

启用调试输出可以查看模块D的详细运行信息：

```c
// 在代码中添加调试输出
printf("模块D: 执行完整性检查，块ID: %lu\n", block->block_id);
```

## 性能考虑

### 性能影响

- **数据完整性保护**：会增加约5-10%的CPU开销
- **事务日志系统**：会增加约10-15%的I/O开销
- **备份系统**：备份期间可能会影响系统性能
- **健康监控**：开销较小，通常可以忽略

### 优化建议

1. **选择性启用功能**：根据实际需求启用必要的功能
2. **调整扫描频率**：降低完整性扫描的频率以减少性能影响
3. **使用增量备份**：减少完整备份的频率
4. **合理设置WAL大小**：根据系统负载调整WAL段大小

## 安全考虑

### 数据安全

- 校验和机制可以检测数据损坏
- 事务日志确保数据一致性
- 备份系统提供数据恢复能力

### 访问控制

- 备份数据应存储在安全的位置
- 健康报告可能包含敏感信息，需要妥善保护
- 预警信息应仅对授权用户可见

## 扩展开发

### 添加新的校验和算法

```c
// 在 module_d.c 中添加新的校验和函数
static uint32_t new_checksum_algorithm(const void *data, size_t size) {
    // 实现新的校验和算法
    return calculated_checksum;
}

// 在 md_calculate_checksum 函数中添加对新算法的支持
```

### 自定义预警规则

```c
// 在健康检查中添加自定义规则
if (custom_condition) {
    md_add_alert(ALERT_WARNING, "自定义监控", "检测到自定义问题");
}
```

## 相关文档

- [模块A：基础文件系统操作](../docs/ModuleA_USAGE.md)
- [模块B：版本管理系统](../docs/ModuleB_USAGE.md)
- [模块C：智能存储优化](../docs/ModuleC_USAGE.md)
- [完整API参考](../include/module_d.h)
- [集成接口参考](../include/module_d_integration.h)

## 测试命令

### 使用扩展属性测试模块D功能

模块D的所有功能都可以通过文件系统的扩展属性（xattr）进行测试，无需修改代码。以下是完整的测试命令集：

#### 数据完整性保护测试
```bash
# 启用数据完整性保护
setfattr -n user.integrity.enable -v 1 /挂载点/测试目录

# 创建测试文件s并写入数据
echo "测试数据内容" > /挂载点/测试目录/integrity_test.txt

# 获取完整性校验和
getfattr -n user.integrity.checksum /挂载点/测试目录/integrity_test.txt

# 启动后台完整性扫描
setfattr -n user.integrity.scan -v 1 /挂载点/测试目录

# 修复损坏数据（如果检测到）
setfattr -n user.integrity.repair -v 1 /挂载点/测试目录
```

#### 事务日志系统测试
```bash
# 启用事务日志
setfattr -n user.transaction.enable -v 1 /挂载点/测试目录

# 创建文件并记录事务
echo "事务测试" > /挂载点/测试目录/transaction_test.txt

# 修改文件并记录事务
echo "修改后的内容" > /挂载点/测试目录/transaction_test.txt

# 查看事务记录
getfattr -n user.transaction.created /挂载点/测试目录/transaction_test.txt
getfattr -n user.transaction.modified /挂载点/测试目录/transaction_test.txt

# 模拟崩溃恢复
setfattr -n user.crash.recovery -v 1 /挂载点/测试目录
```

#### 备份系统测试
```bash
# 配置备份存储路径
setfattr -n user.backup.storage_path -v /tmp/backup_test /挂载点/测试目录

# 创建完整备份
setfattr -n user.backup.create -v "full_backup_$(date +%s)" /挂载点/测试目录

# 验证备份完整性
getfattr -n user.backup.verified /挂载点/测试目录

# 清理孤儿数据
setfattr -n user.orphan.cleanup -v 1 /挂载点/测试目录
```

#### 系统健康监控测试
```bash
# 启用健康监控
setfattr -n user.health.monitor -v 1 /挂载点/测试目录

# 获取健康状态
getfattr -n user.health.status /挂载点/测试目录

# 生成健康报告
setfattr -n user.health.report -v /tmp/health_report.txt /挂载点/测试目录

# 启用性能监控
setfattr -n user.performance.monitor -v 1 /挂载点/测试目录
setfattr -n user.storage.monitor -v 1 /挂载点/测试目录
setfattr -n user.cache.monitor -v 1 /挂载点/测试目录

# 触发预警条件
setfattr -n user.alert.trigger -v "high_usage" /挂载点/测试目录

# 获取预警列表
getfattr -n user.alert.list /挂载点/测试目录
```

### 自动化测试脚本

可以使用以下脚本一次性测试所有模块D功能：

```bash
#!/bin/bash
MOUNT_POINT="$1"
TEST_DIR="$MOUNT_POINT/module_d_test_$(date +%s)"

mkdir -p "$TEST_DIR"

echo "=== 模块D功能测试 ==="

# 数据完整性测试
echo "1. 数据完整性保护测试..."
setfattr -n user.integrity.enable -v 1 "$TEST_DIR"
echo "测试数据" > "$TEST_DIR/integrity_test.txt"
getfattr -n user.integrity.checksum "$TEST_DIR/integrity_test.txt" >/dev/null 2>&1 && echo "✓ 数据完整性测试通过"

# 事务日志测试
echo "2. 事务日志系统测试..."
setfattr -n user.transaction.enable -v 1 "$TEST_DIR"
echo "事务测试" > "$TEST_DIR/transaction_test.txt"
getfattr -n user.transaction.created "$TEST_DIR/transaction_test.txt" >/dev/null 2>&1 && echo "✓ 事务日志测试通过"

# 备份系统测试
echo "3. 备份系统测试..."
setfattr -n user.backup.storage_path -v "/tmp/backup_test" "$TEST_DIR"
setfattr -n user.backup.create -v "test_backup" "$TEST_DIR"
getfattr -n user.backup.verified "$TEST_DIR" >/dev/null 2>&1 && echo "✓ 备份系统测试通过"

# 健康监控测试
echo "4. 系统健康监控测试..."
setfattr -n user.health.monitor -v 1 "$TEST_DIR"
getfattr -n user.health.status "$TEST_DIR" >/dev/null 2>&1 && echo "✓ 健康监控测试通过"

echo "=== 所有测试完成 ==="
```

### 集成测试

模块D的测试已经集成到项目的综合测试脚本中：

```bash
# 运行完整的测试套件（包含模块D测试）
./scripts/test_all.sh

# 或者只运行模块D相关测试
./scripts/test_all.sh | grep -A 20 "模块D"
```

### 调试和验证

如果测试失败，可以启用详细日志输出：

```bash
# 启用调试模式（如果支持）
export SMARTBACKUP_DEBUG=1

# 重新挂载文件系统并运行测试
./scripts/run.sh -d
```

## 技术支持

如果遇到问题，请检查：
1. 系统日志中的错误信息
2. 模块D的调试输出
3. 健康报告中的预警信息
4. 备份验证结果

如需进一步帮助，请参考源代码注释或联系开发团队。