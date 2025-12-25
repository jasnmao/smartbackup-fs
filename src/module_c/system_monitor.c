#include "module_c/system_monitor.h"

#include <stdio.h>
#include <unistd.h>

static long cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? n : 1;
}

double sm_loadavg_1m(void)
{
    FILE *fp = fopen("/proc/loadavg", "r");
    if (!fp)
        return -1.0;
    double one = -1.0;
    if (fscanf(fp, "%lf", &one) != 1)
        one = -1.0;
    fclose(fp);
    return one;
}

double sm_normalized_load(void)
{
    double one = sm_loadavg_1m();
    if (one < 0.0)
        return -1.0;
    double cores = (double)cpu_count();
    if (cores <= 0.0)
        cores = 1.0;
    return one / cores;
}

double get_system_load(void)
{
    return sm_normalized_load();
}
