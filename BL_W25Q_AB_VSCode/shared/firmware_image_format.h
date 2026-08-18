/**
 * @file firmware_image_format.h
 * @brief Bootloader、应用与 XBIN 生成工具共同遵守的固件镜像格式。
 *
 * 外部 A/B 槽保存完整 XBIN：首256字节为本结构，应用数据从偏移4096开始。
 * 安装到 STM32 内部 Flash 后，Header 位于0x0800C000，应用位于0x08010000。
 */

#ifndef __FIRMWARE_IMAGE_FORMAT_H__
#define __FIRMWARE_IMAGE_FORMAT_H__

#include <stdint.h>

#define MAGIC_HEADER_MAGIC       0x4D414749u /**< 小端内存中为字符串 "MAGI"。 */
#define MAGIC_HEADER_ADDR        0x0800C000u
#define MAGIC_HEADER_SIZE        256u
#define MAGIC_HEADER_AREA_SIZE   4096u
#define MAGIC_DATA_ADDRESS       0x08010000u
#define MAGIC_DATA_MAX_SIZE      0x00070000u
#define MAGIC_HEADER_TYPE_APP    1u

/**
 * @brief XBIN 的256字节固件描述头。
 */
typedef struct
{
    uint32_t magic;
    uint32_t bitmask;
    uint32_t reserved1[6];
    uint32_t data_type;
    uint32_t data_offset;
    uint32_t data_address;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t reserved2[11];
    char version[128];
    uint32_t reserved3[6];
    uint32_t this_address;
    uint32_t this_crc32;
} magic_header_t;

typedef char magic_header_must_be_256_bytes[
    (sizeof(magic_header_t) == MAGIC_HEADER_SIZE) ? 1 : -1];

#endif /* __FIRMWARE_IMAGE_FORMAT_H__ */
