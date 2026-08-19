/**
 * @file sha256.h
 * @brief 无动态内存的SHA-256增量计算接口。
 */

#ifndef __SHA256_H__
#define __SHA256_H__

#include <stdint.h>

#define SHA256_DIGEST_SIZE 32u
#define SHA256_BLOCK_SIZE  64u

/**
 * @brief 一次SHA-256计算的上下文。
 */
typedef struct
{
    uint32_t state[8];
    uint64_t total_length;
    uint8_t buffer[SHA256_BLOCK_SIZE];
    uint32_t buffer_length;
} sha256_context_t;

/** @brief 初始化一轮SHA-256计算。 */
void sha256_init(sha256_context_t *context);

/**
 * @brief 向当前SHA-256计算追加数据。
 * @param[in,out] context 哈希上下文。
 * @param[in] data 输入数据。
 * @param[in] length 输入长度，允许为0。
 */
void sha256_update(sha256_context_t *context, const uint8_t *data,
                   uint32_t length);

/**
 * @brief 结束计算并输出32字节摘要。
 */
void sha256_final(sha256_context_t *context,
                  uint8_t digest[SHA256_DIGEST_SIZE]);

/**
 * @brief 一次性计算一段连续内存的SHA-256。
 */
void sha256(const uint8_t *data, uint32_t length,
            uint8_t digest[SHA256_DIGEST_SIZE]);

#endif /* __SHA256_H__ */
