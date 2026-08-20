/**
 * @file firmware_auth.h
 * @brief XBIN Formal固件签名验证接口。
 */

#ifndef CLOCK_F407_FIRMWARE_AUTH_H
#define CLOCK_F407_FIRMWARE_AUTH_H

#include <stdbool.h>
#include <stdint.h>
#include "../../shared/firmware_image_format.h"

/**
 * @brief 验证Header携带的ECDSA-P256签名。
 *
 * Header格式和CRC32应先由magic_header_validate_data()检查。本函数使用构建时
 * 固化到Bootloader中的公钥，验证Header元数据及其中的固件SHA-256声明。
 *
 * @param[in] header 已通过结构检查的 Formal Header。
 * @return signing_key_id 和签名均正确时返回 true。
 */
bool firmware_authenticate_header(const magic_header_t *header);

/**
 * @brief 比较实际固件SHA-256与已签名Header中的摘要。
 *
 * @param[in] header 已通过 ECDSA 验证的 Formal Header。
 * @param[in] digest 对实际明文应用计算得到的 SHA-256。
 * @return 两份 32 字节摘要完全一致时返回 true。
 */
bool firmware_hash_matches(const magic_header_t *header,
                           const uint8_t digest[MAGIC_SHA256_SIZE]);

#endif /* CLOCK_F407_FIRMWARE_AUTH_H */
