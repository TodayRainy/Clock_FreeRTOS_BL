/**
 * @file firmware_decryption.h
 * @brief XBIN Formal AES-256-GCM流式解密接口。
 */

#ifndef CLOCK_F407_FIRMWARE_DECRYPTION_H
#define CLOCK_F407_FIRMWARE_DECRYPTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "mbedtls/gcm.h"
#include "firmware_image_format.h"

/** @brief 一次流式 AES-256-GCM 解密的 Mbed TLS 上下文。 */
typedef struct
{
    mbedtls_gcm_context gcm;
} firmware_decryption_t;

/**
 * @brief 使用Bootloader内置密钥开始解密一个XBIN Formal正文。
 * @param[out] context 解密上下文。
 * @param[in] header 已通过结构和ECDSA验证的Formal Header。
 * @return 密钥 ID 和 GCM 初始化均正确时返回 true。
 */
bool firmware_decryption_begin(firmware_decryption_t *context,
                               const magic_header_t *header);

/**
 * @brief 原地解密一段连续密文。
 * @param[in,out] context 已开始的解密上下文。
 * @param[in,out] data 输入密文和输出明文共用的缓冲区。
 * @param[in] size 本次密文字节数。
 * @param[in] capacity 缓冲区容量；最后一块需比 size 多至少 15 字节。
 * @param[out] output_length 本次产生的明文字节数。
 * @return Mbed TLS 接受本段数据时返回 true。
 */
bool firmware_decryption_update(firmware_decryption_t *context,
                                uint8_t *data, size_t size, size_t capacity,
                                size_t *output_length);

/**
 * @brief 结束解密并验证完整 16 字节 GCM Tag。
 * @param[in,out] context 已开始的解密上下文；返回前会清除内部密钥状态。
 * @param[in] header 带有预期GCM Tag的Formal Header。
 * @param[out] output 接收实现可能缓存的最后不足 16 字节明文。
 * @param[in] output_size output 的容量。
 * @param[out] output_length 最后产生的明文字节数。
 * @return Tag 完全一致时返回 true。
 */
bool firmware_decryption_finish(firmware_decryption_t *context,
                                const magic_header_t *header,
                                uint8_t *output, size_t output_size,
                                size_t *output_length);

/** @brief 放弃一次未完成的解密并清除上下文。 */
void firmware_decryption_abort(firmware_decryption_t *context);

#endif /* CLOCK_F407_FIRMWARE_DECRYPTION_H */
