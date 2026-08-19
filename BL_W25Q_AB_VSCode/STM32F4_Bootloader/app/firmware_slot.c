/**
 * @file firmware_slot.c
 * @brief 内部固件和外部A/B槽的校验、安装与快照实现。
 */

#include "w25q.h"
#include "stm32_flash.h"
#include "crc32.h"
#include "sha256.h"
#include "watchdog.h"
#include "firmware_auth.h"
#include "magic_header.h"
#include "firmware_slot.h"

#define INSTALL_BUFFER_SIZE 4096u

static uint8_t install_buffer[INSTALL_BUFFER_SIZE];

#define SRAM_START     0x20000000u
#define SRAM_END       0x20020000u
#define CCMRAM_START   0x10000000u
#define CCMRAM_END     0x10010000u

/**
 * @brief 从固件字节流读取一个小端32位整数。
 *
 * 外部Flash数据先进入字节缓冲区。逐字节组装既不依赖缓冲区的自然对齐，
 * 也与C编译器的指针别名规则无关，适合读取镜像开头的两个向量表项。
 */
static uint32_t read_u32_le(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

/**
 * @brief 检查应用初始栈和Reset_Handler是否落在允许范围。
 *
 * 栈顶允许等于RAM区域的上边界；Reset_Handler必须带Thumb位并位于本镜像数据
 * 范围内。该检查在真正跳转前执行，也用于拒绝结构正确但无法启动的外部镜像。
 */
static bool application_vector_valid(uint32_t initial_sp, uint32_t reset_handler,
                                     const magic_header_t *header)
{
    uint32_t reset_address = reset_handler & ~1u;
    bool stack_valid = ((initial_sp >= SRAM_START && initial_sp <= SRAM_END) ||
                        (initial_sp >= CCMRAM_START && initial_sp <= CCMRAM_END)) &&
                       (initial_sp & 7u) == 0;

    return stack_valid && (reset_handler & 1u) != 0 &&
           reset_address >= header->data_address &&
           reset_address < header->data_address + header->data_length;
}

/**
 * @brief 在一次顺序读取中同时计算CRC32和SHA-256。
 */
static bool internal_data_valid(const magic_header_t *header)
{
    const uint8_t *data = (const uint8_t *)header->data_address;
    uint8_t digest[SHA256_DIGEST_SIZE];
    sha256_context_t sha;
    uint32_t crc_state = crc32_begin();
    uint32_t remaining = header->data_length;

    sha256_init(&sha);
    while (remaining)
    {
        uint32_t chunk = remaining > INSTALL_BUFFER_SIZE ?
                         INSTALL_BUFFER_SIZE : remaining;
        crc_state = crc32_update(crc_state, data, chunk);
        sha256_update(&sha, data, chunk);
        watchdog_refresh();
        data += chunk;
        remaining -= chunk;
    }
    sha256_final(&sha, digest);

    return crc32_end(crc_state) == header->data_crc32 &&
           firmware_hash_matches(header, digest) &&
           application_vector_valid(*(const uint32_t *)header->data_address,
                                    *(const uint32_t *)(header->data_address + 4u),
                                    header);
}

/**
 * @copydoc firmware_internal_validate
 */
bool firmware_internal_validate(magic_header_t *header)
{
    *header = *(const magic_header_t *)MAGIC_HEADER_ADDR;
    if (!magic_header_validate_data(header) ||
        !firmware_authenticate_header(header))
        return false;
    return internal_data_valid(header);
}

/**
 * @copydoc firmware_slot_load_header
 */
bool firmware_slot_load_header(uint8_t slot, magic_header_t *header)
{
    if (slot >= W25Q_CHIP_COUNT)
        return false;

    w25q_read((w25q_chip_t)slot, 0, (uint8_t *)header, sizeof(*header));
    if (!magic_header_validate_data(header) ||
        !firmware_authenticate_header(header))
        return false;

    return header->data_offset <= FIRMWARE_SLOT_SIZE &&
           header->data_length <= FIRMWARE_SLOT_SIZE - header->data_offset;
}

/**
 * @copydoc firmware_slot_verify_data
 */
bool firmware_slot_verify_data(uint8_t slot, const magic_header_t *header)
{
    uint32_t address;
    uint32_t remaining;
    uint32_t chunk;
    uint32_t state;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t initial_sp = 0;
    uint32_t reset_handler = 0;
    sha256_context_t sha;

    if (slot >= W25Q_CHIP_COUNT)
        return false;

    address = header->data_offset;
    remaining = header->data_length;
    state = crc32_begin();
    sha256_init(&sha);
    while (remaining)
    {
        chunk = remaining > sizeof(install_buffer) ? sizeof(install_buffer) : remaining;
        w25q_read((w25q_chip_t)slot, address, install_buffer, chunk);
        if (address == header->data_offset && chunk >= 8u)
        {
            initial_sp = read_u32_le(install_buffer);
            reset_handler = read_u32_le(install_buffer + 4u);
        }
        state = crc32_update(state, install_buffer, chunk);
        sha256_update(&sha, install_buffer, chunk);
        watchdog_refresh();
        address += chunk;
        remaining -= chunk;
    }
    sha256_final(&sha, digest);
    return crc32_end(state) == header->data_crc32 &&
           firmware_hash_matches(header, digest) &&
           application_vector_valid(initial_sp, reset_handler, header);
}

/**
 * @copydoc firmware_slot_validate
 */
bool firmware_slot_validate(uint8_t slot, magic_header_t *header)
{
    return firmware_slot_load_header(slot, header) &&
           firmware_slot_verify_data(slot, header);
}

/**
 * @copydoc firmware_slot_find_resume_offset
 */
bool firmware_slot_find_resume_offset(uint8_t slot, uint32_t data_length,
                                      const uint32_t *expected_crc32,
                                      uint16_t block_count,
                                      uint32_t *resume_offset)
{
    uint32_t address = MAGIC_HEADER_AREA_SIZE;
    uint32_t remaining = data_length;
    uint32_t expected_blocks;
    uint32_t index;

    if (slot >= W25Q_CHIP_COUNT || data_length == 0 ||
        data_length > MAGIC_DATA_MAX_SIZE || expected_crc32 == NULL ||
        resume_offset == NULL)
        return false;

    expected_blocks = (data_length + FIRMWARE_RESUME_BLOCK_SIZE - 1u) /
                      FIRMWARE_RESUME_BLOCK_SIZE;
    if (block_count != expected_blocks)
        return false;

    for (index = 0; index < expected_blocks; ++index)
    {
        uint32_t chunk = remaining > FIRMWARE_RESUME_BLOCK_SIZE ?
                         FIRMWARE_RESUME_BLOCK_SIZE : remaining;

        w25q_read((w25q_chip_t)slot, address, install_buffer, chunk);
        if (crc32(install_buffer, chunk) != expected_crc32[index])
        {
            *resume_offset = address;
            return true;
        }

        watchdog_refresh();
        address += chunk;
        remaining -= chunk;
    }

    *resume_offset = MAGIC_HEADER_AREA_SIZE + data_length;
    return true;
}

/**
 * @copydoc firmware_slot_install
 */
bool firmware_slot_install(uint8_t slot, const magic_header_t *verified_header)
{
    magic_header_t header;
    uint32_t source;
    uint32_t destination;
    uint32_t remaining;
    uint32_t chunk;
    bool ok = false;

    if (slot >= W25Q_CHIP_COUNT || verified_header == NULL)
        return false;

    /*
     * A/B状态机刚刚完整验证过这个槽。复制Header可避免安装过程中依赖调用方
     * 的栈对象，同时省去对同一份约300 KiB固件的第二次连续读取。
     */
    header = *verified_header;

    stm32_flash_unlock();

    /* 先擦固件头。之后任何时刻掉电，旧的半成品都不会被当成有效应用。 */
    if (!stm32_flash_erase(MAGIC_HEADER_ADDR, MAGIC_HEADER_AREA_SIZE) ||
        !stm32_flash_erase(MAGIC_DATA_ADDRESS, header.data_length))
        goto out;

    source = header.data_offset;
    destination = header.data_address;
    remaining = header.data_length;
    while (remaining)
    {
        chunk = remaining > sizeof(install_buffer) ? sizeof(install_buffer) : remaining;
        w25q_read((w25q_chip_t)slot, source, install_buffer, chunk);
        if (!stm32_flash_program(destination, install_buffer, chunk))
            goto out;
        watchdog_refresh();
        source += chunk;
        destination += chunk;
        remaining -= chunk;
    }

    if (!internal_data_valid(&header))
        goto out;

    /* 数据确认无误后，最后写入256字节有效头，完成一次原子式提交。 */
    if (!stm32_flash_program(MAGIC_HEADER_ADDR, (const uint8_t *)&header, sizeof(header)))
        goto out;

    /* 验证刚写入Flash的Header，而不是仍在RAM中的源副本。 */
    header = *(const magic_header_t *)MAGIC_HEADER_ADDR;
    ok = magic_header_validate_data(&header) &&
         firmware_authenticate_header(&header);

out:
    stm32_flash_lock();
    return ok;
}

/**
 * @copydoc firmware_slot_snapshot_internal
 */
bool firmware_slot_snapshot_internal(uint8_t slot)
{
    magic_header_t header;
    uint32_t source;
    uint32_t destination;
    uint32_t remaining;
    uint32_t chunk;
    uint32_t erase_size;
    magic_header_t verify_header;

    if (slot >= W25Q_CHIP_COUNT || !firmware_internal_validate(&header))
        return false;

    erase_size = header.data_offset + header.data_length;
    erase_size = (erase_size + W25Q_SECTOR_SIZE - 1) & ~(W25Q_SECTOR_SIZE - 1);
    if (!w25q_erase((w25q_chip_t)slot, 0, erase_size))
        return false;

    /* 先复制应用数据，最后写首256字节头；快照中断不会产生有效的半成品槽。 */
    source = header.data_address;
    destination = header.data_offset;
    remaining = header.data_length;
    while (remaining)
    {
        chunk = remaining > sizeof(install_buffer) ? sizeof(install_buffer) : remaining;
        if (!w25q_program((w25q_chip_t)slot, destination,
                          (const uint8_t *)source, chunk))
            return false;
        watchdog_refresh();
        source += chunk;
        destination += chunk;
        remaining -= chunk;
    }

    if (!w25q_program((w25q_chip_t)slot, 0, (const uint8_t *)&header, sizeof(header)))
        return false;
    return firmware_slot_validate(slot, &verify_header);
}
