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
#include "firmware_security_policy.h"
#include "boot_protocol.h"
#include "crc16.h"
#include "crc32.h"
#include "utils.h"

#define LOG_TAG    "protocol"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

#ifdef FIRMWARE_BUILD_PROFILE_DEV
#define BL_VERSION       "v3.3.0-resume-dev"
#else
#define BL_VERSION       "v3.3.0-resume-prod"
#endif
#define RX_TIMEOUT_MS              20u
#define HIGH_SPEED_IDLE_TIMEOUT_MS 2000u

/* 8 KiB固件块加旧PROGRAM命令的8字节参数头。 */
#define PAYLOAD_SIZE_MAX (8192u + 8u)
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
#define SLOT_RESUME_FIXED_LENGTH      7u
#define SLOT_RESUME_CRC_LENGTH        4u
#define SLOT_RESUME_MAX_BLOCKS        \
    ((MAGIC_DATA_MAX_SIZE + FIRMWARE_RESUME_BLOCK_SIZE - 1u) / \
     FIRMWARE_RESUME_BLOCK_SIZE)
#define EEPROM_READ_PARAM_LENGTH      4u
#define EEPROM_WRITE_HEADER_LENGTH    4u
#define EEPROM_MAINTENANCE_BASE       0xFC00u
#define EEPROM_MAINTENANCE_LIMIT      BOOT_STATE_ADDR_A

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
    PACKET_OPCODE_SET_BAUD = 0x02,
    PACKET_OPCODE_RESET = 0x21,
    PACKET_OPCODE_BOOT = 0x22,
    PACKET_OPCODE_W25_INFO = 0x40,
    PACKET_OPCODE_W25_READ = 0x41,
    PACKET_OPCODE_W25_ERASE = 0x42,
    PACKET_OPCODE_W25_PROGRAM = 0x43,
    PACKET_OPCODE_W25_PROGRAM_ASYNC = 0x44,
    PACKET_OPCODE_EEPROM_INFO = 0x48,
    PACKET_OPCODE_EEPROM_READ = 0x49,
    PACKET_OPCODE_EEPROM_WRITE = 0x4A,
    PACKET_OPCODE_SLOT_STATUS = 0x50,
    PACKET_OPCODE_SLOT_SET_ACTIVE = 0x51,
    PACKET_OPCODE_SLOT_ACTIVATE = 0x52,
    PACKET_OPCODE_SLOT_SNAPSHOT = 0x53,
    PACKET_OPCODE_SLOT_STATUS_FAST = 0x54,
    PACKET_OPCODE_SECURITY_STATUS = 0x55,
    PACKET_OPCODE_SLOT_RESUME_SCAN = 0x56,
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
    PACKET_ERRCODE_DENIED,
    PACKET_ERRCODE_UNKNOWN = 0xFF,
} packet_errcode_t;

static uint8_t packet_buffer[PACKET_SIZE_MAX];
static uint32_t packet_index;
static packet_state_t packet_state = PACKET_STATE_HEADER;
static packet_opcode_t packet_opcode;
static uint16_t packet_payload_length;
static uint8_t storage_buffer[4096];
static uint32_t resume_crc32[SLOT_RESUME_MAX_BLOCKS];
static bool w25_program_ok = true;
static bool maintenance_unlocked;
static bool downgrade_authorized;
static uint32_t protocol_baud_rate = BL_USART_DEFAULT_BAUD_RATE;
static uint64_t protocol_last_activity_ms;

/**
 * @brief 高速协议链路空闲过久后恢复到默认波特率。
 *
 * 超时从上一条协议响应发送完成后开始计算，而不是从命令到达时开始计算。
 * 因此W25Q擦除、校验等耗时操作不会误占恢复窗口。DMA中仍有数据时也不会
 * 切换波特率；如果上位机只发了半包后离线，超时后则丢弃残包并恢复115200。
 */
