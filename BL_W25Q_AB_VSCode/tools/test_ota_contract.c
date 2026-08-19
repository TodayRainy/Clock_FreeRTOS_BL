/**
 * @file test_ota_contract.c
 * @brief 在宿主机上锁定 OTA EEPROM 记录的尺寸、偏移和 CRC 覆盖范围。
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ota_contract.h"
#include "crc32.h"

int main(void)
{
    ota_progress_t progress;
    ota_request_t request;
    uint32_t expected_crc;

    assert(sizeof(progress) == OTA_RECORD_SIZE);
    assert(sizeof(request) == OTA_RECORD_SIZE);
    assert(offsetof(ota_progress_t, record_crc32) == 124u);
    assert(offsetof(ota_request_t, record_crc32) == 124u);
    assert(OTA_REQUEST_ADDR - OTA_PROGRESS_ADDR == OTA_RECORD_SIZE);

    memset(&progress, 0, sizeof(progress));
    progress.magic = OTA_PROGRESS_MAGIC;
    progress.target_slot = 1u;
    progress.next_offset = 4096u + OTA_DOWNLOAD_CHECKPOINT;
    progress.record_crc32 = crc32((const uint8_t *)&progress,
                                  offsetof(ota_progress_t, record_crc32));
    expected_crc = progress.record_crc32;
    progress.next_offset++;
    assert(crc32((const uint8_t *)&progress,
                 offsetof(ota_progress_t, record_crc32)) != expected_crc);

    memset(&request, 0, sizeof(request));
    request.magic = OTA_REQUEST_MAGIC;
    request.target_slot = 0u;
    request.xbin_size = 123456u;
    request.record_crc32 = crc32((const uint8_t *)&request,
                                 offsetof(ota_request_t, record_crc32));
    expected_crc = request.record_crc32;
    request.image_sha256[31] ^= 1u;
    assert(crc32((const uint8_t *)&request,
                 offsetof(ota_request_t, record_crc32)) != expected_crc);

    puts("OTA contract self-test passed");
    return 0;
}
