#ifndef MODULE_C_STORAGE_MONITOR_BASIC_H
#define MODULE_C_STORAGE_MONITOR_BASIC_H

#include <stdint.h>
#include <stddef.h>

typedef struct
{
    uint64_t total_blocks;
    uint64_t unique_blocks;
    uint64_t dedup_saved_bytes;
    uint64_t compress_saved_bytes;
    uint64_t compress_input_bytes;
} basic_storage_stats_t;

typedef struct
{
    uint64_t l1_hits;
    uint64_t l1_misses;
    uint64_t l2_hits;
    uint64_t l2_misses;
    uint64_t l3_hits;
    uint64_t l3_misses;
    uint64_t l1_usage_bytes;
    uint64_t l2_usage_bytes;
    uint64_t l3_usage_bytes;
    uint64_t l2_dirty_slots;
    uint64_t l2_total_slots;
} cache_stats_t;

typedef struct
{
    double dedup_ratio;
    double compress_ratio;
} basic_storage_ratios_t;

typedef enum
{
    SMB_FILE_CLASS_UNKNOWN = 0,
    SMB_FILE_CLASS_TEXT,
    SMB_FILE_CLASS_COMPRESSED,
    SMB_FILE_CLASS_BINARY,
    SMB_FILE_CLASS_MAX
} smb_file_class_t;

typedef struct
{
    uint64_t raw_bytes;
    uint64_t compressed_bytes;
} compress_class_stats_t;

typedef struct
{
    uint64_t predicted_bytes;
    uint32_t horizon_days;
    uint32_t sample_count;
    double slope_bytes_per_day;
} storage_prediction_stats_t;

void smb_update_dedup_on_hit(size_t saved_bytes);
void smb_update_unique_block(void);
void smb_on_unique_block_removed(void);
void smb_update_compress(size_t raw_size, size_t compressed_size);
void smb_get_stats(basic_storage_stats_t *out);
void smb_get_ratios(basic_storage_ratios_t *out);
void smb_cache_get_stats(cache_stats_t *out);
void smb_cache_set_usage(uint64_t l1_bytes, uint64_t l2_bytes, uint64_t l3_bytes);
void smb_cache_update_hits(int level, int hit);
void smb_cache_set_l2_dirty(uint64_t dirty, uint64_t total);
void smb_set_prediction(const storage_prediction_stats_t *pred);
void smb_get_prediction(storage_prediction_stats_t *out);
void smb_update_compress_class(smb_file_class_t cls, size_t raw_size, size_t compressed_size);
void smb_get_compress_class_stats(compress_class_stats_t *out_array, size_t array_len);

#endif /* MODULE_C_STORAGE_MONITOR_BASIC_H */
