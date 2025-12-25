#ifndef MODULE_C_AGGREGATE_H
#define MODULE_C_AGGREGATE_H

#include "module_c/storage_prediction.h"
#include "module_c/storage_monitor_basic.h"

/* 统一暴露模块C的监控与预测接口 */
typedef struct metrics
{
    basic_storage_stats_t basic;
    cache_stats_t cache;
    storage_prediction_t prediction;
    compress_class_stats_t class_stats[SMB_FILE_CLASS_MAX];
} metrics_t;

int collect_metrics(metrics_t *out);
int export_metrics(metrics_t *out);

#endif /* MODULE_C_AGGREGATE_H */
