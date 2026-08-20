/**
 * @file firmware_image_format.h
 * @brief XBIN Formal、Bootloader、WeatherClock和上位机共用的固件格式。
 *
 * Formal格式只保留一种安全形态：ECDSA-P256签名和AES-256-GCM加密均为必选。
 * 外部W25Q槽保存完整加密XBIN；安装到STM32内部Flash后，Header保持不变，
 * 应用正文则是已经通过GCM认证的明文。
 *
 * Header占320字节，但仍位于原有4 KiB Header区内。应用地址继续使用
 * 0x08010000，因此本次格式收口不会改变Bootloader或应用的链接布局。
 */

#ifndef __FIRMWARE_IMAGE_FORMAT_H__
#define __FIRMWARE_IMAGE_FORMAT_H__

#include <stddef.h>
#include <stdint.h>

#define MAGIC_HEADER_MAGIC             0x4D524F46u /**< 小端内存中为"FORM"。 */
#define MAGIC_HEADER_FORMAT_FORMAL     1u
#define MAGIC_HEADER_ADDR              0x0800C000u
#define MAGIC_HEADER_SIZE              320u
#define MAGIC_HEADER_AREA_SIZE         4096u
#define MAGIC_DATA_ADDRESS             0x08010000u
#define MAGIC_DATA_MAX_SIZE            0x00070000u
#define MAGIC_HEADER_TYPE_APP          1u

#define MAGIC_HASH_SHA256              1u
#define MAGIC_SIGNATURE_ECDSA_P256_SHA256 1u
#define MAGIC_ENCRYPTION_AES_256_GCM   1u

#define MAGIC_SHA256_SIZE              32u
#define MAGIC_ENCRYPTION_KEY_SIZE      32u
#define MAGIC_ENCRYPTION_NONCE_SIZE    12u
#define MAGIC_ENCRYPTION_TAG_SIZE      16u
#define MAGIC_SIGNATURE_SIZE           64u
#define MAGIC_VERSION_SIZE             64u

/**
 * @brief XBIN Formal revision 1的统一320字节Header。
 *
 * 打包顺序固定为：先填写元数据和随机Nonce；以Tag、签名和Header CRC清零后的
 * Header作为GCM AAD并加密应用；写入Tag；对仅清零签名和Header CRC的Header
 * 做ECDSA签名；最后计算Header CRC32。由此版本、地址、摘要、Nonce和GCM Tag
 * 都受到签名保护，同时不会形成Tag与签名互相依赖的循环。
 *
 * `signature`采用64字节r||s格式。`header_crc32`只负责快速发现传输损坏，
 * 固件授权由ECDSA决定，正文真实性由GCM Tag和已签名的SHA-256共同决定。
 */
typedef struct
{
    uint32_t magic;
    uint32_t format_revision;
    uint32_t header_size;
    uint32_t data_type;
    uint32_t data_offset;
    uint32_t data_address;
    uint32_t data_length;
    uint32_t data_crc32;
    uint32_t security_version; /**< 安全修复形成的单调防降级下限。 */
    uint32_t release_sequence; /**< 普通发布顺序；自动OTA只接受更大的值。 */
    uint32_t hash_algorithm;
    uint32_t signature_algorithm;
    uint32_t encryption_algorithm;
    uint32_t signing_key_id;
    uint32_t encryption_key_id;
    uint32_t reserved0;
    uint8_t image_sha256[MAGIC_SHA256_SIZE];
    uint8_t encryption_nonce[MAGIC_ENCRYPTION_NONCE_SIZE];
    uint8_t encryption_tag[MAGIC_ENCRYPTION_TAG_SIZE];
    char version[MAGIC_VERSION_SIZE];
    uint8_t signature[MAGIC_SIGNATURE_SIZE];
    uint32_t reserved[16];
    uint32_t header_crc32;
} magic_header_t;

/* Python和C#按固定偏移访问；在编译期锁住跨语言二进制契约。 */
_Static_assert(sizeof(magic_header_t) == MAGIC_HEADER_SIZE,
               "magic_header_t size changed");
_Static_assert(offsetof(magic_header_t, data_length) == 24u,
               "data fields moved");
_Static_assert(offsetof(magic_header_t, security_version) == 32u,
               "security version moved");
_Static_assert(offsetof(magic_header_t, release_sequence) == 36u,
               "release sequence moved");
_Static_assert(offsetof(magic_header_t, image_sha256) == 64u,
               "SHA-256 moved");
_Static_assert(offsetof(magic_header_t, encryption_nonce) == 96u,
               "AES-GCM nonce moved");
_Static_assert(offsetof(magic_header_t, encryption_tag) == 108u,
               "AES-GCM tag moved");
_Static_assert(offsetof(magic_header_t, version) == 124u,
               "version moved");
_Static_assert(offsetof(magic_header_t, signature) == 188u,
               "signature moved");
_Static_assert(offsetof(magic_header_t, header_crc32) == 316u,
               "Header CRC moved");

#endif /* __FIRMWARE_IMAGE_FORMAT_H__ */
