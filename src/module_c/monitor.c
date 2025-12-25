#include "module_c.h"
#include <string.h>

int collect_metrics(metrics_t *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    smb_get_stats(&out->basic);
    smb_cache_get_stats(&out->cache);
    smb_get_prediction(&out->prediction);
    smb_get_compress_class_stats(out->class_stats, SMB_FILE_CLASS_MAX);
    return 0;
}

int export_metrics(metrics_t *out)
{
    return collect_metrics(out);
}
