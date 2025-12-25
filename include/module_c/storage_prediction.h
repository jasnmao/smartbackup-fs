#ifndef MODULE_C_STORAGE_PREDICTION_H
#define MODULE_C_STORAGE_PREDICTION_H

#include <stddef.h>
#include <stdint.h>
#include "module_c/storage_monitor_basic.h"

typedef storage_prediction_stats_t storage_prediction_t;

int predict_storage_usage(unsigned int horizon_days, storage_prediction_stats_t *out);
int predict_storage_usage_internal(unsigned int horizon_days, storage_prediction_stats_t *out);

#endif /* MODULE_C_STORAGE_PREDICTION_H */
