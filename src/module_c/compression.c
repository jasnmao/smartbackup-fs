#include "module_c/compression.h"
#include "module_c/adaptive_compress.h"

int adaptive_compress(data_block_t *block, dedup_config_t *cfg)
{
    return ac_adaptive_compress_block(block, cfg);
}
