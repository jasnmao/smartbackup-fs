# Module C Stage 3

在Stage 2基础之上的预测信号、负载感知压缩、Module D钩子和缓存卫生清理。

## 交付范围
- 存储预测:基于版本历史(时间、file_size)的线性回归,预测未来使用水平;在创建版本时刷新。
- 自适应压缩v2:文件类型检测+负载感知算法/级别;高负载(>1.5归一化)跳过压缩,中高负载降低级别或优先使用lz4,文本在负载允许时优先使用zstd。
- Module D适配器:完整性和缓存辅助函数+统计导出连线。
- 缓存卫生清理:从刷新线程和`multi_level_cache_manage()`调用L3过期修剪,驱逐陈旧的磁盘缓存条目。

## 关键接口
- 预测:`predict_storage_usage(horizon_days, storage_prediction_stats_t *out)`; `smb_set_prediction/smb_get_prediction`承载结果。
- 监控:`md_get_current_storage_stats()`聚合基础/缓存/预测快照。
- 自适应压缩:`ac_detect_file_type`, `ac_select_algorithm`, `ac_adaptive_compress_block`现在考虑`sm_normalized_load`和文件类型。
- 缓存卫生清理:`l3_trim_expired()`(内部函数)绑定到刷新/维护。
- Module D钩子:`md_get_block_hash`, `md_find_block_by_hash`, `md_cache_force_writeback`, `md_cache_prefetch_block`, `md_get_current_storage_stats`。

## 文件
- 预测:include/module_c/storage_prediction.h, src/module_c/storage_prediction.c, include/module_c/storage_monitor_basic.h, src/module_c/storage_monitor_basic.c, src/module_b/version_manager.c
- 自适应压缩:include/module_c/adaptive_compress.h, src/module_c/adaptive_compress.c, include/module_c/system_monitor.h, src/module_c/system_monitor.c
- 适配器:include/module_c/module_d_adapter.h, src/module_c/module_d_adapter.c
- 构建:CMakeLists.txt

## 注意/限制
- 预测是尽力而为的线性回归;样本不足时归零。
- 负载归一化使用/proc/loadavg除以CPU数;压缩级别仍限制在1-9。
- L3写回行为除过期修剪外保持不变;更深层的持久化可在后续添加。
