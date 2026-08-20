/**
 * @file firmware_encryption_format.h
 * @brief XBIN v3 外层 AES-256-GCM 加密描述格式。
 *
 * XBIN v3 没有改变原有的 256 字节签名 Header。它把一份 64 字节加密描述放在
 * Header 区偏移 256 的位置，并把从偏移 4096 开始的应用数据替换为等长密文。
 * 因而现有版本号、发布顺序、ECDSA 签名和内部 Flash Header 均继续使用 v2
 * 契约；Bootloader 安装完成后只写入原有 Header 和解密后的应用。
 */

#ifndef __FIRMWARE_ENCRYPTION_FORMAT_H__
#define __FIRMWARE_ENCRYPTION_FORMAT_H__

#include <stddef.h>
#include <stdint.h>
#include "firmware_image_format.h"

#define XBIN_CONTAINER_FORMAT_PLAINTEXT       2u
#define XBIN_CONTAINER_FORMAT_ENCRYPTED       3u
#define XBIN_ENCRYPTION_MAGIC                 0x33434E45u /**< 小端内存中为 "ENC3"。 */
#define XBIN_ENCRYPTION_FORMAT_V1             1u
#define XBIN_ENCRYPTION_HEADER_OFFSET         MAGIC_HEADER_SIZE
#define XBIN_ENCRYPTION_HEADER_SIZE           64u
#define XBIN_ENCRYPTION_ALGORITHM_AES_256_GCM 1u
#define XBIN_ENCRYPTION_KEY_SIZE              32u
#define XBIN_ENCRYPTION_NONCE_SIZE            12u
#define XBIN_ENCRYPTION_TAG_SIZE              16u

/**
 * @brief 位于 XBIN Header 区偏移 256 的加密描述。
 *
 * GCM 的附加认证数据（AAD）由完整的 256 字节签名 Header 和本结构中 `tag`
 * 之前的 32 字节组成。这样算法、密钥 ID 和随机 Nonce 都受到认证，同时保留
 * 原有 ECDSA 签名对明文摘要和版本策略的最终授权。
 */
typedef struct
{
    uint32_t magic;
    uint32_t format_version;
    uint32_t header_size;
    uint32_t algorithm;
    uint32_t key_id;
    uint8_t nonce[XBIN_ENCRYPTION_NONCE_SIZE];
    uint8_t tag[XBIN_ENCRYPTION_TAG_SIZE];
    uint32_t reserved[3];
    uint32_t this_crc32;
} xbin_encryption_header_t;

#define XBIN_ENCRYPTION_AAD_PREFIX_SIZE \
    ((uint32_t)offsetof(xbin_encryption_header_t, tag))

_Static_assert(sizeof(xbin_encryption_header_t) == XBIN_ENCRYPTION_HEADER_SIZE,
               "xbin_encryption_header_t size changed");
_Static_assert(offsetof(xbin_encryption_header_t, tag) == 32u,
               "XBIN encryption AAD prefix moved");
_Static_assert(offsetof(xbin_encryption_header_t, this_crc32) == 60u,
               "XBIN encryption CRC moved");

#endif /* __FIRMWARE_ENCRYPTION_FORMAT_H__ */
