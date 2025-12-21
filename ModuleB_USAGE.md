# Module B 使用说明（透明版本管理）

## 模块概览
- 目标：在不影响模块A基础性能的前提下，为文件提供透明的版本管理与回溯能力。
- 触发策略：
   - 事件：rename/unlink 前自动建版
   - 变化：写入后若块级差异 >10% 自动建版（滚动哈希）
   - 定时：按 `version_time_interval`（默认 3600s）周期建版
   - 手动：xattr `user.version.create` 触发
- 主要结构：`version_chain_t`（版本链）、`version_node_t`（单个版本），缓存复用 `lru_cache_t`。
- 配置字段（`fs_state_t`）：`version_time_interval`、`version_retention_count`（默认10）、`version_retention_days`（默认30）、`version_cache`、`version_cleaner_thread`。
- 元数据扩展（`file_metadata_t`）：`version_count`、`latest_version_id`、`last_version_time`、`version_pinned`。

## 快速使用
1. 构建
   ```bash
   ./scripts/build.sh
   ```
2. 启动挂载（默认挂载点 /tmp/smartbackup）
   ```bash
   ./scripts/run.sh -d
   ```
3. 进行文件操作触发版本：rename/unlink、写入（差异>10%）、后台定时。
4. 手动快照：
   ```bash
   setfattr -n user.version.create -v 1 /tmp/smartbackup/path/to/file
   ```
5. 标记重要版本（跳过清理）：
   ```bash
   setfattr -n user.version.pinned -v 1 /tmp/smartbackup/path/to/file
   # 取消
   setfattr -x user.version.pinned /tmp/smartbackup/path/to/file
   ```
6. 查看版本列表：`ls /tmp/smartbackup/file@versions`（返回 v1, v2, ...）。
7. 访问指定版本：`cat /tmp/smartbackup/file@v3` 或 `file@latest`；时间表达式可用 `file@2h`、`file@yesterday` 选择历史版本。

## 路径语法
- 列表：`<path>@versions` —— 在 readdir 中返回该文件的所有版本名（v1, v2, ...）。
- 指定版本：`<path>@vN` 或 `<path>@latest` —— 解析到该版本的元数据。
- 时间表达式：支持 `<path>@2h`、`<path>@1d`、`<path>@yesterday`（选择不晚于目标时间的最新版本）。

## 版本创建策略
- 事件触发：unlink、rename 前自动建版。
- 定时策略：后台线程按 `version_time_interval` 定期为文件建版。
- 内容变化：写入后基于滚动哈希计算块差异，>10% 触发建版。
- 手动快照：xattr `user.version.create` 触发。

## 存储与清理
- 增量信息：每个版本记录 `diff_blocks`（变更块索引）和 `block_checksums`（每块校验和）。
- 清理策略：保留最近 `version_retention_count` 个版本，且保留 `version_retention_days` 天内的版本；后台线程周期性执行。
- 重要版本标记：xattr `user.version.pinned` 为重要版本，清理时跳过。

## 测试
- 基本版本测试脚本：
   ```bash
   ./scripts/test_versions.sh
   ```
   说明：脚本假定已挂载到 /tmp/smartbackup，通过多次重命名/删除触发版本创建，并列出 `@versions` 目录；可在执行前确保 `build/bin/smartbackup-fs -f /tmp/smartbackup` 已运行。
- 手动校验：
   ```bash
   # 手动快照
   setfattr -n user.version.create -v 1 /tmp/smartbackup/test.txt
   # 列表
   ls /tmp/smartbackup/test.txt@versions
   # 访问历史
   cat /tmp/smartbackup/test.txt@2h
   # pinned 标记
   setfattr -n user.version.pinned -v 1 /tmp/smartbackup/test.txt
   ```

## 已知限制与后续计划
- 版本数据仍未分离存储（仅元数据与校验和差异）；需与模块C去重/压缩结合以实现真正增量恢复。
- 手动快照当前通过 xattr 触发，仍可补充 CLI/管理工具或 FUSE 扩展 op。
- 性能与容量占用未做基准测试；需后续补充性能脚本与统计。
