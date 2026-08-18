/**
 * @file boot_protocol.c
 * @brief USART3 Bootloader协议实现及命令处理。
 *
 * 请求帧：AA + opcode + payload_length(LE16) + payload + CRC16(LE16)
 * 响应帧：55 + opcode + error + data_length(LE16) + data + CRC16(LE16)
 *
 * 本模块负责通信，不决定设备当前应该安装、试运行还是回退哪个固件。
 * BOOT命令只向bootloader.c返回动作，由统一的A/B状态机做最终决定。
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "stm32f4xx.h"
#include "bl_usart.h"
#include "tim_delay.h"
#include "stm32_flash.h"
#include "w25q.h"
#include "bl24c512.h"
#include "boot_state.h"
#include "firmware_slot.h"
#include "magic_header.h"
#include "boot_protocol.h"
#include "crc16.h"
#include "crc32.h"
#include "ringbuffer.h"
#include "utils.h"

#define LOG_TAG    "protocol"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

#define BL_VERSION       "v1.2.0-ab"
#define RX_BUFFER_SIZE   (5u * 1024u)
#define RX_TIMEOUT_MS    20u

/* 旧PROGRAM命令保留8字节参数头，因此协议MTU继续保持4104字节。 */
#define PAYLOAD_SIZE_MAX (4096u + 8u)
#define PACKET_SIZE_MAX  (4u + PAYLOAD_SIZE_MAX + 2u)

#define PACKET_HEADER_REQUEST   0xAAu
#define PACKET_HEADER_RESPONSE  0x55u
#define PACKET_HEADER_OFFSET    0u
#define PACKET_OPCODE_OFFSET    1u
#define PACKET_LENGTH_OFFSET    2u
#define PACKET_PAYLOAD_OFFSET   4u
#define PACKET_CRC_SIZE         2u
#define PACKET_MIN_SIZE         6u

#define ADDR_SIZE_PARAM_LENGTH        8u
#define ADDR_SIZE_CRC_PARAM_LENGTH    12u
#define W25_READ_PARAM_LENGTH         7u
#define W25_ERASE_PARAM_LENGTH        9u
#define W25_PROGRAM_HEADER_LENGTH     7u
#define EEPROM_READ_PARAM_LENGTH      4u
#define EEPROM_WRITE_HEADER_LENGTH    4u
#define EEPROM_RESERVED_BASE          0xFC00u

typedef enum
{
    PACKET_STATE_HEADER,
    PACKET_STATE_OPCODE,
    PACKET_STATE_LENGTH,
    PACKET_STATE_PAYLOAD,
    PACKET_STATE_CRC16,
} packet_state_t;

typedef enum
{
    PACKET_OPCODE_INQUERY = 0x01,
    PACKET_OPCODE_RESET = 0x21,
    PACKET_OPCODE_BOOT = 0x22,
    PACKET_OPCODE_W25_INFO = 0x40,
    PACKET_OPCODE_W25_READ = 0x41,
    PACKET_OPCODE_W25_ERASE = 0x42,
    PACKET_OPCODE_W25_PROGRAM = 0x43,
    PACKET_OPCODE_EEPROM_INFO = 0x48,
    PACKET_OPCODE_EEPROM_READ = 0x49,
    PACKET_OPCODE_EEPROM_WRITE = 0x4A,
    PACKET_OPCODE_SLOT_STATUS = 0x50,
    PACKET_OPCODE_SLOT_SET_ACTIVE = 0x51,
    PACKET_OPCODE_SLOT_ACTIVATE = 0x52,
    PACKET_OPCODE_SLOT_SNAPSHOT = 0x53,
    PACKET_OPCODE_ERASE = 0x81,
    PACKET_OPCODE_PROGRAM = 0x82,
    PACKET_OPCODE_VERIFY = 0x83,
} packet_opcode_t;

typedef enum
{
    INQUERY_SUBCODE_VERSION = 0x00,
    INQUERY_SUBCODE_MTU = 0x01,
} inquery_subcode_t;

typedef enum
{
    PACKET_ERRCODE_OK = 0,
    PACKET_ERRCODE_OPCODE,
    PACKET_ERRCODE_OVERFLOW,
    PACKET_ERRCODE_TIMEOUT,
    PACKET_ERRCODE_FORMAT,
    PACKET_ERRCODE_VERIFY,
    PACKET_ERRCODE_PARAM,
    PACKET_ERRCODE_UNKNOWN = 0xFF,
} packet_errcode_t;

