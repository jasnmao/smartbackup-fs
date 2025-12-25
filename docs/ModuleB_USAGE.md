# Module B 使用说明（透明版本管理，v6.0）

## v6.0 新增/变化
- 增量存储与继承：版本仅保存差异块数据(`snapshots`+`diff_blocks`)，未变更块通过 `parent` 继承；`stored_bytes` 记录增量占用。
- 容量上限清理：支持 `user.version.max_size_mb`（默认 1GB），保留策略同时考虑数量/时间/容量，容量超限时触发从最旧版本开始清理。
- 父子修复：删除父版本时对子版本未持有的数据进行物化拷贝，重定向 `parent` 防止悬挂引用。
- 引用计数职责分离：版本清理不再修改底层块引用计数，块生命周期由文件写路径/去重模块负责，避免容量清理时因误减引用导致崩溃。
- 版本访问能力保持：`snapshot_get_block_data` 自顶向上解析父链，确保增量链可读。

## 模块概览
- 目标：在不影响模块A基础性能的前提下，为文件提供透明版本管理与回溯能力。
- 触发策略：事件(rename/unlink 前自动建版)、变化(写入后块级差异>10%)、定时(`version_time_interval`)、手动(xattr create/delete/important/pinned)、清理(`version_max_versions`/`version_expire_days`/`version_retention_size_mb`，跳过 important/pinned)。
- 主要结构：`version_chain_t`（双向版本链）、`version_node_t`（含 `parent_id`、`description`、`block_map`、`stored_bytes`）、缓存复用 `lru_cache_t`。
- 配置字段（`fs_state_t`）：`version_time_interval`、`version_clean_interval`、`version_retention_count`、`version_retention_days`、`version_max_versions`、`version_expire_days`、`version_retention_size_mb`、`version_cache`、`version_cleaner_thread`，别名 `max_versions`/`expire_days` 便于模块C读取。
- 元数据扩展（`file_metadata_t`）：`version_count`、`latest_version_id`、`last_version_time`、`version_pinned`、`current_block_map`、`data_hash`、`version_lock`。
- 块与映射：`data_block_t` 含 `hash[32]`、`compressed_size`、`ref_count/ref_lock`；`block_map_t` 包含 `version_id`、`block_index`（块ID->块指针），为去重/压缩提供索引。

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
6. 查看版本列表：`ls /tmp/smartbackup/file@versions`（返回 `vN | YYYY-MM-DD HH:MM:SS | desc`，脚本可用 `cut -d'|' -f1` 取版本号）。
7. 访问指定版本：`cat /tmp/smartbackup/file@v3` 或 `file@latest`；时间表达式可用 `file@3s`、`file@2h`、`file@1d`、`file@1w`、`file@yesterday`、`file@today` 选择历史版本（选取不晚于目标时间的最新版本）。
8. 删除版本：`setfattr -n user.version.delete -v v3 /tmp/smartbackup/file`。
9. 标记重要版本（跳过自动清理）：`setfattr -n user.version.important -v v2 /tmp/smartbackup/file`（移除：`setfattr -x user.version.important ...`）。

## xattr 接口速查（v6 扩展）
- 创建版本：`setfattr -n user.version.create -v 1 <path>`
- 删除版本：`setfattr -n user.version.delete -v vN <path>`
- 标记重要版本：`setfattr -n user.version.important -v vN <path>`（跳过清理；移除用 `-x`）
- 文件级 pinned：`setfattr -n user.version.pinned -v 1 <path>`（整文件版本链跳过清理；移除用 `-x`）
- 容量上限：`setfattr -n user.version.max_size_mb -v <MB> <dir-or-file>`（默认 1024 MB；清理时与数量/时间策略并行生效）

## 路径语法
- 列表：`<path>@versions` —— 在 readdir 中返回该文件的所有版本名（v1, v2, ...）。
- 指定版本：`<path>@vN` 或 `<path>@latest` —— 解析到该版本的元数据。
- 时间表达式：支持 `<path>@3s`、`<path>@2h`、`<path>@1d`、`<path>@1w`、`<path>@yesterday`、`<path>@today`（选择不晚于目标时间的最新版本）。

## 结构与接口要点（面向模块C）
- `file_metadata_t`：增加 `current_block_map`，版本/去重相关操作需持有 `version_lock`；`data_hash` 作为整体哈希占位。
- `data_block_t`：提供 `hash[32]` 与 `ref_count/ref_lock`，便于多版本共享块与后续压缩。
- `block_map_t`：记录 `version_id` 与 `block_index`，可用 `block_map_diff(old, new, diff_ht)` 获取差异块集合（key=块索引，value=新块或标记1表示删除）。
- `version_node_t`：包含 `parent_id`、`description`、`block_map`，链表为双向（head=最新，tail=最早）。

## 版本访问与列表
- `@versions` 输出带时间与描述，便于审计；解析版本可直接使用 `vN` 前缀部分。
- 时间表达式解析支持 `s/h/d/w/today/yesterday`，选择不晚于目标时间的最新版本。

## 版本创建策略
- 事件触发：unlink、rename 前自动建版。
- 定时策略：后台线程按 `version_time_interval` 定期为文件建版。
- 内容变化：写入后基于滚动哈希计算块差异，>10% 触发建版。
- 手动快照：xattr `user.version.create` 触发。

## 存储与清理
- 增量信息：每个版本记录 `diff_blocks`（变更块索引）和 `block_checksums`（每块校验和），保存差异块数据(`snapshots`)并通过 `parent` 继承未变更块。
- 清理策略：后台线程按 `version_clean_interval` 轮询，保留最近 `version_max_versions`（或 `version_retention_count`），删除超过 `version_expire_days`（或 `version_retention_days`）的旧版本；容量上限 `version_retention_size_mb` 超限时自尾向头删除，跳过 `important`/`pinned`。
- 引用计数职责：版本清理仅维护自身增量数据与父链修复，不修改底层块引用计数；块生命周期由写路径、去重/压缩模块管理。
- 重要版本标记：xattr `user.version.pinned` 为文件级重要标记，`user.version.important` 为版本级标记；清理时跳过。

## Bug 修复记录（v6.0 容量清理崩溃）
- 现象：容量上限测试（700KB v1 + 700KB v2，`user.version.max_size_mb=1`）触发清理时 FUSE 进程崩溃，挂载点变为 "Transport endpoint is not connected"。
- 根因：版本删除时对底层块做引用计数递减，导致仍被文件/去重引用的数据块被提前释放，后续读访问崩溃。
- 修复：
   - 移除版本层对块引用计数的增减，块生命周期交由写路径/去重模块管理。
   - 删除父版本时物化子版本未持有的数据并重定向 `parent`，确保继承链可读且无悬挂指针。
   - 保留策略容量分支稳定可用，手动/自动测试通过（`scripts/test_all.sh` 全部通过）。

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
