/**
 * @file firmware_auth.h
 * @brief XBIN Formal固件签名验证接口。
 */

#ifndef __FIRMWARE_AUTH_H__
#define __FIRMWARE_AUTH_H__

#include <stdbool.h>
#include <stdint.h>
#include "../../shared/firmware_image_format.h"

/**
 * @brief 验证Header携带的ECDSA-P256签名。
 *
 * Header格式和CRC32应先由magic_header_validate_data()检查。本函数使用构建时
 * 固化到Bootloader中的公钥，验证Header元数据及其中的固件SHA-256声明。
 *
 * @return signing_key_id和签名均正确时返回true。
 */
bool firmware_authenticate_header(const magic_header_t *header);

/**
 * @brief 比较实际固件SHA-256与已签名Header中的摘要。
 */
bool firmware_hash_matches(const magic_header_t *header,
                           const uint8_t digest[MAGIC_SHA256_SIZE]);

#endif /* __FIRMWARE_AUTH_H__ */
