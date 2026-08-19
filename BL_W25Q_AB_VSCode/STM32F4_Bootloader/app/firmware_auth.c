/**
 * @file firmware_auth.c
 * @brief SHA-256与ECDSA-P256组成的固件真实性验证。
 *
 * ECDSA大整数运算交给只启用secp256r1验签能力的micro-ecc。本模块只描述本
 * 项目的签名对象和公钥选择，不再增加通用密码框架。
 */

#include <stddef.h>
#include <string.h>
#include "sha256.h"
#include "uECC.h"
#include "firmware_public_key.h"
#include "firmware_auth.h"

/* 固定域分隔符避免同一把密钥在其他数据格式中的签名被误当作固件签名。 */
static const uint8_t signature_domain[] = "ClockFW-XBIN-v2";

/**
 * @brief 计算与PC签名工具完全一致的规范化Header摘要。
 */
static void signature_digest(const magic_header_t *header,
                             uint8_t digest[SHA256_DIGEST_SIZE])
{
    static const uint8_t zero_signature[MAGIC_SIGNATURE_SIZE] = { 0 };
    static const uint8_t zero_crc32[sizeof(uint32_t)] = { 0 };
    const uint8_t *bytes = (const uint8_t *)header;
    sha256_context_t context;
    size_t signature_offset = offsetof(magic_header_t, signature);
    size_t after_signature = signature_offset + MAGIC_SIGNATURE_SIZE;
    size_t crc_offset = offsetof(magic_header_t, this_crc32);

    sha256_init(&context);
    sha256_update(&context, signature_domain, sizeof(signature_domain) - 1u);
    sha256_update(&context, bytes, (uint32_t)signature_offset);
    sha256_update(&context, zero_signature, sizeof(zero_signature));
    sha256_update(&context, bytes + after_signature,
                  (uint32_t)(crc_offset - after_signature));
    sha256_update(&context, zero_crc32, sizeof(zero_crc32));
    sha256_final(&context, digest);
}

bool firmware_authenticate_header(const magic_header_t *header)
{
    uint8_t digest[SHA256_DIGEST_SIZE];

    if (header->key_id != FIRMWARE_PUBLIC_KEY_ID)
        return false;

    signature_digest(header, digest);
    return uECC_verify(firmware_public_key, digest, sizeof(digest),
                       header->signature, uECC_secp256r1()) == 1;
}

bool firmware_hash_matches(const magic_header_t *header,
                           const uint8_t digest[MAGIC_SHA256_SIZE])
{
    return memcmp(header->image_sha256, digest, MAGIC_SHA256_SIZE) == 0;
}