static void recover_default_baud_if_idle(void)
{
    uint64_t now_ms;

    if (protocol_baud_rate == BL_USART_DEFAULT_BAUD_RATE ||
        bl_usart_data_pending())
        return;

    now_ms = tim_get_ms();
    if (now_ms - protocol_last_activity_ms < HIGH_SPEED_IDLE_TIMEOUT_MS)
        return;

    packet_index = 0;
    packet_state = PACKET_STATE_HEADER;
    bl_usart_set_baud(BL_USART_DEFAULT_BAUD_RATE);
    protocol_baud_rate = BL_USART_DEFAULT_BAUD_RATE;
    protocol_last_activity_ms = now_ms;
    log_w("protocol idle timeout, baud rate restored to %u",
          BL_USART_DEFAULT_BAUD_RATE);
}

/**
 * @brief 判断一个Opcode是否属于可能改写底层状态或导出原始内容的工程接口。
 *
 * W25_ERASE/W25_PROGRAM是正常A/B上传必须经过的受限命令：它们只能操作非活动
 * 槽，因此不列入维护命令。SLOT_ACTIVATE仍会执行完整签名验证。
 */
static bool opcode_requires_maintenance(packet_opcode_t opcode)
{
    switch (opcode)
    {
        case PACKET_OPCODE_W25_READ:
        case PACKET_OPCODE_EEPROM_READ:
        case PACKET_OPCODE_EEPROM_WRITE:
        case PACKET_OPCODE_SLOT_SET_ACTIVE:
        case PACKET_OPCODE_SLOT_SNAPSHOT:
        case PACKET_OPCODE_ERASE:
        case PACKET_OPCODE_PROGRAM:
        case PACKET_OPCODE_VERIFY:
            return true;

        default:
            return false;
    }
}

