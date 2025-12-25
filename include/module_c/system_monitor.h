#ifndef MODULE_C_SYSTEM_MONITOR_H
#define MODULE_C_SYSTEM_MONITOR_H

/* 简易系统负载检测（1分钟平均值） */
double sm_loadavg_1m(void);

/* 按CPU核心数归一化的负载值，<0 表示读取失败 */
double sm_normalized_load(void);

/* 提示词兼容接口 */
double get_system_load(void);

#endif /* MODULE_C_SYSTEM_MONITOR_H */
