#ifndef MODULE_C_COMPRESSION_H
#define MODULE_C_COMPRESSION_H

#include "smartbackupfs.h"
#include "dedup.h"

int adaptive_compress(data_block_t *block, dedup_config_t *cfg);

#endif /* MODULE_C_COMPRESSION_H */
