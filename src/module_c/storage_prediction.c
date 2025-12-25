#include "module_c/storage_prediction.h"

#include "version_manager.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int predict_storage_usage_internal(unsigned int horizon_days, storage_prediction_stats_t *out)
{
    const size_t max_samples = 2048;
    version_history_sample_t *samples = calloc(max_samples, sizeof(version_history_sample_t));
    if (!samples)
        return -ENOMEM;

    size_t count = version_manager_collect_samples(samples, max_samples);
    if (count == 0)
    {
        free(samples);
        if (out)
            memset(out, 0, sizeof(*out));
        return -ENOENT;
    }

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_x2 = 0.0;
    double origin = (double)samples[0].create_time;

    for (size_t i = 0; i < count; i++)
    {
        double x = ((double)samples[i].create_time - origin) / 86400.0; /* days */
        double y = (double)samples[i].file_size;
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    double slope = 0.0;
    double denom = (double)count * sum_x2 - sum_x * sum_x;
    if (count > 1 && denom > 0.0)
        slope = (((double)count * sum_xy) - (sum_x * sum_y)) / denom;

    double intercept = (double)count ? (sum_y - slope * sum_x) / (double)count : 0.0;
    double horizon_x = ((double)time(NULL) - origin) / 86400.0 + (double)horizon_days;
    double predicted = intercept + slope * horizon_x;
    if (predicted < 0.0)
        predicted = 0.0;

    storage_prediction_stats_t stats = {0};
    stats.predicted_bytes = (uint64_t)predicted;
    stats.horizon_days = horizon_days;
    stats.sample_count = (uint32_t)count;
    stats.slope_bytes_per_day = slope;

    smb_set_prediction(&stats);
    if (out)
        *out = stats;

    free(samples);
    return 0;
}
