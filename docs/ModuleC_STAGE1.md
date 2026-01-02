# Module C Stage 1

最小可行的存储优化循环:块大小提示、跨文件去重核心、基准压缩和基础统计管道。

## 交付范围
- 块大小助手:4KB–64KB的数据摄取选择器(`block_splitter_pick_size`)。
- 去重核心:基于SHA-256的索引和引用计数;跨文件复用;添加/删除/增/减引用的辅助函数。
- 自适应压缩v1:基于魔数跳过已压缩输入、简单算法选择、级别透传。
- 遥测v1:通过存储监控提供去重/压缩计数器和比率。
- 缓存基线:L1哈希;L2 mmap槽位已启动;L3占位符(此阶段尚未持久化/驱逐)。

## 关键API
- 块大小:`block_splitter_default_config()`, `block_splitter_pick_size(cfg, file_size_hint)`。
- 去重核心:`dedup_core_calculate_hash`, `dedup_core_find`, `dedup_core_index`, `dedup_core_remove`, `dedup_core_inc_ref`, `dedup_core_dec_ref`。
- 压缩:`ac_is_already_compressed`, `ac_select_algorithm`, `ac_adaptive_compress_block`。
- 统计:`smb_update_dedup_on_hit`, `smb_update_unique_block`, `smb_update_compress`, `smb_get_stats`, `smb_get_ratios`。

## 集成
- 去重管道(src/module_c/dedup.c)调用哈希/索引+自适应压缩,并在复用/节省空间时更新统计信息。
- 块大小提示在文件系统初始化期间应用(src/module_a/metadata_manager.c)。
- CMake连接新的Module C源文件和头文件。

## 验证
- 功能冒烟测试:`./scripts/test.sh`(基础FS操作,去重+LZ4/GZIP路径)。
- 手动测试:通过xattr切换去重/压缩;写入重复数据;检查`user.dedup.stats`。

## 已知限制
- L1之上的缓存是临时的;尚无分层驱逐。
- 压缩启发式规则是简单的魔数检查;无负载感知或按类型调整。
- 引用计数生命周期依赖释放路径;块释放之外无异步GC。
