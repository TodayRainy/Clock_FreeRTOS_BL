/**
 * @file firmware_slot.c
 * @brief 内部固件和外部A/B槽的校验、安装与快照实现。
 */

#include "w25q.h"
#include "stm32_flash.h"
#include "crc32.h"
#include "magic_header.h"
#include "firmware_slot.h"

#define INSTALL_BUFFER_SIZE 4096u

static uint8_t install_buffer[INSTALL_BUFFER_SIZE];

/**
 * @copydoc firmware_internal_validate
 */
bool firmware_internal_validate(magic_header_t *header)
{
    *header = *(const magic_header_t *)MAGIC_HEADER_ADDR;
    if (!magic_header_validate_data(header))
        return false;
    return crc32((const uint8_t *)header->data_address, header->data_length) ==
           header->data_crc32;
}

/**
 * @copydoc firmware_slot_load_header
 */
bool firmware_slot_load_header(uint8_t slot, magic_header_t *header)
{
    if (slot >= W25Q_CHIP_COUNT)
        return false;

    w25q_read((w25q_chip_t)slot, 0, (uint8_t *)header, sizeof(*header));
    if (!magic_header_validate_data(header))
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

    if (slot >= W25Q_CHIP_COUNT)
        return false;

    address = header->data_offset;
    remaining = header->data_length;
    state = crc32_begin();
    while (remaining)
    {
        chunk = remaining > sizeof(install_buffer) ? sizeof(install_buffer) : remaining;
        w25q_read((w25q_chip_t)slot, address, install_buffer, chunk);
        state = crc32_update(state, install_buffer, chunk);
        address += chunk;
        remaining -= chunk;
    }
    return crc32_end(state) == header->data_crc32;
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
 * @copydoc firmware_slot_install
 */
bool firmware_slot_install(uint8_t slot, const magic_header_t *verified_header)
{
    magic_header_t header;
    uint32_t source;
    uint32_t destination;
    uint32_t remaining;
    uint32_t chunk;
    uint32_t internal_crc;
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
        source += chunk;
        destination += chunk;
        remaining -= chunk;
    }

    internal_crc = crc32((const uint8_t *)header.data_address, header.data_length);
    if (internal_crc != header.data_crc32)
        goto out;

    /* 数据确认无误后，最后写入256字节有效头，完成一次原子式提交。 */
    if (!stm32_flash_program(MAGIC_HEADER_ADDR, (const uint8_t *)&header, sizeof(header)))
        goto out;
    ok = magic_header_validate();

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
        source += chunk;
        destination += chunk;
        remaining -= chunk;
    }

    if (!w25q_program((w25q_chip_t)slot, 0, (const uint8_t *)&header, sizeof(header)))
        return false;
    return firmware_slot_validate(slot, &verify_header);
}
