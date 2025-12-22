# Module B 使用说明（透明版本管理，v4.0）

## 模块概览
- 目标：在不影响模块A基础性能的前提下，为文件提供透明的版本管理与回溯能力。
- 触发策略：
   - 事件：rename/unlink 前自动建版
   - 变化：写入后若块级差异 >10% 自动建版（滚动哈希）
   - 定时：按 `version_time_interval`（默认 3600s）周期建版
   - 手动：xattr `user.version.create/delete/important` 触发；`user.version.pinned` 跳过清理
   - 清理：按 `version_max_versions` 与 `version_expire_days` 自动清理（跳过 important/pinned）
- 主要结构：`version_chain_t`（版本链）、`version_node_t`（单个版本），缓存复用 `lru_cache_t`。
- 配置字段（`fs_state_t`）：`version_time_interval`、`version_clean_interval`、`version_retention_count`（默认10）、`version_retention_days`（默认30）、`version_max_versions`、`version_expire_days`、`version_cache`、`version_cleaner_thread`。
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
7. 访问指定版本：`cat /tmp/smartbackup/file@v3` 或 `file@latest`；时间表达式可用 `file@3s`、`file@2h`、`file@1d`、`file@1w`、`file@yesterday`、`file@today` 选择历史版本（选取不晚于目标时间的最新版本）。
8. 删除版本：`setfattr -n user.version.delete -v v3 /tmp/smartbackup/file`。
9. 标记重要版本（跳过自动清理）：`setfattr -n user.version.important -v v2 /tmp/smartbackup/file`（移除：`setfattr -x user.version.important ...`）。

## xattr 接口速查（v4 新增/扩展）
- 创建版本：`setfattr -n user.version.create -v 1 <path>`
- 删除版本：`setfattr -n user.version.delete -v vN <path>`
- 标记重要版本：`setfattr -n user.version.important -v vN <path>`（跳过清理；移除用 `-x`）
- 文件级 pinned：`setfattr -n user.version.pinned -v 1 <path>`（整文件版本链跳过清理；移除用 `-x`）

## 路径语法
- 列表：`<path>@versions` —— 在 readdir 中返回该文件的所有版本名（v1, v2, ...）。
- 指定版本：`<path>@vN` 或 `<path>@latest` —— 解析到该版本的元数据。
- 时间表达式：支持 `<path>@3s`、`<path>@2h`、`<path>@1d`、`<path>@1w`、`<path>@yesterday`、`<path>@today`（选择不晚于目标时间的最新版本）。

## 版本创建策略
- 事件触发：unlink、rename 前自动建版。
- 定时策略：后台线程按 `version_time_interval` 定期为文件建版。
- 内容变化：写入后基于滚动哈希计算块差异，>10% 触发建版。
- 手动快照：xattr `user.version.create` 触发。

## 存储与清理
- 增量信息：每个版本记录 `diff_blocks`（变更块索引）和 `block_checksums`（每块校验和），并为每块保存数据快照以支持版本读取。
- 清理策略：后台线程按 `version_clean_interval` 轮询，保留最近 `version_max_versions`（或 `version_retention_count`）个版本，并删除超过 `version_expire_days`（或 `version_retention_days`）的旧版本；带 `important`/`pinned` 标签的版本跳过清理。
- 重要版本标记：xattr `user.version.pinned` 为文件级重要标记，`user.version.important` 为版本级标记；清理时跳过。

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
- 手动快照/删除/标记通过 xattr 触发，仍可补充 CLI/管理工具或 FUSE 扩展 op。
- 性能与容量占用未做基准测试；需后续补充性能脚本与统计。
