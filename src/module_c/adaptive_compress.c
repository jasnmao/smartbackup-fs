#include "module_c/adaptive_compress.h"

#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "module_c/system_monitor.h"
#include "module_c/storage_monitor_basic.h"

static int clamp_level(int level)
{
    if (level < 1)
        return 1;
    if (level > 9)
        return 9;
    return level;
}

static bool has_magic(const data_block_t *block, const unsigned char *magic, size_t len)
{
    if (!block || !block->data || block->size < len)
        return false;
    return memcmp(block->data, magic, len) == 0;
}

static bool looks_text(const data_block_t *block)
{
    if (!block || !block->data || block->size == 0)
        return false;
    size_t printable = 0;
    size_t sample = block->size < 4096 ? block->size : 4096;
    for (size_t i = 0; i < sample; i++)
    {
        unsigned char c = (unsigned char)block->data[i];
        if (c == '\n' || c == '\r' || c == '\t' || (isprint(c) && c < 0x7F))
            printable++;
    }
    double ratio = sample ? ((double)printable / (double)sample) : 0.0;
    return ratio > 0.8;
}

ac_file_type_t ac_detect_file_type(const data_block_t *block)
{
    if (!block || !block->data)
        return AC_FILE_UNKNOWN;

    if (ac_is_already_compressed(block))
        return AC_FILE_COMPRESSED;

    if (looks_text(block))
        return AC_FILE_TEXT;

    return AC_FILE_BINARY;
}

bool ac_is_already_compressed(const data_block_t *block)
{
    if (!block || !block->data)
        return false;
    /* 常见压缩/归档魔数检测 */
    const unsigned char gzip_magic[] = {0x1F, 0x8B};
    const unsigned char zip_magic[] = {0x50, 0x4B, 0x03, 0x04};
    const unsigned char zlib_magic[] = {0x78, 0x9C};
    const unsigned char zstd_magic[] = {0x28, 0xB5, 0x2F, 0xFD};
    const unsigned char lz4_magic[] = {0x04, 0x22, 0x4D, 0x18};
    if (has_magic(block, gzip_magic, sizeof(gzip_magic)))
        return true;
    if (has_magic(block, zip_magic, sizeof(zip_magic)))
        return true;
    if (has_magic(block, zlib_magic, sizeof(zlib_magic)))
        return true;
    if (has_magic(block, zstd_magic, sizeof(zstd_magic)))
        return true;
    if (has_magic(block, lz4_magic, sizeof(lz4_magic)))
        return true;
    return false;
}

compression_algorithm_t ac_select_algorithm(const data_block_t *block, const dedup_config_t *cfg)
{
    double norm_load = sm_normalized_load();
    ac_file_type_t type = ac_detect_file_type(block);

    if (type == AC_FILE_COMPRESSED)
        return COMPRESSION_NONE;

    compression_algorithm_t preferred = COMPRESSION_LZ4;
    if (type == AC_FILE_TEXT)
        preferred = COMPRESSION_ZSTD;

    if (cfg && cfg->algo != COMPRESSION_NONE)
        preferred = cfg->algo;

    /* 在高负载下动态降级压缩算法；极高负载直接跳过压缩 */
    if (norm_load > 1.5)
        return COMPRESSION_NONE;
    if (norm_load > 1.2 && preferred == COMPRESSION_ZSTD)
        preferred = COMPRESSION_LZ4;

    return preferred;
}

int ac_adaptive_compress_block(data_block_t *block, dedup_config_t *cfg)
{
    if (!block || !cfg)
        return -1;
    size_t raw_before = block->size;
    ac_file_type_t type = ac_detect_file_type(block);
    double norm_load = sm_normalized_load();
    compression_algorithm_t choice = ac_select_algorithm(block, cfg);
    block->file_type = (uint8_t)type;

    int level = cfg->compression_level;
    if (norm_load >= 0.0)
    {
        if (norm_load > 1.5)
            level -= 3;
        else if (norm_load > 1.0)
            level -= 2;
        else if (norm_load < 0.5)
            level += 1;
    }
    level = clamp_level(level);

    dedup_set_compression(cfg, choice, level);
    if (choice == COMPRESSION_NONE)
    {
        block->compressed_size = 0;
        block->compression = COMPRESSION_NONE;
        smb_update_compress_class((smb_file_class_t)type, raw_before, raw_before);
        return 0;
    }

    int rc = block_compress(block, cfg);
    if (rc == 0)
    {
        size_t effective = (block->compressed_size > 0) ? block->compressed_size : block->size;
        smb_update_compress_class((smb_file_class_t)type, raw_before, effective);
    }
    return rc;
}