static uint8_t rb_buffer[RX_BUFFER_SIZE];
static rb_t rxrb;
static uint8_t packet_buffer[PACKET_SIZE_MAX];
static uint32_t packet_index;
static packet_state_t packet_state = PACKET_STATE_HEADER;
static packet_opcode_t packet_opcode;
static uint16_t packet_payload_length;
static uint8_t storage_buffer[4096];

/**
 * @brief USART中断回调，只搬运数据，不在中断中解析或擦写Flash。
 */
static void protocol_rx_handler(const uint8_t *data, uint32_t length)
{
    rb_puts(rxrb, data, length);
}

/**
 * @brief 发送一个带错误码和CRC16的协议响应。
 */
static void protocol_response(packet_opcode_t opcode, packet_errcode_t error,
                              const uint8_t *data, uint16_t length)
{
    uint8_t *response = packet_buffer;
    uint8_t *cursor = response;
    uint16_t crc;

    put_u8_inc(&cursor, PACKET_HEADER_RESPONSE);
    put_u8_inc(&cursor, (uint8_t)opcode);
    put_u8_inc(&cursor, (uint8_t)error);
    put_u16_inc(&cursor, length);
    put_bytes_inc(&cursor, data, length);
    crc = crc16(response, cursor - response);
    put_u16_inc(&cursor, crc);
    bl_usart_write(response, cursor - response);
}

/**
 * @brief 判断一段STM32内部Flash范围是否完全有效。
 */
static bool internal_flash_range_valid(uint32_t address, uint32_t size)
{
    uint32_t flash_end = STM32_FLASH_BASE + STM32_FLASH_SIZE;

    return size > 0 && address >= STM32_FLASH_BASE &&
           address < flash_end && size <= flash_end - address;
}

/**
 * @brief 判断旧调试命令是否会触及Bootloader保留区。
 */
static bool internal_flash_range_protected(uint32_t address, uint32_t size)
{
    uint32_t end = address + size;

    return address < MAGIC_HEADER_ADDR && end > STM32_FLASH_BASE;
}

/**
 * @brief 判断当前阶段是否允许写指定外部槽。
 *
 * 没有BCR时允许首次初始化。存在BCR后，仅Normal阶段可以写非活动槽；
 * InstallPending、Trial和RollbackPending期间冻结两个槽，避免破坏恢复来源。
 */
static bool slot_write_allowed(uint8_t slot)
{
    boot_state_t state;

    if (!boot_state_load(&state))
        return true;
    return state.phase == BOOT_PHASE_NORMAL && state.active_slot != slot;
}

static void handle_inquiry(void)
{
    uint8_t subcode;

    if (packet_payload_length != 1)
    {
        protocol_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    subcode = get_u8(packet_buffer + PACKET_PAYLOAD_OFFSET);
    if (subcode == INQUERY_SUBCODE_VERSION)
    {
        protocol_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_OK,
                          (const uint8_t *)BL_VERSION, strlen(BL_VERSION));
    }
    else if (subcode == INQUERY_SUBCODE_MTU)
    {
        uint8_t mtu[2];
        put_u16(mtu, PAYLOAD_SIZE_MAX);
        protocol_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_OK, mtu, sizeof(mtu));
    }
    else
    {
        protocol_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_PARAM, NULL, 0);
    }
}

