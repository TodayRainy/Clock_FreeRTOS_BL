/**
 * @file stm32_flash.c
 * @brief STM32F407内部Flash扇区擦除和字编程封装。
 */

#include "stm32_flash.h"

#include <string.h>

#include "stm32f4xx.h"
#include "utils.h"

#define LOG_TAG    "flash"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

typedef struct
{
    uint32_t sector;
    uint32_t size;
} sector_desc_t;

static const sector_desc_t sector_descs[] =
{
    {FLASH_Sector_0, 16u * 1024u},
    {FLASH_Sector_1, 16u * 1024u},
    {FLASH_Sector_2, 16u * 1024u},
    {FLASH_Sector_3, 16u * 1024u},
    {FLASH_Sector_4, 64u * 1024u},
    {FLASH_Sector_5, 128u * 1024u},
    {FLASH_Sector_6, 128u * 1024u},
    {FLASH_Sector_7, 128u * 1024u},
    {FLASH_Sector_8, 128u * 1024u},
    {FLASH_Sector_9, 128u * 1024u},
    {FLASH_Sector_10, 128u * 1024u},
    {FLASH_Sector_11, 128u * 1024u},
};

void stm32_flash_lock(void)
{
    FLASH_Lock();
}

void stm32_flash_unlock(void)
{
    FLASH_Unlock();
}

bool stm32_flash_erase(uint32_t address, uint32_t size)
{
    uint32_t sector_address = STM32_FLASH_BASE;
    uint32_t end = address + size;
    uint32_t index;

    for (index = 0u; index < ARRAY_SIZE(sector_descs); ++index)
    {
        uint32_t sector_end = sector_address + sector_descs[index].size;

        if (sector_end > address && sector_address < end)
        {
            log_i("erasing sector %lu at address 0x%08lX size %lu",
                  index, sector_address, sector_descs[index].size);
            if (FLASH_EraseSector(sector_descs[index].sector,
                                  VoltageRange_3) != FLASH_COMPLETE)
            {
                log_e("flash erase error");
                return false;
            }
        }

        sector_address += sector_descs[index].size;
    }
    return true;
}

bool stm32_flash_program(uint32_t address, const uint8_t *data, uint32_t size)
{
    uint32_t word;
    uint32_t chunk;

    while (size)
    {
        word = 0xFFFFFFFFu;
        chunk = size >= sizeof(word) ? sizeof(word) : size;
        memcpy(&word, data, chunk);
        if (FLASH_ProgramWord(address, word) != FLASH_COMPLETE)
        {
            log_e("flash program error at address 0x%08lX", address);
            return false;
        }
        address += sizeof(word);
        data += chunk;
        size -= chunk;
    }
    return true;
}