void boot_protocol_set_maintenance_unlocked(bool physical_presence)
{
    downgrade_authorized = physical_presence;
#ifdef FIRMWARE_BUILD_PROFILE_DEV
    (void)physical_presence;
    maintenance_unlocked = true;
#else
    maintenance_unlocked = physical_presence;
#endif
    log_i("security profile=%s maintenance=%s downgrade=%s",
#ifdef FIRMWARE_BUILD_PROFILE_DEV
          "DEV",
#else
          "PROD",
#endif
          maintenance_unlocked ? "unlocked" : "locked",
          downgrade_authorized ? "authorized" : "locked");
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
    protocol_last_activity_ms = tim_get_ms();
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

/**
 * @brief 在115200初始链路上协商后续协议波特率。
 *
 * ACK始终使用旧波特率完整发送；ACK离开发送移位寄存器后才切换USART。
 * 限定少量实测价值明确的档位，避免上位机误传任意数值后失去当前会话。
 */
static void handle_set_baud(void)
{
    uint32_t baud_rate;

    if (packet_payload_length != sizeof(baud_rate))
    {
        protocol_response(PACKET_OPCODE_SET_BAUD, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    baud_rate = get_u32(packet_buffer + PACKET_PAYLOAD_OFFSET);
    if (baud_rate != BL_USART_DEFAULT_BAUD_RATE && baud_rate != 230400u &&
        baud_rate != 460800u && baud_rate != 921600u && baud_rate != 2000000u)
    {
        protocol_response(PACKET_OPCODE_SET_BAUD, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    protocol_response(PACKET_OPCODE_SET_BAUD, PACKET_ERRCODE_OK, NULL, 0);
    bl_usart_set_baud(baud_rate);
    protocol_baud_rate = baud_rate;
    protocol_last_activity_ms = tim_get_ms();
    log_i("protocol baud rate: %u", baud_rate);
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
        address < EEPROM_MAINTENANCE_BASE ||
        (uint32_t)address + size > EEPROM_MAINTENANCE_LIMIT)
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

/**
 * @brief 把BCR中供上位机观察的字段编码为固定17字节协议格式。
 * @return 指向已编码数据末尾的游标。
 */
static uint8_t *encode_boot_state(uint8_t *cursor, const boot_state_t *state,
                                  bool state_found)
{
    put_u8_inc(&cursor, state_found ? 1u : 0u);
    put_u8_inc(&cursor, state->phase);
    put_u8_inc(&cursor, state->active_slot);
    put_u8_inc(&cursor, state->pending_slot);
    put_u8_inc(&cursor, state->attempts_left);
    put_u32_inc(&cursor, state->sequence);
    put_u32_inc(&cursor, state->active_crc32);
    put_u32_inc(&cursor, state->pending_crc32);
    return cursor;
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
    cursor = encode_boot_state(cursor, &state, state_found);

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

/**
 * @brief 返回升级决策所需的BCR状态，不扫描两份外部固件的数据区。
 *
 * 完整SLOT_STATUS会读取两个槽的全部固件并计算CRC32，适合人工健康检查；
 * 正常升级只需知道当前阶段和活动槽，因此本命令仅返回两份EEPROM状态记录中
 * 通过格式及CRC校验的最新一份。新槽在SLOT_ACTIVATE时仍会执行完整CRC校验，
 * 本命令不会降低固件提交条件。
 *
 * 响应固定为17字节：state_found、phase、active_slot、pending_slot、
 * attempts_left、sequence、active_crc32、pending_crc32。
 */
static void handle_slot_status_fast(void)
{
    boot_state_t state;
    uint8_t result[17];
    uint8_t *cursor = result;
    bool state_found;

    if (packet_payload_length != 0)
    {
        protocol_response(PACKET_OPCODE_SLOT_STATUS_FAST,
                          PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    state_found = boot_state_load(&state);
    cursor = encode_boot_state(cursor, &state, state_found);

    protocol_response(PACKET_OPCODE_SLOT_STATUS_FAST, PACKET_ERRCODE_OK,
                      result, cursor - result);
}

/**
 * @brief 返回防降级下限、当前候选版本以及本次会话的物理授权状态。
 *
 * 响应固定为9字节，便于上位机在发送固件前解释设备策略；最终授权仍由
 * SLOT_ACTIVATE在固件签名和完整数据校验通过后执行。
 */
static void handle_security_status(void)
{
    boot_state_t state;
    uint8_t result[9];
    uint8_t *cursor = result;

    if (packet_payload_length != 0)
    {
        protocol_response(PACKET_OPCODE_SECURITY_STATUS,
                          PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    boot_state_load(&state);
    put_u32_inc(&cursor, state.minimum_security_version);
    put_u32_inc(&cursor, state.pending_security_version);
    put_u8_inc(&cursor, downgrade_authorized ? 1u : 0u);
    protocol_response(PACKET_OPCODE_SECURITY_STATUS, PACKET_ERRCODE_OK,
                      result, cursor - result);
}

/**
 * @brief 使非活动槽Header失效并查找候选固件的安全续传位置。
 *
 * 请求格式：slot(1)、data_length(4)、block_count(2)，随后是每个4 KiB应用
 * 数据块的CRC32。设备按顺序比较并在首个不一致块停止，响应一个槽内绝对
 * 偏移。最后一块CRC只覆盖真实数据长度。
 *
 * 扫描前只把旧Header的magic编程为0，不擦除数据区。这样即使随后立即掉电，
 * 非活动槽也保持无效；上位机完成数据续传后会擦除Header扇区并最后重写有效
 * Header。该命令不返回固件原文，也不能操作活动槽，因此属于正常升级接口。
 */
static void handle_slot_resume_scan(void)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint8_t invalid_magic[4] = {0, 0, 0, 0};
    uint8_t response[4];
    uint8_t slot;
    uint16_t block_count;
    uint32_t data_length;
    uint32_t expected_blocks;
    uint32_t resume_offset;
    uint32_t index;

    if (packet_payload_length < SLOT_RESUME_FIXED_LENGTH)
    {
        protocol_response(PACKET_OPCODE_SLOT_RESUME_SCAN,
                          PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = payload[0];
    data_length = get_u32(payload + 1);
    block_count = get_u16(payload + 5);
    expected_blocks = (data_length + FIRMWARE_RESUME_BLOCK_SIZE - 1u) /
                      FIRMWARE_RESUME_BLOCK_SIZE;
    if (slot >= BOOT_SLOT_COUNT || !slot_write_allowed(slot) ||
        data_length == 0 || data_length > MAGIC_DATA_MAX_SIZE ||
        block_count != expected_blocks || block_count > SLOT_RESUME_MAX_BLOCKS ||
        packet_payload_length != SLOT_RESUME_FIXED_LENGTH +
                                 block_count * SLOT_RESUME_CRC_LENGTH)
    {
        protocol_response(PACKET_OPCODE_SLOT_RESUME_SCAN,
                          PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    for (index = 0; index < block_count; ++index)
        resume_crc32[index] = get_u32(payload + SLOT_RESUME_FIXED_LENGTH +
                                     index * SLOT_RESUME_CRC_LENGTH);

    /* NOR Flash把任意旧magic可靠地变成0，只涉及1->0，不需要先擦除。 */
    if (!w25q_program((w25q_chip_t)slot, 0, invalid_magic,
                      sizeof(invalid_magic)))
    {
        w25_program_ok = false;
        protocol_response(PACKET_OPCODE_SLOT_RESUME_SCAN,
                          PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }
    w25_program_ok = true;

    if (!firmware_slot_find_resume_offset(slot, data_length, resume_crc32,
                                          block_count, &resume_offset))
    {
        protocol_response(PACKET_OPCODE_SLOT_RESUME_SCAN,
                          PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    put_u32(response, resume_offset);
    log_i("resume scan slot=%u matched=%u/%u bytes", slot,
          resume_offset - MAGIC_HEADER_AREA_SIZE, data_length);
    protocol_response(PACKET_OPCODE_SLOT_RESUME_SCAN, PACKET_ERRCODE_OK,
                      response, sizeof(response));
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
    state.minimum_security_version = firmware_security_floor_after_confirm(
        state.minimum_security_version, slot_header.security_version);
    state.pending_security_version = 0;
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
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint32_t expected_crc = 0;
    uint8_t slot;

    /*
     * 1字节格式兼容旧上位机；新上位机额外发送目标XBIN的数据CRC32。
     * 这样完整槽只需由本命令扫描一次，仍能确认Bootloader看到的镜像就是
     * 上位机刚刚选择的那个文件。
     */
    if (packet_payload_length != 1 && packet_payload_length != 5)
    {
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = payload[0];
    if (packet_payload_length == 5)
        expected_crc = get_u32(payload + 1);

    if (!boot_state_load(&state) || state.phase != BOOT_PHASE_NORMAL ||
        state.active_slot >= BOOT_SLOT_COUNT || slot >= BOOT_SLOT_COUNT ||
        slot == state.active_slot || !firmware_slot_validate(slot, &header) ||
        (packet_payload_length == 5 && header.data_crc32 != expected_crc))
    {
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    if (!firmware_security_version_allowed(state.minimum_security_version,
                                           header.security_version,
                                           downgrade_authorized))
    {
        log_w("firmware security downgrade denied: candidate=%u minimum=%u",
              header.security_version, state.minimum_security_version);
        protocol_response(PACKET_OPCODE_SLOT_ACTIVATE,
                          PACKET_ERRCODE_DENIED, NULL, 0);
        return;
    }

    state.phase = BOOT_PHASE_INSTALL_PENDING;
    state.pending_slot = slot;
    state.pending_crc32 = header.data_crc32;
    state.pending_security_version = header.security_version;
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
    state.minimum_security_version = header.security_version;
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
    w25_program_ok = true;
    protocol_response(PACKET_OPCODE_W25_ERASE, PACKET_ERRCODE_OK, NULL, 0);
}

/**
 * @brief 写入一个外部槽数据块。
 *
 * 普通W25_PROGRAM在写入完成后应答，用作一段流水的最终屏障。
 * W25_PROGRAM_ASYNC在请求CRC和参数通过后先应答，再执行页编程，使PC可以在
 * W25Q忙期间发送下一块。由于PC收到ACK后才开始下一帧，两条串口方向不会在
 * 大包尾部重叠。异步写失败会由下一块或最终同步块返回VERIFY。
 *
 * @param[in] async_ack true表示使用先应答后写入的流水模式。
 */
static void handle_w25_program(bool async_ack)
{
    uint8_t *payload = packet_buffer + PACKET_PAYLOAD_OFFSET;
    uint8_t slot;
    uint32_t address;
    uint16_t size;

    if (packet_payload_length <= W25_PROGRAM_HEADER_LENGTH)
    {
        protocol_response(packet_opcode, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    slot = payload[0];
    address = get_u32(payload + 1);
    size = get_u16(payload + 5);
    if (slot >= BOOT_SLOT_COUNT || !slot_write_allowed(slot) ||
        size != packet_payload_length - W25_PROGRAM_HEADER_LENGTH ||
        address >= FIRMWARE_SLOT_SIZE || size > FIRMWARE_SLOT_SIZE - address)
    {
        protocol_response(packet_opcode, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (!w25_program_ok)
    {
        protocol_response(packet_opcode, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    if (async_ack)
        protocol_response(packet_opcode, PACKET_ERRCODE_OK, NULL, 0);

    if (!w25q_program((w25q_chip_t)slot, address,
                      payload + W25_PROGRAM_HEADER_LENGTH, size))
    {
        w25_program_ok = false;
        if (!async_ack)
            protocol_response(packet_opcode, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    if (!async_ack)
        protocol_response(packet_opcode, PACKET_ERRCODE_OK, NULL, 0);
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
    if (opcode_requires_maintenance(packet_opcode) && !maintenance_unlocked)
    {
        log_w("maintenance opcode denied: %02X", packet_opcode);
        protocol_response(packet_opcode, PACKET_ERRCODE_DENIED, NULL, 0);
        return BOOT_PROTOCOL_ACTION_NONE;
    }

    switch (packet_opcode)
    {
        case PACKET_OPCODE_INQUERY:         handle_inquiry(); break;
        case PACKET_OPCODE_SET_BAUD:        handle_set_baud(); break;
        case PACKET_OPCODE_W25_INFO:        handle_w25_info(); break;
        case PACKET_OPCODE_W25_READ:        handle_w25_read(); break;
        case PACKET_OPCODE_W25_ERASE:       handle_w25_erase(); break;
        case PACKET_OPCODE_W25_PROGRAM:     handle_w25_program(false); break;
        case PACKET_OPCODE_W25_PROGRAM_ASYNC: handle_w25_program(true); break;
        case PACKET_OPCODE_EEPROM_INFO:     handle_eeprom_info(); break;
        case PACKET_OPCODE_EEPROM_READ:     handle_eeprom_read(); break;
        case PACKET_OPCODE_EEPROM_WRITE:    handle_eeprom_write(); break;
        case PACKET_OPCODE_SLOT_STATUS:     handle_slot_status(); break;
        case PACKET_OPCODE_SLOT_SET_ACTIVE: handle_slot_set_active(); break;
        case PACKET_OPCODE_SLOT_ACTIVATE:   handle_slot_activate(); break;
        case PACKET_OPCODE_SLOT_SNAPSHOT:   handle_slot_snapshot(); break;
        case PACKET_OPCODE_SLOT_STATUS_FAST: handle_slot_status_fast(); break;
        case PACKET_OPCODE_SECURITY_STATUS: handle_security_status(); break;
        case PACKET_OPCODE_SLOT_RESUME_SCAN: handle_slot_resume_scan(); break;
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
    bl_usart_init();
    protocol_baud_rate = BL_USART_DEFAULT_BAUD_RATE;
    protocol_last_activity_ms = tim_get_ms();
}

bool boot_protocol_data_pending(void)
{
    return bl_usart_data_pending();
}

boot_protocol_action_t boot_protocol_process(void)
{
    uint8_t byte;

    recover_default_baud_if_idle();

    /*
     * 主循环一次尽量排空DMA中已经到达的数据，但发现完整帧后立即执行该帧。
     * W25_PROGRAM执行期间，下一帧仍可由DMA独立写入循环缓冲区。
     */
    while (bl_usart_read_byte(&byte))
    {
        if (parse_byte(byte))
            return handle_packet();
    }
    return BOOT_PROTOCOL_ACTION_NONE;
}
