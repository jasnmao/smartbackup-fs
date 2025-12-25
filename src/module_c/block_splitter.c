#include "module_c/block_splitter.h"

#include <stddef.h>

block_splitter_config_t block_splitter_default_config(void)
{
    block_splitter_config_t cfg;
    cfg.min_block = 4096;      /* 4KB */
    cfg.max_block = 65536;     /* 64KB */
    cfg.default_block = 4096;  /* default to 4KB */
    return cfg;
}

size_t block_splitter_pick_size(const block_splitter_config_t *cfg, size_t file_size_hint)
{
    block_splitter_config_t use = cfg ? *cfg : block_splitter_default_config();
    if (use.min_block == 0 || use.max_block < use.min_block)
    {
        use.min_block = 4096;
        use.max_block = 65536;
    }
    if (use.default_block < use.min_block || use.default_block > use.max_block)
        use.default_block = use.min_block;

    /* 简单启发：文件小于1MB用最小块，>64MB用最大块，中间线性插值 */
    if (file_size_hint == 0)
        return use.default_block;

    if (file_size_hint <= (1U << 20))
        return use.min_block;
    if (file_size_hint >= (64U << 20))
        return use.max_block;

    double ratio = (double)(file_size_hint - (1U << 20)) / (double)((64U << 20) - (1U << 20));
    size_t span = use.max_block - use.min_block;
    size_t suggested = use.min_block + (size_t)(ratio * (double)span);
    if (suggested < use.min_block)
        suggested = use.min_block;
    if (suggested > use.max_block)
        suggested = use.max_block;
    return suggested;
}
