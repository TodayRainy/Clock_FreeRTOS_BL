/**
 * @file firmware_image_format.h
 * @brief Bootloader、应用与 XBIN 生成工具共同遵守的固件镜像格式。
 *
 * 外部 A/B 槽保存完整 XBIN：首256字节为本结构，应用数据从偏移4096开始。
 * 安装到 STM32 内部 Flash 后，Header 位于0x0800C000，应用位于0x08010000。
 *
 * v2格式保留了旧格式中地址、长度和CRC32字段的偏移，使旧开发Bootloader仍能
 * 完成一次迁移安装。v2 Bootloader则只接受SHA-256和ECDSA-P256签名均正确的
 * 镜像；CRC32仅用于快速发现传输误码，不再作为固件可信性的依据。
 */

#ifndef __FIRMWARE_IMAGE_FORMAT_H__
#define __FIRMWARE_IMAGE_FORMAT_H__

#include <stddef.h>
#include <stdint.h>

#define MAGIC_HEADER_MAGIC       0x4D414749u /**< 小端内存中为字符串 "MAGI"。 */
#define MAGIC_HEADER_FORMAT_V2   2u
#define MAGIC_HEADER_ADDR        0x0800C000u
#define MAGIC_HEADER_SIZE        256u
#define MAGIC_HEADER_AREA_SIZE   4096u
#define MAGIC_DATA_ADDRESS       0x08010000u
#define MAGIC_DATA_MAX_SIZE      0x00070000u
#define MAGIC_HEADER_TYPE_APP    1u
#define MAGIC_HASH_SHA256        1u
#define MAGIC_SIGNATURE_ECDSA_P256_SHA256 1u
#define MAGIC_SHA256_SIZE        32u
#define MAGIC_SIGNATURE_SIZE     64u
#define MAGIC_VERSION_SIZE       64u

/**
 * @brief XBIN v2的256字节固件描述头。
 *
 * `signature`采用SEC1未压缩坐标对应的ECDSA-P256原始格式：前32字节为r，
 * 后32字节为s。签名对象是固定域分隔符和一份规范化Header；规范化时将
 * `signature`与`this_crc32`字段清零。固件本体由Header内的`image_sha256`
 * 间接纳入签名。
 */
typedef struct
{
    uint32_t magic;
    uint32_t format_version;
    uint32_t header_size;
    uint32_t hash_algorithm;
    uint32_t signature_algorithm;
    uint32_t key_id;
    uint32_t security_version; /**< 防降级计数；与面向用户的版本字符串相互独立。 */
    uint32_t reserved1;
    uint32_t data_type;
    uint32_t data_offset;
    uint32_t data_address;
    uint32_t data_length;
    uint32_t data_crc32;
    uint8_t image_sha256[MAGIC_SHA256_SIZE];
    uint32_t reserved2[3];
    char version[MAGIC_VERSION_SIZE];
    uint8_t signature[MAGIC_SIGNATURE_SIZE];
    uint32_t reserved3[6];
    uint32_t this_address;
    uint32_t this_crc32;
} magic_header_t;

/* Python生成器和C#上位机按固定偏移读写；编译期直接锁住跨语言二进制契约。 */
_Static_assert(sizeof(magic_header_t) == MAGIC_HEADER_SIZE,
               "magic_header_t size changed");
_Static_assert(offsetof(magic_header_t, data_type) == 32u,
               "magic_header_t data fields moved");
_Static_assert(offsetof(magic_header_t, security_version) == 24u,
               "magic_header_t security version moved");
_Static_assert(offsetof(magic_header_t, image_sha256) == 52u,
               "magic_header_t SHA-256 moved");
_Static_assert(offsetof(magic_header_t, version) == 96u,
               "magic_header_t version moved");
_Static_assert(offsetof(magic_header_t, signature) == 160u,
               "magic_header_t signature moved");
_Static_assert(offsetof(magic_header_t, this_address) == 248u,
               "magic_header_t address moved");
_Static_assert(offsetof(magic_header_t, this_crc32) == 252u,
               "magic_header_t CRC moved");

#endif /* __FIRMWARE_IMAGE_FORMAT_H__ */
