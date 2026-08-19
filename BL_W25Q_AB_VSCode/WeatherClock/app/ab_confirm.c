/**
 * @file ab_confirm.c
 * @brief WeatherClock把通过健康观察期的Trial固件提交为Normal。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "bl24c512.h"
#include "ab_confirm.h"
#include "health_monitor.h"
#include "../../shared/ab_boot_contract.h"
#include "../../shared/firmware_image_format.h"
#include "../../shared/firmware_security_policy.h"

#ifndef AB_CONFIRM_ENABLED
#define AB_CONFIRM_ENABLED 1
#endif

#if AB_CONFIRM_ENABLED
/**
 * @brief 计算与Bootloader一致的标准CRC32。
 */
static uint32_t calculate_crc32(const uint8_t *data, uint32_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;
    uint32_t bit;

    for (i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/**
 * @brief 校验一份EEPROM启动状态副本。
 */
static bool state_valid(const boot_state_t *state)
{
    if (state->magic != BOOT_STATE_MAGIC || state->phase > BOOT_PHASE_ROLLBACK_PENDING)
        return false;
    if ((state->active_slot >= BOOT_SLOT_COUNT && state->active_slot != BOOT_SLOT_NONE) ||
        (state->pending_slot >= BOOT_SLOT_COUNT && state->pending_slot != BOOT_SLOT_NONE))
        return false;
    return state->record_crc32 ==
           calculate_crc32((const uint8_t *)state,
                           offsetof(boot_state_t, record_crc32));
}

/**
 * @brief 读取两份BCR并选择sequence较大的有效记录。
 */
static bool load_state(boot_state_t *state)
{
    boot_state_t a;
    boot_state_t b;
    bool av = bl24c512_read(BOOT_STATE_ADDR_A, (uint8_t *)&a, sizeof(a)) && state_valid(&a);
    bool bv = bl24c512_read(BOOT_STATE_ADDR_B, (uint8_t *)&b, sizeof(b)) && state_valid(&b);

    if (!av && !bv) return false;
    *state = av && (!bv || a.sequence >= b.sequence) ? a : b;
    /* v3.1及更早的BCR把新增字段留为EEPROM擦除值。 */
    if (state->minimum_security_version == UINT32_MAX)
        state->minimum_security_version = 0;
    if (state->pending_security_version == UINT32_MAX)
        state->pending_security_version = 0;
    return true;
}

/**
 * @brief 交替保存BCR并读回确认。
 */
static bool save_state(boot_state_t *state)
{
    boot_state_t verify;
    uint16_t address;

    state->sequence++;
    state->record_crc32 = calculate_crc32((const uint8_t *)state,
                                          offsetof(boot_state_t, record_crc32));
    address = (state->sequence & 1u) ? BOOT_STATE_ADDR_B : BOOT_STATE_ADDR_A;
    return bl24c512_write(address, (const uint8_t *)state, sizeof(*state)) &&
           bl24c512_read(address, (uint8_t *)&verify, sizeof(verify)) &&
           memcmp(state, &verify, sizeof(*state)) == 0;
}

/**
 * @brief 确认当前运行固件就是BCR中记录的pending固件。
 *
 * Bootloader在跳转前已经完成SHA-256、ECDSA签名和CRC32验证。应用端只复核
 * 带CRC保护的v2 Header及其中的固件CRC标识，避免再次扫描整个内部Flash。
 */
static bool running_firmware_matches(uint32_t expected_crc32,
                                     uint32_t expected_security_version)
{
    const magic_header_t *header = (const magic_header_t *)MAGIC_HEADER_ADDR;

    if (header->magic != MAGIC_HEADER_MAGIC ||
        header->format_version != MAGIC_HEADER_FORMAT_V2 ||
        header->header_size != MAGIC_HEADER_SIZE ||
        header->hash_algorithm != MAGIC_HASH_SHA256 ||
        header->signature_algorithm != MAGIC_SIGNATURE_ECDSA_P256_SHA256 ||
        header->data_type != MAGIC_HEADER_TYPE_APP ||
        header->data_offset != MAGIC_HEADER_AREA_SIZE ||
        header->data_address != MAGIC_DATA_ADDRESS ||
        header->data_length == 0 || header->data_length > MAGIC_DATA_MAX_SIZE ||
        header->this_address != MAGIC_HEADER_ADDR)
        return false;

    if (calculate_crc32((const uint8_t *)header,
                        offsetof(magic_header_t, this_crc32)) != header->this_crc32)
        return false;
    return header->data_crc32 == expected_crc32 &&
           header->security_version == expected_security_version;
}

/**
 * @brief FreeRTOS试运行确认任务。
 *
 * 先等待主初始化任务报告板级设备和基本显示路径正常，再观察调度器稳定运行
 * 5秒，最后把Trial原子地提交为Normal。网络状态不参与确认，避免临时断网
 * 被误判为固件启动失败。
 */
static void confirm_task(void *parameter)
{
    boot_state_t state;

    (void)parameter;
    while (!health_monitor_is_ready())
        vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(5000));
    bl24c512_init();

    if (load_state(&state) && state.phase == BOOT_PHASE_TRIAL &&
        state.pending_slot < BOOT_SLOT_COUNT &&
        running_firmware_matches(state.pending_crc32,
                                 state.pending_security_version))
    {
        state.phase = BOOT_PHASE_NORMAL;
        state.active_slot = state.pending_slot;
        state.active_crc32 = state.pending_crc32;
        state.minimum_security_version = firmware_security_floor_after_confirm(
            state.minimum_security_version, state.pending_security_version);
        state.pending_slot = BOOT_SLOT_NONE;
        state.pending_crc32 = 0;
        state.pending_security_version = 0;
        state.attempts_left = 0;
        if (save_state(&state))
            printf("[AB] trial firmware confirmed, security floor=%lu\n",
                   (unsigned long)state.minimum_security_version);
        else
            printf("[AB] confirm write failed\n");
    }
    vTaskDelete(NULL);
}
#endif

void ab_confirm_start(void)
{
#if AB_CONFIRM_ENABLED
    xTaskCreate(confirm_task, "ab confirm", 256, NULL, 4, NULL);
#endif
}