static void handle_w25_info(void)
{
    uint8_t info[28];
    uint8_t *cursor = info;
    uint32_t chip;

    if (packet_payload_length != 0)
    {
        protocol_response(PACKET_OPCODE_W25_INFO, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    /* 每颗芯片14字节：JEDEC ID(4)、SR1、SR2、Unique ID(8)。 */
    for (chip = 0; chip < W25Q_CHIP_COUNT; ++chip)
    {
        put_u32_inc(&cursor, w25q_read_jedec_id((w25q_chip_t)chip));
        put_u8_inc(&cursor, w25q_read_status1((w25q_chip_t)chip));
        put_u8_inc(&cursor, w25q_read_status2((w25q_chip_t)chip));
        w25q_read_unique_id((w25q_chip_t)chip, cursor);
        cursor += 8;
    }
    protocol_response(PACKET_OPCODE_W25_INFO, PACKET_ERRCODE_OK,
                      info, cursor - info);
}

static void handle_w25_read(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint8_t chip;
    uint32_t address;
    uint16_t size;

    if (packet_payload_length != W25_READ_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_W25_READ, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    chip = payload[0];
    address = get_u32(payload + 1);
    size = get_u16(payload + 5);
    if (chip >= W25Q_CHIP_COUNT || size == 0 || size > sizeof(storage_buffer) ||
        address > W25Q_FLASH_SIZE - size)
    {
        protocol_response(PACKET_OPCODE_W25_READ, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    w25q_read((w25q_chip_t)chip, address, storage_buffer, size);
    protocol_response(PACKET_OPCODE_W25_READ, PACKET_ERRCODE_OK,
                      storage_buffer, size);
}

static void handle_eeprom_info(void)
{
    uint8_t present;

    if (packet_payload_length != 0)
    {
        protocol_response(PACKET_OPCODE_EEPROM_INFO, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }
    present = bl24c512_is_present() ? 1u : 0u;
    protocol_response(PACKET_OPCODE_EEPROM_INFO, PACKET_ERRCODE_OK, &present, 1);
}

static void handle_eeprom_read(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint16_t address;
    uint16_t size;

    if (packet_payload_length != EEPROM_READ_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_EEPROM_READ, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    address = get_u16(payload);
    size = get_u16(payload + 2);
    if (size == 0 || size > sizeof(storage_buffer) ||
        (uint32_t)address + size > BL24C512_SIZE)
    {
        protocol_response(PACKET_OPCODE_EEPROM_READ, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (!bl24c512_read(address, storage_buffer, size))
    {
        protocol_response(PACKET_OPCODE_EEPROM_READ, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_EEPROM_READ, PACKET_ERRCODE_OK,
                      storage_buffer, size);
}

static void handle_eeprom_write(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint16_t address;
    uint16_t size;

    if (packet_payload_length <= EEPROM_WRITE_HEADER_LENGTH)
    {
        protocol_response(PACKET_OPCODE_EEPROM_WRITE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    address = get_u16(payload);
    size = get_u16(payload + 2);
    if (size != packet_payload_length - EEPROM_WRITE_HEADER_LENGTH ||
        address < EEPROM_RESERVED_BASE || (uint32_t)address + size > BL24C512_SIZE)
    {
        protocol_response(PACKET_OPCODE_EEPROM_WRITE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (!bl24c512_write(address, payload + EEPROM_WRITE_HEADER_LENGTH, size) ||
        !bl24c512_read(address, storage_buffer, size) ||
        memcmp(storage_buffer, payload + EEPROM_WRITE_HEADER_LENGTH, size) != 0)
    {
        protocol_response(PACKET_OPCODE_EEPROM_WRITE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_EEPROM_WRITE, PACKET_ERRCODE_OK, NULL, 0);
}

static void handle_slot_status(void)
{
    boot_state_t state;
    magic_header_t header;
    uint8_t result[101];
    uint8_t *cursor = result;
    uint32_t slot;
    bool state_found;
    bool header_valid;
    bool full_valid;

    if (packet_payload_length != 0)
    {
        protocol_response(PACKET_OPCODE_SLOT_STATUS, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    state_found = boot_state_load(&state);
    put_u8_inc(&cursor, state_found ? 1u : 0u);
    put_u8_inc(&cursor, state.phase);
    put_u8_inc(&cursor, state.active_slot);
    put_u8_inc(&cursor, state.pending_slot);
    put_u8_inc(&cursor, state.attempts_left);
    put_u32_inc(&cursor, state.sequence);
    put_u32_inc(&cursor, state.active_crc32);
    put_u32_inc(&cursor, state.pending_crc32);

    for (slot = 0; slot < BOOT_SLOT_COUNT; ++slot)
    {
        memset(&header, 0, sizeof(header));
        header_valid = firmware_slot_load_header(slot, &header);
        full_valid = header_valid && firmware_slot_verify_data(slot, &header);
        put_u8_inc(&cursor, header_valid ? 1u : 0u);
        put_u8_inc(&cursor, full_valid ? 1u : 0u);
        put_u32_inc(&cursor, header_valid ? header.data_length : 0u);
        put_u32_inc(&cursor, header_valid ? header.data_crc32 : 0u);
        if (header_valid)
            put_bytes_inc(&cursor, (const uint8_t *)header.version, 32);
        else
        {
            memset(cursor, 0, 32);
            cursor += 32;
        }
    }
    protocol_response(PACKET_OPCODE_SLOT_STATUS, PACKET_ERRCODE_OK,
                      result, cursor - result);
}

static void handle_slot_set_active(void)
{
    boot_state_t state;
    magic_header_t slot_header;
    magic_header_t internal_header;
    uint8_t slot;

    if (packet_payload_length != 1)
    {
        protocol_response(PACKET_OPCODE_SLOT_SET_ACTIVE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = packet_buffer[PACKET_PAYLOAD_OFFSET];
    if (slot >= BOOT_SLOT_COUNT || !firmware_slot_validate(slot, &slot_header) ||
        !firmware_internal_validate(&internal_header) ||
        internal_header.data_crc32 != slot_header.data_crc32)
    {
        protocol_response(PACKET_OPCODE_SLOT_SET_ACTIVE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    boot_state_load(&state);
    state.phase = BOOT_PHASE_NORMAL;
    state.active_slot = slot;
    state.pending_slot = BOOT_SLOT_NONE;
    state.attempts_left = 0;
    state.active_crc32 = slot_header.data_crc32;
    state.pending_crc32 = 0;
    if (!boot_state_save(&state))
    {
        protocol_response(PACKET_OPCODE_SLOT_SET_ACTIVE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_SLOT_SET_ACTIVE, PACKET_ERRCODE_OK, NULL, 0);
}

static void handle_slot_activate(void)
{
    boot_state_t state;
    magic_header_t header;
    uint8_t slot;

    if (packet_payload_length != 1)
    {
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = packet_buffer[PACKET_PAYLOAD_OFFSET];
    if (!boot_state_load(&state) || state.phase != BOOT_PHASE_NORMAL ||
        state.active_slot >= BOOT_SLOT_COUNT || slot >= BOOT_SLOT_COUNT ||
        slot == state.active_slot || !firmware_slot_validate(slot, &header))
    {
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    state.phase = BOOT_PHASE_INSTALL_PENDING;
    state.pending_slot = slot;
    state.pending_crc32 = header.data_crc32;
    state.attempts_left = BOOT_TRIAL_ATTEMPTS;
    if (!boot_state_save(&state))
    {
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    /* 先确保ACK离开发送移位寄存器，再复位进入安装流程。 */
    protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_OK, NULL, 0);
    tim_delay_ms(5);
    NVIC_SystemReset();
}

static void handle_slot_snapshot(void)
{
    boot_state_t state;
    magic_header_t header;
    uint8_t slot;

    if (packet_payload_length != 1)
    {
        protocol_response(PACKET_OPCODE_SLOT_SNAPSHOT, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = packet_buffer[PACKET_PAYLOAD_OFFSET];
    if ((boot_state_load(&state) && state.active_slot < BOOT_SLOT_COUNT) ||
        slot >= BOOT_SLOT_COUNT || !firmware_slot_snapshot_internal(slot) ||
        !firmware_slot_validate(slot, &header))
    {
        protocol_response(PACKET_OPCODE_SLOT_SNAPSHOT, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    boot_state_default(&state);
    state.active_slot = slot;
    state.active_crc32 = header.data_crc32;
    if (!boot_state_save(&state))
    {
        protocol_response(PACKET_OPCODE_SLOT_SNAPSHOT, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_SLOT_SNAPSHOT, PACKET_ERRCODE_OK, NULL, 0);
}

static void handle_w25_erase(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint8_t slot;
    uint32_t address;
    uint32_t size;

    if (packet_payload_length != W25_ERASE_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_W25_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = payload[0];
    address = get_u32(payload + 1);
    size = get_u32(payload + 5);
    if (slot >= BOOT_SLOT_COUNT || !slot_write_allowed(slot) || size == 0 ||
        address >= FIRMWARE_SLOT_SIZE || size > FIRMWARE_SLOT_SIZE - address)
    {
        protocol_response(PACKET_OPCODE_W25_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    log_i("erase W25 slot=%u address=0x%06X size=%u", slot, address, size);
    if (!w25q_erase((w25q_chip_t)slot, address, size))
    {
        protocol_response(PACKET_OPCODE_W25_ERASE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_W25_ERASE, PACKET_ERRCODE_OK, NULL, 0);
}

static void handle_w25_program(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint8_t slot;
    uint32_t address;
    uint16_t size;

    if (packet_payload_length <= W25_PROGRAM_HEADER_LENGTH)
    {
        protocol_response(PACKET_OPCODE_W25_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = payload[0];
    address = get_u32(payload + 1);
    size = get_u16(payload + 5);
    if (slot >= BOOT_SLOT_COUNT || !slot_write_allowed(slot) ||
        size != packet_payload_length - W25_PROGRAM_HEADER_LENGTH ||
        address >= FIRMWARE_SLOT_SIZE || size > FIRMWARE_SLOT_SIZE - address)
    {
        protocol_response(PACKET_OPCODE_W25_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (!w25q_program((w25q_chip_t)slot, address,
                      payload + W25_PROGRAM_HEADER_LENGTH, size))
    {
        protocol_response(PACKET_OPCODE_W25_PROGRAM, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    protocol_response(PACKET_OPCODE_W25_PROGRAM, PACKET_ERRCODE_OK, NULL, 0);
}

/* 以下三个内部Flash命令仅为旧上位机和维护场景保留。 */
static void handle_legacy_erase(void)
{
    uint8_t *payload;
    uint32_t address;
    uint32_t size;
    bool ok;

    if (packet_payload_length != ADDR_SIZE_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    address = get_u32_inc(&payload);
    size = get_u32_inc(&payload);
    if (!internal_flash_range_valid(address, size) ||
        internal_flash_range_protected(address, size))
    {
        protocol_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    stm32_flash_unlock();
    ok = stm32_flash_erase(address, size);
    stm32_flash_lock();
    protocol_response(PACKET_OPCODE_ERASE,
                      ok ? PACKET_ERRCODE_OK : PACKET_ERRCODE_VERIFY, NULL, 0);
}

static void handle_legacy_program(void)
{
    uint8_t *payload;
    uint32_t address;
    uint32_t size;
    bool ok;

    if (packet_payload_length <= ADDR_SIZE_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    address = get_u32_inc(&payload);
    size = get_u32_inc(&payload);
    if (size != packet_payload_length - ADDR_SIZE_PARAM_LENGTH ||
        !internal_flash_range_valid(address, size) ||
        internal_flash_range_protected(address, size))
    {
        protocol_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    stm32_flash_unlock();
    ok = stm32_flash_program(address, payload, size);
    stm32_flash_lock();
    protocol_response(PACKET_OPCODE_PROGRAM,
                      ok ? PACKET_ERRCODE_OK : PACKET_ERRCODE_VERIFY, NULL, 0);
}

static void handle_legacy_verify(void)
{
    uint8_t *payload;
    uint32_t address;
    uint32_t size;
    uint32_t expected_crc;

    if (packet_payload_length != ADDR_SIZE_CRC_PARAM_LENGTH)
    {
        protocol_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    address = get_u32_inc(&payload);
    size = get_u32_inc(&payload);
    expected_crc = get_u32_inc(&payload);
    if (!internal_flash_range_valid(address, size))
    {
        protocol_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    protocol_response(PACKET_OPCODE_VERIFY,
                      crc32((const uint8_t *)address, size) == expected_crc ?
                      PACKET_ERRCODE_OK : PACKET_ERRCODE_VERIFY,
                      NULL, 0);
}

/**
 * @brief 执行一个已经完成CRC校验的请求帧。
 */
static boot_protocol_action_t handle_packet(void)
{
    switch (packet_opcode)
    {
        case PACKET_OPCODE_INQUERY:         handle_inquiry(); break;
        case PACKET_OPCODE_W25_INFO:        handle_w25_info(); break;
        case PACKET_OPCODE_W25_READ:        handle_w25_read(); break;
        case PACKET_OPCODE_W25_ERASE:       handle_w25_erase(); break;
        case PACKET_OPCODE_W25_PROGRAM:     handle_w25_program(); break;
        case PACKET_OPCODE_EEPROM_INFO:     handle_eeprom_info(); break;
        case PACKET_OPCODE_EEPROM_READ:     handle_eeprom_read(); break;
        case PACKET_OPCODE_EEPROM_WRITE:    handle_eeprom_write(); break;
        case PACKET_OPCODE_SLOT_STATUS:     handle_slot_status(); break;
        case PACKET_OPCODE_SLOT_SET_ACTIVE: handle_slot_set_active(); break;
        case PACKET_OPCODE_SLOT_ACTIVATE:   handle_slot_activate(); break;
        case PACKET_OPCODE_SLOT_SNAPSHOT:   handle_slot_snapshot(); break;
        case PACKET_OPCODE_ERASE:           handle_legacy_erase(); break;
        case PACKET_OPCODE_PROGRAM:         handle_legacy_program(); break;
        case PACKET_OPCODE_VERIFY:          handle_legacy_verify(); break;

        case PACKET_OPCODE_RESET:
            if (packet_payload_length != 0)
            {
                protocol_response(PACKET_OPCODE_RESET, PACKET_ERRCODE_PARAM, NULL, 0);
                break;
            }
            protocol_response(PACKET_OPCODE_RESET, PACKET_ERRCODE_OK, NULL, 0);
            tim_delay_ms(2);
            NVIC_SystemReset();
            break;

        case PACKET_OPCODE_BOOT:
            if (packet_payload_length != 0)
            {
                protocol_response(PACKET_OPCODE_BOOT, PACKET_ERRCODE_PARAM, NULL, 0);
                break;
            }
            protocol_response(PACKET_OPCODE_BOOT, PACKET_ERRCODE_OK, NULL, 0);
            return BOOT_PROTOCOL_ACTION_BOOT_APPLICATION;

        default:
            log_w("unknown opcode: %02X", packet_opcode);
            protocol_response(packet_opcode, PACKET_ERRCODE_OPCODE, NULL, 0);
            break;
    }
    return BOOT_PROTOCOL_ACTION_NONE;
}

/**
 * @brief 向字节状态机输入一个字节。
 * @return 收到并校验出一个完整帧时返回 true。
 */
static bool parse_byte(uint8_t byte)
{
    static uint64_t last_byte_ms;
    uint64_t now_ms = tim_get_ms();
    bool complete = false;

    if (now_ms - last_byte_ms > RX_TIMEOUT_MS)
    {
        if (packet_state != PACKET_STATE_HEADER)
            log_w("packet receive timeout");
        packet_index = 0;
        packet_state = PACKET_STATE_HEADER;
    }
    last_byte_ms = now_ms;

    packet_buffer[packet_index++] = byte;
    switch (packet_state)
    {
        case PACKET_STATE_HEADER:
            if (packet_buffer[PACKET_HEADER_OFFSET] == PACKET_HEADER_REQUEST)
                packet_state = PACKET_STATE_OPCODE;
            else
            {
                packet_index = 0;
                packet_state = PACKET_STATE_HEADER;
            }
            break;

        case PACKET_STATE_OPCODE:
            /* Opcode语义只在handle_packet中维护，解析器不再保存第二份白名单。 */
            packet_opcode = (packet_opcode_t)packet_buffer[PACKET_OPCODE_OFFSET];
            packet_state = PACKET_STATE_LENGTH;
            break;

        case PACKET_STATE_LENGTH:
            if (packet_index == PACKET_PAYLOAD_OFFSET)
            {
                packet_payload_length = get_u16(packet_buffer + PACKET_LENGTH_OFFSET);
                if (packet_payload_length <= PAYLOAD_SIZE_MAX)
                {
                    packet_state = packet_payload_length ?
                                   PACKET_STATE_PAYLOAD : PACKET_STATE_CRC16;
                }
                else
                {
                    log_w("payload too large: %u", packet_payload_length);
                    packet_index = 0;
                    packet_state = PACKET_STATE_HEADER;
                }
            }
            break;

        case PACKET_STATE_PAYLOAD:
            if (packet_index == PACKET_PAYLOAD_OFFSET + packet_payload_length)
                packet_state = PACKET_STATE_CRC16;
            break;

        case PACKET_STATE_CRC16:
            if (packet_index == PACKET_MIN_SIZE + packet_payload_length)
            {
                uint16_t received_crc = get_u16(
                    packet_buffer + PACKET_PAYLOAD_OFFSET + packet_payload_length);
                uint16_t calculated_crc = crc16(
                    packet_buffer, PACKET_PAYLOAD_OFFSET + packet_payload_length);

                complete = received_crc == calculated_crc;
                if (!complete)
                    log_w("CRC16 mismatch: expected %04X, got %04X",
                          received_crc, calculated_crc);
                packet_index = 0;
                packet_state = PACKET_STATE_HEADER;
            }
            break;
    }
    return complete;
}

void boot_protocol_init(void)
{
    rxrb = rb_new(rb_buffer, sizeof(rb_buffer));
    bl_usart_register_rx_callback(protocol_rx_handler);
    bl_usart_init();
}

bool boot_protocol_data_pending(void)
{
    return !rb_empty(rxrb);
}

boot_protocol_action_t boot_protocol_process(void)
{
    uint8_t byte;

    if (rb_empty(rxrb))
        return BOOT_PROTOCOL_ACTION_NONE;
    rb_get(rxrb, &byte);
    return parse_byte(byte) ? handle_packet() : BOOT_PROTOCOL_ACTION_NONE;
}
