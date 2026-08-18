/**
 * @file boot_state.c
 * @brief A/B启动控制记录在BL24C512中的双副本持久化实现。
 */

#include <string.h>
#include "bl24c512.h"
#include "crc32.h"
#include "utils.h"
#include "boot_state.h"

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
    state->record_crc32 = crc32((const uint8_t *)state,
                                offset_of(boot_state_t, record_crc32));
}

bool boot_state_load(boot_state_t *state)
{
    boot_state_t a;
    boot_state_t b;
    bool a_valid = bl24c512_read(BOOT_STATE_ADDR_A, (uint8_t *)&a, sizeof(a)) &&
                   boot_state_record_valid(&a);
    bool b_valid = bl24c512_read(BOOT_STATE_ADDR_B, (uint8_t *)&b, sizeof(b)) &&
                   boot_state_record_valid(&b);

    if (!a_valid && !b_valid)
    {
        boot_state_default(state);
        return false;
    }

    if (a_valid && (!b_valid || a.sequence >= b.sequence))
        *state = a;
    else
        *state = b;
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
    address = (state->sequence & 1u) ? BOOT_STATE_ADDR_B : BOOT_STATE_ADDR_A;

    if (!bl24c512_write(address, (const uint8_t *)state, sizeof(*state)) ||
        !bl24c512_read(address, (uint8_t *)&verify, sizeof(verify)))
        return false;
    return memcmp(state, &verify, sizeof(*state)) == 0;
}
