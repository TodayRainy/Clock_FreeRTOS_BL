/**
 * @file firmware_decryption.c
 * @brief 使用Mbed TLS实现XBIN Formal的AES-256-GCM流式解密。
 */

#include "firmware_decryption.h"

#include <string.h>

#include "mbedtls/constant_time.h"
#include "firmware_encryption_key.h"

bool firmware_decryption_begin(firmware_decryption_t *context,
                               const magic_header_t *header)
{
    magic_header_t aad;

    if (header->encryption_key_id != FIRMWARE_ENCRYPTION_KEY_ID)
        return false;

    /* 与打包器一致：GCM先于Tag和签名生成，因此AAD中清零这三个字段。 */
    aad = *header;
    memset(aad.encryption_tag, 0, sizeof(aad.encryption_tag));
    memset(aad.signature, 0, sizeof(aad.signature));
    aad.header_crc32 = 0u;

    mbedtls_gcm_init(&context->gcm);
    if (mbedtls_gcm_setkey(&context->gcm, MBEDTLS_CIPHER_ID_AES,
                           firmware_encryption_key,
                           MAGIC_ENCRYPTION_KEY_SIZE * 8u) != 0 ||
        mbedtls_gcm_starts(&context->gcm, MBEDTLS_GCM_DECRYPT,
                           header->encryption_nonce,
                           sizeof(header->encryption_nonce)) != 0 ||
        mbedtls_gcm_update_ad(&context->gcm, (const uint8_t *)&aad,
                              sizeof(aad)) != 0)
    {
        mbedtls_gcm_free(&context->gcm);
        return false;
    }
    return true;
}

bool firmware_decryption_update(firmware_decryption_t *context,
                                uint8_t *data, size_t size, size_t capacity,
                                size_t *output_length)
{
    return mbedtls_gcm_update(&context->gcm, data, size, data, capacity,
                              output_length) == 0;
}

bool firmware_decryption_finish(firmware_decryption_t *context,
                                const magic_header_t *header,
                                uint8_t *output, size_t output_size,
                                size_t *output_length)
{
    uint8_t tag[MAGIC_ENCRYPTION_TAG_SIZE];
    bool authenticated =
        mbedtls_gcm_finish(&context->gcm, output, output_size,
                           output_length, tag, sizeof(tag)) == 0 &&
        mbedtls_ct_memcmp(tag, header->encryption_tag, sizeof(tag)) == 0;

    mbedtls_gcm_free(&context->gcm);
    return authenticated;
}

void firmware_decryption_abort(firmware_decryption_t *context)
{
    mbedtls_gcm_free(&context->gcm);
}
