/**
 * @file magic_header.c
 * @brief 固件Magic Header的格式、布局和CRC32校验实现。
 */

#include <stdbool.h>
#include <stdint.h>
#include "crc32.h"
#include "utils.h"
#include "magic_header.h"

bool magic_header_validate_data(const magic_header_t *header)
{
    if (header->magic != MAGIC_HEADER_MAGIC)
        return false;

    uint32_t ccrc = crc32((uint8_t *)header, offset_of(magic_header_t, this_crc32));
    if (ccrc != header->this_crc32)
        return false;

    /* 这些字段决定固件扫描与内部Flash擦写范围，使用前必须全部固定。 */
    if (header->format_version != MAGIC_HEADER_FORMAT_V2 ||
        header->header_size != MAGIC_HEADER_SIZE ||
        header->hash_algorithm != MAGIC_HASH_SHA256 ||
        header->signature_algorithm != MAGIC_SIGNATURE_ECDSA_P256_SHA256 ||
        header->data_type != MAGIC_HEADER_TYPE_APP ||
        header->data_offset != MAGIC_HEADER_AREA_SIZE ||
        header->data_address != MAGIC_DATA_ADDRESS ||
        header->data_length == 0 || header->data_length > MAGIC_DATA_MAX_SIZE ||
        header->this_address != MAGIC_HEADER_ADDR)
        return false;

    return true;
}

bool magic_encryption_header_validate(const xbin_encryption_header_t *header)
{
    uint32_t index;

    if (header->magic != XBIN_ENCRYPTION_MAGIC ||
        header->format_version != XBIN_ENCRYPTION_FORMAT_V1 ||
        header->header_size != XBIN_ENCRYPTION_HEADER_SIZE ||
        header->algorithm != XBIN_ENCRYPTION_ALGORITHM_AES_256_GCM ||
        header->key_id == 0u ||
        crc32((const uint8_t *)header,
              offsetof(xbin_encryption_header_t, this_crc32)) !=
            header->this_crc32)
        return false;

    for (index = 0; index < sizeof(header->reserved) / sizeof(header->reserved[0]);
         ++index)
    {
        if (header->reserved[index] != 0u)
            return false;
    }
    return true;
}
