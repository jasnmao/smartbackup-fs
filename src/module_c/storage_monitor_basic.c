#include "module_c/storage_monitor_basic.h"

#include <string.h>

static basic_storage_stats_t g_stats;
static cache_stats_t g_cache_stats;
static storage_prediction_stats_t g_pred_stats;
static compress_class_stats_t g_class_stats[SMB_FILE_CLASS_MAX];

void smb_update_dedup_on_hit(size_t saved_bytes)
{
    g_stats.dedup_saved_bytes += saved_bytes;
}

void smb_update_unique_block(void)
{
    g_stats.total_blocks++;
    g_stats.unique_blocks++;
}

void smb_on_unique_block_removed(void)
{
    if (g_stats.total_blocks > 0)
        g_stats.total_blocks--;
    if (g_stats.unique_blocks > 0)
        g_stats.unique_blocks--;
}

void smb_update_compress(size_t raw_size, size_t compressed_size)
{
    g_stats.compress_input_bytes += raw_size;
    if (compressed_size < raw_size)
        g_stats.compress_saved_bytes += (raw_size - compressed_size);
}

void smb_update_compress_class(smb_file_class_t cls, size_t raw_size, size_t compressed_size)
{
    if (cls < 0 || cls >= SMB_FILE_CLASS_MAX)
        cls = SMB_FILE_CLASS_UNKNOWN;
    g_class_stats[cls].raw_bytes += raw_size;
    g_class_stats[cls].compressed_bytes += compressed_size;
}

void smb_get_stats(basic_storage_stats_t *out)
{
    if (!out)
        return;
    *out = g_stats;
}

void smb_get_ratios(basic_storage_ratios_t *out)
{
    if (!out)
        return;
    basic_storage_stats_t snap = g_stats;
    out->dedup_ratio = snap.total_blocks ? 1.0 - ((double)snap.unique_blocks / (double)snap.total_blocks) : 0.0;
    if (snap.compress_input_bytes > 0)
        out->compress_ratio = (double)snap.compress_saved_bytes / (double)snap.compress_input_bytes;
    else
        out->compress_ratio = 0.0;
}

void smb_cache_get_stats(cache_stats_t *out)
{
    if (!out)
        return;
    *out = g_cache_stats;
}

void smb_cache_set_usage(uint64_t l1_bytes, uint64_t l2_bytes, uint64_t l3_bytes)

{
    g_cache_stats.l1_usage_bytes = l1_bytes;
    g_cache_stats.l2_usage_bytes = l2_bytes;
    g_cache_stats.l3_usage_bytes = l3_bytes;
}

void smb_cache_update_hits(int level, int hit)
{
    switch (level)
    {
    case 1:
        if (hit)
            g_cache_stats.l1_hits++;
        else
            g_cache_stats.l1_misses++;
        break;
    case 2:
        if (hit)
            g_cache_stats.l2_hits++;
        else
            g_cache_stats.l2_misses++;
        break;
    case 3:
        if (hit)
            g_cache_stats.l3_hits++;
        else
            g_cache_stats.l3_misses++;
        break;
    default:
        break;
    }
}

void smb_cache_set_l2_dirty(uint64_t dirty, uint64_t total)
{
    g_cache_stats.l2_dirty_slots = dirty;
    g_cache_stats.l2_total_slots = total;
}

void smb_set_prediction(const storage_prediction_stats_t *pred)
{
    if (!pred)
        return;
    g_pred_stats = *pred;
}

void smb_get_prediction(storage_prediction_stats_t *out)
{
    if (!out)
        return;
    *out = g_pred_stats;
}

void smb_get_compress_class_stats(compress_class_stats_t *out_array, size_t array_len)
{
    if (!out_array || array_len == 0)
        return;
    size_t n = (array_len < SMB_FILE_CLASS_MAX) ? array_len : SMB_FILE_CLASS_MAX;
    for (size_t i = 0; i < n; i++)
        out_array[i] = g_class_stats[i];
}
