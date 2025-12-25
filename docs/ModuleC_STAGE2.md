# Module C Stage 2

Multi-level cache, flush, and monitoring enrichment atop Stage 1 dedup/compress.

## Delivered Scope
- Multi-level cache: L1 hash; L2 mmap slots with slot_ids + dirty flags; L3 on-disk directory cache with expiry/eviction.
- Dirty tracking & flush: L2 dirty flags, msync writeback, background flush thread (30s interval, 20% dirty threshold, manual wake).
- Prefetch/invalidate: sequential prefetch hook on reads; level-masked invalidation.
- Monitoring: cache hits/misses, usage, dirty slots; per-file-class compression aggregation; dedup/compress stats unchanged.

## Key Structures & APIs
- Caches: `l1_cache_t`, `l2_cache_t` (mmap, slots, dirty_flags), `l3_cache_t` (cache_dir, expiry, max_entries, usage).
- Lifecycle: `cache_system_init(l1_bytes, l2_bytes, l3_bytes)`, `cache_system_shutdown()`; L3 path `/tmp/smartbackupfs_l3`.
- Access: `cache_get_block`, `cache_put_block`, `cache_invalidate_block`, `cache_invalidate_block_level(block_id, level_mask)`.
- Flush/control: `cache_flush_l2_dirty`, background flush thread, `cache_flush_request` for manual trigger.
- Prefetch: `cache_prefetch(block_ids, count)` on sequential read path.
- L3 internals: `l3_cache_get/put`, eviction by capacity+entry cap, expiry check.

## Integration
- Reads: L1 → L2 → L3 fallback; L3 hits promote upward; sequential prefetch in metadata_manager.c.
- Writes: populate L1/L2 (dirty) and mirror to L3; invalidation clears all tiers.
- Background flush: dedicated thread; also callable from maintenance hooks.
- Telemetry: storage_monitor_basic exports cache stats (hits/misses/usage/dirty) and compression class bytes.

## Files
- Cache: include/module_c/cache.h, src/module_c/cache.c
- Monitoring: include/module_c/storage_monitor_basic.h, src/module_c/storage_monitor_basic.c
- Prefetch: src/module_a/metadata_manager.c
- Cleaner hook: src/module_b/version_manager.c

## Validation
- Regression: `./scripts/test.sh` (basic ops, dedup/compress, cross-dir dedup, repeated reads hitting L2/L3/prefetch). Use `./scripts/run.sh -d` for foreground logs.

## Notes / Limits
- L3 index not rebuilt on crash; shutdown cleanup is best-effort.
- No user-facing CLI for cache stats yet (available via monitor structs).
- Flush thread focuses on L2 msync; deeper L3 persistence can be extended later.
