/**
 * @file magic_header.c
 * @brief XBIN Formal Header的格式、布局和CRC32校验实现。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "crc32.h"
#include "utils.h"
#include "magic_header.h"

bool magic_header_validate_data(const magic_header_t *header)
{
    if (header->magic != MAGIC_HEADER_MAGIC)
        return false;

    uint32_t index;
    uint32_t ccrc = crc32((const uint8_t *)header,
                          offset_of(magic_header_t, header_crc32));

    if (ccrc != header->header_crc32)
        return false;

    /* Formal只有这一种算法组合和地址布局，不再保留旧容器分支。 */
    if (header->format_revision != MAGIC_HEADER_FORMAT_FORMAL ||
        header->header_size != MAGIC_HEADER_SIZE ||
        header->data_type != MAGIC_HEADER_TYPE_APP ||
        header->data_offset != MAGIC_HEADER_AREA_SIZE ||
        header->data_address != MAGIC_DATA_ADDRESS ||
        header->data_length == 0 || header->data_length > MAGIC_DATA_MAX_SIZE ||
        header->hash_algorithm != MAGIC_HASH_SHA256 ||
        header->signature_algorithm != MAGIC_SIGNATURE_ECDSA_P256_SHA256 ||
        header->encryption_algorithm != MAGIC_ENCRYPTION_AES_256_GCM ||
        header->signing_key_id == 0u || header->encryption_key_id == 0u ||
        header->reserved0 != 0u ||
        memchr(header->version, '\0', sizeof(header->version)) == NULL)
        return false;

    for (index = 0; index < sizeof(header->reserved) / sizeof(header->reserved[0]);
         ++index)
    {
        if (header->reserved[index] != 0u)
            return false;
    }
    return true;
}
