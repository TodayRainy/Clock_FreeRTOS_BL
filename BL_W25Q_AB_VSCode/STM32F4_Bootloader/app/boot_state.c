/**
 * @file boot_state.c
 * @brief A/B启动控制记录在BL24C512中的双副本持久化实现。
 */

#include "boot_state.h"

#include <stdint.h>
#include <string.h>

#include "bl24c512.h"
#include "crc32.h"
#include "utils.h"

/**
 * @brief 把旧版 BCR 保留区中的擦除值迁移为尚未启用防降级的版本 0。
 *
 * v3.1及更早版本把这两个位置作为保留字节并保持为0xFF。记录先按原始内容
 * 完成CRC校验，再在RAM中规范化；下次正常保存时会自然写成新版格式。
 */
static void boot_state_normalize_security(boot_state_t *state)
{
    if (state->minimum_security_version == UINT32_MAX)
        state->minimum_security_version = 0;
    if (state->pending_security_version == UINT32_MAX)
        state->pending_security_version = 0;
}

/**
 * @brief 检查单个 EEPROM 状态副本的格式和 CRC32。
 */
static bool boot_state_record_valid(const boot_state_t *state)
{
    uint32_t crc;

    if (state->magic != BOOT_STATE_MAGIC || state->phase > BOOT_PHASE_ROLLBACK_PENDING)
        return false;
    if ((state->active_slot >= BOOT_SLOT_COUNT && state->active_slot != BOOT_SLOT_NONE) ||
        (state->pending_slot >= BOOT_SLOT_COUNT && state->pending_slot != BOOT_SLOT_NONE))
        return false;

    crc = crc32((const uint8_t *)state, offset_of(boot_state_t, record_crc32));
    return crc == state->record_crc32;
}

void boot_state_default(boot_state_t *state)
{
    memset(state, 0xFF, sizeof(*state));
    state->magic = BOOT_STATE_MAGIC;
    state->sequence = 0;
    state->phase = BOOT_PHASE_NORMAL;
    state->active_slot = BOOT_SLOT_NONE;
    state->pending_slot = BOOT_SLOT_NONE;
    state->attempts_left = 0;
    state->active_crc32 = 0;
    state->pending_crc32 = 0;
    state->minimum_security_version = 0;
    state->pending_security_version = 0;
    state->record_crc32 = crc32((const uint8_t *)state,
                                offset_of(boot_state_t, record_crc32));
}

bool boot_state_load(boot_state_t *state)
{
    boot_state_t state_a;
    boot_state_t state_b;
    bool state_a_valid;
    bool state_b_valid;

    state_a_valid =
        bl24c512_read(BOOT_STATE_ADDR_A, (uint8_t *)&state_a, sizeof(state_a)) &&
        boot_state_record_valid(&state_a);
    state_b_valid =
        bl24c512_read(BOOT_STATE_ADDR_B, (uint8_t *)&state_b, sizeof(state_b)) &&
        boot_state_record_valid(&state_b);

    if (!state_a_valid && !state_b_valid)
    {
        boot_state_default(state);
        return false;
    }

    if (state_a_valid && (!state_b_valid || state_a.sequence >= state_b.sequence))
        *state = state_a;
    else
        *state = state_b;

    boot_state_normalize_security(state);
    return true;
}

bool boot_state_save(boot_state_t *state)
{
    boot_state_t verify;
    uint16_t address;

    state->magic = BOOT_STATE_MAGIC;
    state->sequence++;
    state->record_crc32 = crc32((const uint8_t *)state,
                                offset_of(boot_state_t, record_crc32));

    /* 奇偶交替使本次写入和上一份有效记录位于不同 EEPROM 页。 */
    address = (state->sequence & 1u) ? BOOT_STATE_ADDR_B : BOOT_STATE_ADDR_A;

    if (!bl24c512_write(address, (const uint8_t *)state, sizeof(*state)) ||
        !bl24c512_read(address, (uint8_t *)&verify, sizeof(verify)))
        return false;
    return memcmp(state, &verify, sizeof(*state)) == 0;
}
