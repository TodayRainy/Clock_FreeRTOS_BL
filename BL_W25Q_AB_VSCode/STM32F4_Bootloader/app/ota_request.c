/**
 * @file ota_request.c
 * @brief OTA 安装请求的读取、独立验证与 BCR 提交。
 *
 * WeatherClock 先把完整 XBIN 下载到非活动 W25Q 槽，最后写入本模块读取的
 * 一次性请求。请求本身不能绕过固件签名校验：Bootloader始终重新扫描外部
 * 固件，并仅在全部检查通过后进入现有的 A/B 安装状态机。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "bl24c512.h"
#include "boot_state.h"
#include "crc32.h"
#include "firmware_security_policy.h"
#include "firmware_slot.h"
#include "ota_contract.h"
#include "ota_request.h"

#define LOG_TAG    "ota"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

/**
 * @brief 检查请求记录自身的固定字段和 CRC32。
 */
static bool request_valid(const ota_request_t *request)
{
    uint32_t crc;

    if (request->magic != OTA_REQUEST_MAGIC ||
        request->target_slot >= BOOT_SLOT_COUNT)
        return false;

    crc = crc32((const uint8_t *)request,
                offsetof(ota_request_t, record_crc32));
    return crc == request->record_crc32;
}

/**
 * @brief 清除请求魔术字，使同一请求不会被重复消费。
 *
 * EEPROM 允许在同一页内只更新这 4 字节。即使掉电导致清除失败，已经保存的
 * 非 Normal BCR 也会优先推进原有安装事务，下一次启动再清除这条陈旧请求。
 */
static void request_clear(void)
{
    const uint32_t empty_magic = 0u;

    if (!bl24c512_write(OTA_REQUEST_ADDR,
                        (const uint8_t *)&empty_magic,
                        sizeof(empty_magic)))
        log_w("cannot clear OTA request");
}

void ota_request_process(void)
{
    ota_request_t request;
    boot_state_t state;
    magic_header_t header;
    uint32_t xbin_size;

    if (!bl24c512_read(OTA_REQUEST_ADDR, (uint8_t *)&request,
                       sizeof(request)) ||
        !request_valid(&request))
        return;

    if (!boot_state_load(&state) || state.phase != BOOT_PHASE_NORMAL)
    {
        /* BCR 中已有事务时，绝不让一条旧请求改变它的方向。 */
        log_w("discarding stale OTA request");
        request_clear();
        return;
    }

    if (state.active_slot >= BOOT_SLOT_COUNT ||
        request.target_slot == state.active_slot ||
        !firmware_slot_validate(request.target_slot, &header))
    {
        log_e("OTA target slot is not a valid inactive image");
        request_clear();
        return;
    }

    xbin_size = header.data_offset + header.data_length;
    if (request.xbin_size != xbin_size ||
        request.header_crc32 != header.this_crc32 ||
        request.payload_crc32 != header.data_crc32 ||
        request.security_version != header.security_version ||
        memcmp(request.image_sha256, header.image_sha256,
               sizeof(header.image_sha256)) != 0)
    {
        log_e("OTA request does not match target image");
        request_clear();
        return;
    }

    if (!firmware_security_version_allowed(state.minimum_security_version,
                                           header.security_version, false))
    {
        log_e("OTA security version %lu is below floor %lu",
              (unsigned long)header.security_version,
              (unsigned long)state.minimum_security_version);
        request_clear();
        return;
    }

    state.phase = BOOT_PHASE_INSTALL_PENDING;
    state.pending_slot = request.target_slot;
    state.pending_crc32 = header.data_crc32;
    state.pending_security_version = header.security_version;
    state.attempts_left = 0;
    if (!boot_state_save(&state))
    {
        /* 保留请求，下一次复位可以重新提交。 */
        log_e("cannot commit OTA request to boot state");
        return;
    }

    log_i("OTA slot %u accepted, version=%s", request.target_slot,
          header.version);
    request_clear();
}
