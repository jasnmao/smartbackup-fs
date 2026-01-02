# Module C Stage 2

在Stage 1去重/压缩基础之上的多级缓存、刷新和监控增强。

## 交付范围
- 多级缓存:L1哈希;L2 mmap槽位(带slot_id +脏标志);L3磁盘目录缓存(带过期/驱逐)。
- 脏跟踪与刷新:L2脏标志、msync写回、后台刷新线程(30秒间隔、20%脏阈值、手动唤醒)。
- 预取/失效:读取时的顺序预取钩子;按级别掩码失效。
- 监控:缓存命中/未命中、使用情况、脏槽位;按文件类别的压缩聚合;去重/压缩统计信息保持不变。

## 关键结构与API
- 缓存:`l1_cache_t`, `l2_cache_t`(mmap、slots、dirty_flags), `l3_cache_t`(cache_dir、expiry、max_entries、usage)。
- 生命周期:`cache_system_init(l1_bytes, l2_bytes, l3_bytes)`, `cache_system_shutdown()`; L3路径为`/tmp/smartbackupfs_l3`。
- 访问:`cache_get_block`, `cache_put_block`, `cache_invalidate_block`, `cache_invalidate_block_level(block_id, level_mask)`。
- 刷新/控制:`cache_flush_l2_dirty`,后台刷新线程,`cache_flush_request`用于手动触发。
- 预取:`cache_prefetch(block_ids, count)`在顺序读取路径上。
- L3内部实现:`l3_cache_get/put`,按容量+条目上限驱逐,过期检查。

## 集成
- 读取:L1 → L2 → L3回退;L3命中向上提升;metadata_manager.c中的顺序预取。
- 写入:填充L1/L2(脏)并镜像到L3;失效清除所有层级。
- 后台刷新:专用线程;也可从维护钩子调用。
- 遥测:storage_monitor_basic导出缓存统计信息(命中/未命中/使用/脏)和压缩类别字节数。

## 文件
- 缓存:include/module_c/cache.h, src/module_c/cache.c
- 监控:include/module_c/storage_monitor_basic.h, src/module_c/storage_monitor_basic.c
- 预取:src/module_a/metadata_manager.c
- 清理钩子:src/module_b/version_manager.c

## 验证
- 回归测试:`./scripts/test.sh`(基本操作、去重/压缩、跨目录去重、重复读取命中L2/L3/预取)。使用`./scripts/run.sh -d`查看前台日志。

## 注意/限制
- L3索引在崩溃时不会重建;关闭清理是尽力而为的。
- 尚无面向用户的缓存统计CLI(可通过监控结构获取)。
- 刷新线程专注于L2 msync;更深层的L3持久化可在后续扩展。
