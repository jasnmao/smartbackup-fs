#ifndef MODULE_C_BLOCK_SPLITTER_H
#define MODULE_C_BLOCK_SPLITTER_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    size_t min_block;  /* lower bound, e.g., 4KB */
    size_t max_block;  /* upper bound, e.g., 64KB */
    size_t default_block; /* fallback when no hints are available */
} block_splitter_config_t;

block_splitter_config_t block_splitter_default_config(void);
size_t block_splitter_pick_size(const block_splitter_config_t *cfg, size_t file_size_hint);

#endif /* MODULE_C_BLOCK_SPLITTER_H */
