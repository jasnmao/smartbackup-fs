# Module C Usage Guide (Dedup, Compression, Cache, Prediction)

## Capabilities
- Block-level deduplication with SHA-256 index, cross-file reuse, refcounts, and saved-space accounting.
- Adaptive compression: LZ4/Zstd/GZIP (if zlib present); skips already-compressed data; load-aware level/algorithm selection.
- Multi-level cache: L1 memory, L2 mmap file cache, L3 on-disk cache with expiry/eviction; dirty tracking + background flush.
- Telemetry: stats for dedup, compression, cache hits/usage/dirty slots, per-file-class compression bytes, and storage prediction snapshot.

## Runtime Controls (xattr, global scope)
Apply on any writable path under the mount:
- `user.dedup.enable`: `1`/`0` toggle deduplication.
- `user.compression.algo`: `none` | `lz4` | `zstd` | `gzip` (falls back to `none` if zlib missing).
- `user.compression.level`: integer level (clamped 1–9); adjusted dynamically by load-aware logic.
- `user.compression.min_size`: minimum block size to compress (bytes); default 1024, floor 512.
- `user.dedup.stats`: read-only `unique=<n>;saved=<bytes>;algo=<name>;dedup=on|off;comp=on|off`.

## Data Flow
- Write: inflate if needed → mutate → `dedup_process_block_on_write` (hash, dedup hit check) → optional adaptive compression → index block → update stats.
- Read: `read_block` auto-decompresses; callers always see plain bytes.
- Versioning: snapshots/diffs operate on decompressed data; diff pipeline can reuse `dedup_process_diff_blocks` for dedup+compress on outputs.
- Free: `free_block` removes from dedup index and updates unique counters.

## Cache Behavior
- Lookup order: L1 → L2 → L3; L3 hits promote upward. Sequential reads trigger `cache_prefetch` of the next block.
- Writes land in L1/L2 (dirty) and mirror to L3 for cold reuse; invalidation clears all tiers (`cache_invalidate_block_level`).
- Background flush: thread msyncs L2 dirty slots (30s cadence, 20% dirty threshold) and performs L3 expiry trimming; manual trigger via `cache_flush_request`.

## Prediction & Monitoring
- Storage prediction: `predict_storage_usage(horizon_days, storage_prediction_stats_t *out)` (linear regression on version history); `smb_set_prediction/smb_get_prediction` expose last result.
- Metrics aggregation: `md_get_current_storage_stats()` bundles basic, cache, and prediction stats; compression class stats available via storage_monitor_basic.

## Key APIs
- Dedup/compress core (include/dedup.h): `dedup_init/shutdown`, `block_compute_hash`, `dedup_find_duplicate`, `dedup_index_block`, `dedup_remove_block`, `dedup_process_block_on_write`, `dedup_process_diff_blocks`, `block_compress/block_decompress`, `dedup_update_config`, `dedup_format_stats`.
- Cache (include/module_c/cache.h): `cache_system_init/shutdown`, `cache_get_block`, `cache_put_block`, `cache_invalidate_block`, `cache_invalidate_block_level`, `cache_prefetch`, `cache_flush_l2_dirty`, `cache_flush_request`.
- Adaptive compression (include/module_c/adaptive_compress.h): `ac_detect_file_type`, `ac_is_already_compressed`, `ac_select_algorithm`, `ac_adaptive_compress_block`.
- Prediction/monitor (include/module_c/storage_prediction.h, storage_monitor_basic.h, module_d_adapter.h): `predict_storage_usage`, `smb_set_prediction/smb_get_prediction`, `md_get_current_storage_stats`.

## Build & Dependencies
- Required: OpenSSL (SHA-256), LZ4, ZSTD.
- Optional: zlib enables GZIP; absent -> GZIP requests behave as `none`.
- Cache backing: L2 mmap file `/tmp/smartbackupfs_l2.cache`, L3 directory `/tmp/smartbackupfs_l3` created on init.

## Usage Examples
```bash
# Enable dedup + LZ4 compression
setfattr -n user.dedup.enable -v 1 /tmp/smartbackup
setfattr -n user.compression.algo -v lz4 /tmp/smartbackup
setfattr -n user.compression.level -v 3 /tmp/smartbackup

# Inspect dedup stats
getfattr --only-values -n user.dedup.stats /tmp/smartbackup
```

## Testing
- Build: `./scripts/build.sh`.
- Mount: `./scripts/run.sh` (`-d` for foreground logs).
- Regression: `./scripts/test_all.sh` covers dedup/压缩/缓存/配置持久化、容量清理等场景；当前 57/57 通过。

## Notes & Limits
- `saved_space` aggregates dedup reuse + compression savings (not separated).
- Dedupbed blocks are not CoW; concurrent writers to the same deduped block are not supported.
- L3 index is not rebuilt after crashes; shutdown cleanup is best-effort.
