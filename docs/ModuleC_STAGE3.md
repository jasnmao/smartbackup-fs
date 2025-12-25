# Module C Stage 3

Predictive signals, load-aware compression, Module D hooks, and cache hygiene on top of Stage 2.

## Delivered Scope
- Storage prediction: linear regression on version history (time, file_size) to forecast future usage for a horizon; refreshed on version creation.
- Adaptive compression v2: file-type detection plus load-aware algo/level; high load (>1.5 normalized) skips compression, medium-high load lowers level or prefers lz4, text prefers zstd when load permits.
- Module D adapter: integrity and cache helpers plus stats export wiring.
- Cache hygiene: L3 expiry trimming invoked from flush thread and `multi_level_cache_manage()` to evict stale disk cache entries.

## Key Interfaces
- Prediction: `predict_storage_usage(horizon_days, storage_prediction_stats_t *out)`; `smb_set_prediction/smb_get_prediction` carry results.
- Monitoring: `md_get_current_storage_stats()` aggregates basic/cache/prediction snapshots.
- Adaptive compression: `ac_detect_file_type`, `ac_select_algorithm`, `ac_adaptive_compress_block` now factor `sm_normalized_load` and file type.
- Cache hygiene: `l3_trim_expired()` (internal) tied to flush/maintenance.
- Module D hooks: `md_get_block_hash`, `md_find_block_by_hash`, `md_cache_force_writeback`, `md_cache_prefetch_block`, `md_get_current_storage_stats`.

## Files
- Prediction: include/module_c/storage_prediction.h, src/module_c/storage_prediction.c, include/module_c/storage_monitor_basic.h, src/module_c/storage_monitor_basic.c, src/module_b/version_manager.c
- Adaptive compression: include/module_c/adaptive_compress.h, src/module_c/adaptive_compress.c, include/module_c/system_monitor.h, src/module_c/system_monitor.c
- Adapter: include/module_c/module_d_adapter.h, src/module_c/module_d_adapter.c
- Build: CMakeLists.txt

## Notes / Limits
- Prediction is best-effort linear; zeroed when samples are insufficient.
- Load normalization uses /proc/loadavg divided by CPU count; compression level remains clamped 1–9.
- L3 writeback behavior unchanged beyond expiry trimming; deeper persistence can be added later.
