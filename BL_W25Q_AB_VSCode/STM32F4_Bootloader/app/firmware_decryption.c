/**
 * @file firmware_decryption.c
 * @brief 使用 Mbed TLS 实现 XBIN v3 的 AES-256-GCM 流式解密。
 */

#include "mbedtls/constant_time.h"
#include "firmware_encryption_key.h"
#include "firmware_decryption.h"

bool firmware_decryption_begin(firmware_decryption_t *context,
                               const magic_header_t *header,
                               const xbin_encryption_header_t *encryption)
{
    if (encryption->key_id != FIRMWARE_ENCRYPTION_KEY_ID)
        return false;

    mbedtls_gcm_init(&context->gcm);
    if (mbedtls_gcm_setkey(&context->gcm, MBEDTLS_CIPHER_ID_AES,
                           firmware_encryption_key,
                           XBIN_ENCRYPTION_KEY_SIZE * 8u) != 0 ||
        mbedtls_gcm_starts(&context->gcm, MBEDTLS_GCM_DECRYPT,
                           encryption->nonce, sizeof(encryption->nonce)) != 0 ||
        mbedtls_gcm_update_ad(&context->gcm, (const uint8_t *)header,
                              sizeof(*header)) != 0 ||
        mbedtls_gcm_update_ad(&context->gcm, (const uint8_t *)encryption,
                              XBIN_ENCRYPTION_AAD_PREFIX_SIZE) != 0)
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
                                const xbin_encryption_header_t *encryption,
                                uint8_t *output, size_t output_size,
                                size_t *output_length)
{
    uint8_t tag[XBIN_ENCRYPTION_TAG_SIZE];
    bool ok = mbedtls_gcm_finish(&context->gcm, output, output_size,
                                 output_length, tag, sizeof(tag)) == 0 &&
              mbedtls_ct_memcmp(tag, encryption->tag, sizeof(tag)) == 0;

    mbedtls_gcm_free(&context->gcm);
    return ok;
}

void firmware_decryption_abort(firmware_decryption_t *context)
{
    mbedtls_gcm_free(&context->gcm);
}
