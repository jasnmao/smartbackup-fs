# Module C 使用指南(去重、压缩、缓存、预测)

## 能力
- 块级去重:基于SHA-256索引、跨文件复用、引用计数和节省空间统计。
- 自适应压缩:LZ4/Zstd/GZIP(如存在zlib);跳过已压缩数据;负载感知级别/算法选择。
- 多级缓存:L1内存、L2 mmap文件缓存、L3磁盘缓存(带过期/驱逐);脏跟踪+后台刷新。
- 遥测:去重、压缩、缓存命中/使用/脏槽位、按文件类别压缩字节数和存储预测快照的统计信息。

## 运行时控制(xattr,全局作用域)
应用于挂载点下的任何可写路径:
- `user.dedup.enable`:`1`/`0`切换去重。
- `user.compression.algo`:`none` | `lz4` | `zstd` | `gzip`(如缺失zlib则回退为`none`)。
- `user.compression.level`:整数级别(限制在1-9);由负载感知逻辑动态调整。
- `user.compression.min_size`:要压缩的最小块大小(字节);默认1024,下限512。
- `user.dedup.stats`:只读`unique=<n>;saved=<bytes>;algo=<name>;dedup=on|off;comp=on|off`。

## 数据流
- 写入:如需则解压 → 修改 → `dedup_process_block_on_write`(哈希、去重命中检查) → 可选自适应压缩 → 索引块 → 更新统计。
- 读取:`read_block`自动解压;调用者始终看到明文字节。
- 版本控制:快照/差异在解压数据上操作;差异管道可复用`dedup_process_diff_blocks`对输出进行去重+压缩。
- 释放:`free_block`从去重索引中删除并更新唯一计数器。

## 缓存行为
- 查找顺序:L1 → L2 → L3;L3命中向上提升。顺序读取触发下一个块的`cache_prefetch`。
- 写入落入L1/L2(脏)并镜像到L3供冷数据复用;失效清除所有层级(`cache_invalidate_block_level`)。
- 后台刷新:线程msync L2脏槽位(30秒间隔、20%脏阈值)并执行L3过期修剪;通过`cache_flush_request`手动触发。

## 预测与监控
- 存储预测:`predict_storage_usage(horizon_days, storage_prediction_stats_t *out)`(版本历史的线性回归); `smb_set_prediction/smb_get_prediction`暴露上次结果。
- 指标聚合:`md_get_current_storage_stats()`打包基础、缓存和预测统计信息;压缩类别统计信息可通过storage_monitor_basic获取。

## 关键API
- 去重/压缩核心(include/dedup.h):`dedup_init/shutdown`, `block_compute_hash`, `dedup_find_duplicate`, `dedup_index_block`, `dedup_remove_block`, `dedup_process_block_on_write`, `dedup_process_diff_blocks`, `block_compress/block_decompress`, `dedup_update_config`, `dedup_format_stats`。
- 缓存(include/module_c/cache.h):`cache_system_init/shutdown`, `cache_get_block`, `cache_put_block`, `cache_invalidate_block`, `cache_invalidate_block_level`, `cache_prefetch`, `cache_flush_l2_dirty`, `cache_flush_request`。
- 自适应压缩(include/module_c/adaptive_compress.h):`ac_detect_file_type`, `ac_is_already_compressed`, `ac_select_algorithm`, `ac_adaptive_compress_block`。
- 预测/监控(include/module_c/storage_prediction.h, storage_monitor_basic.h, module_d_adapter.h):`predict_storage_usage`, `smb_set_prediction/smb_get_prediction`, `md_get_current_storage_stats`。

## 构建与依赖
- 必需:OpenSSL(SHA-256), LZ4, ZSTD。
- 可选:zlib启用GZIP;缺失 → GZIP请求表现为`none`。
- 缓存后备:L2 mmap文件`/tmp/smartbackupfs_l2.cache`,L3目录`/tmp/smartbackupfs_l3`在初始化时创建。

## 使用示例
```bash
# 启用去重 + LZ4压缩
setfattr -n user.dedup.enable -v 1 /tmp/smartbackup
setfattr -n user.compression.algo -v lz4 /tmp/smartbackup
setfattr -n user.compression.level -v 3 /tmp/smartbackup

# 检查去重统计信息
getfattr --only-values -n user.dedup.stats /tmp/smartbackup
```

## 测试
- 构建:`./scripts/build.sh`。
- 挂载:`./scripts/run.sh`( `-d`查看前台日志)。
- 回归测试:`./scripts/test_all.sh`覆盖去重/压缩/缓存/配置持久化、容量清理等场景;当前57/57通过。

## 注意与限制
- `saved_space`聚合去重复用+压缩节省(未分离)。
- 已去重的块不支持CoW;不支持同一去重块的并发写入者。
- L3索引在崩溃后不会重建;关闭清理是尽力而为的。
