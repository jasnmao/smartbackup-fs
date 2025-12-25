#include "module_c/storage_prediction.h"

/* Wrapper file to align interface location requirements. */
int predict_storage_usage(unsigned int horizon_days, storage_prediction_stats_t *out)
{
    return predict_storage_usage_internal(horizon_days, out);
}
