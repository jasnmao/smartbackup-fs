#ifndef MODULE_C_ADAPTIVE_COMPRESS_H
#define MODULE_C_ADAPTIVE_COMPRESS_H

#include <stdbool.h>
#include "smartbackupfs.h"
#include "dedup.h"

typedef enum
{
	AC_FILE_UNKNOWN = 0,
	AC_FILE_TEXT,
	AC_FILE_COMPRESSED,
	AC_FILE_BINARY
} ac_file_type_t;

/* 文件类型/魔数检测 */
ac_file_type_t ac_detect_file_type(const data_block_t *block);
bool ac_is_already_compressed(const data_block_t *block);
compression_algorithm_t ac_select_algorithm(const data_block_t *block, const dedup_config_t *cfg);
int ac_adaptive_compress_block(data_block_t *block, dedup_config_t *cfg);

#endif /* MODULE_C_ADAPTIVE_COMPRESS_H */
