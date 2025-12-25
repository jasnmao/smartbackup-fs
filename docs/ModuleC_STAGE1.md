# Module C Stage 1

Minimum viable storage-optimization loop: block sizing hints, cross-file dedup core, baseline compression, and basic stats plumbing.

## Delivered Scope
- Block sizing helper: 4KB–64KB picker for ingestion (`block_splitter_pick_size`).
- Dedup core: SHA-256 indexing with refcounts; cross-file reuse; helpers to add/remove/inc/dec refs.
- Adaptive compression v1: magic-based skip for already-compressed inputs, simple algo choice, level passthrough.
- Telemetry v1: dedup/compress counters and ratios via storage monitor.
- Cache baseline: L1 hash; L2 mmap slots brought up; L3 placeholder (not yet persisted/evicted in this stage).

## Key APIs
- Block sizing: `block_splitter_default_config()`, `block_splitter_pick_size(cfg, file_size_hint)`.
- Dedup core: `dedup_core_calculate_hash`, `dedup_core_find`, `dedup_core_index`, `dedup_core_remove`, `dedup_core_inc_ref`, `dedup_core_dec_ref`.
- Compression: `ac_is_already_compressed`, `ac_select_algorithm`, `ac_adaptive_compress_block`.
- Stats: `smb_update_dedup_on_hit`, `smb_update_unique_block`, `smb_update_compress`, `smb_get_stats`, `smb_get_ratios`.

## Integration
- Dedup pipeline (src/module_c/dedup.c) invokes hash/index + adaptive compression and updates stats on reuse/space saved.
- Block size hint applied during fs init (src/module_a/metadata_manager.c).
- CMake wires new Module C sources and headers.

## Validation
- Functional smoke: `./scripts/test.sh` (basic FS ops, dedup+LZ4/GZIP paths).
- Manual: toggle xattrs for dedup/compress; write duplicates; inspect `user.dedup.stats`.

## Known Limits
- Caching beyond L1 is provisional; no tiered eviction yet.
- Compression heuristics are simple magic checks; no load awareness or per-type tuning.
- Refcount lifecycle relies on free paths; no async GC beyond block free.
