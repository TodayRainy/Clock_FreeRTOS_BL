#include <stdint.h>
#include <stdio.h>
#include "stm32f4xx_hal.h"
#include "stm32f4xx_hal_flash.h"
#include "stm32f4xx_hal_flash_ex.h"
#include "macros.h"

#define LOG_TAG    "flash"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

#define FLASH_BASE_ADDRESS  0x08000000

typedef struct
{
    uint32_t sector;
    uint32_t size;
} sector_desc_t;

static const sector_desc_t sector_descs[] =
 {
    {FLASH_SECTOR_0,  16 * 1024},  // Sector 0: 16 KB
    {FLASH_SECTOR_1,  16 * 1024},  // Sector 1: 16 KB
    {FLASH_SECTOR_2,  16 * 1024},  // Sector 2: 16 KB
    {FLASH_SECTOR_3,  16 * 1024},  // Sector 3: 16 KB
    {FLASH_SECTOR_4,  64 * 1024},  // Sector 4: 64 KB
    {FLASH_SECTOR_5,  128 * 1024}, // Sector 5: 128 KB
    {FLASH_SECTOR_6,  128 * 1024}, // Sector 6: 128 KB
    {FLASH_SECTOR_7,  128 * 1024}, // Sector 7: 128 KB
    {FLASH_SECTOR_8,  128 * 1024}, // Sector 8: 128 KB
    {FLASH_SECTOR_9,  128 * 1024}, // Sector 9: 128 KB
    {FLASH_SECTOR_10, 128 * 1024}, // Sector10: 128 KB
    {FLASH_SECTOR_11, 128 * 1024}, // Sector11: 128 KB
};

void stm32_flash_lock(void)
{
    HAL_FLASH_Lock();
}

void stm32_flash_unlock(void)
{
    HAL_FLASH_Unlock();
}

void stm32_flash_erase(uint32_t address, uint32_t size)
{
    uint32_t addr = FLASH_BASE_ADDRESS;

    for (uint32_t i = 0; i < ARRAY_SIZE(sector_descs); i++)
    {
        if (addr >= address && addr < address + size)
        {
            log_i("erasing sector %lu at address 0x%08lX size %lu", i, addr, sector_descs[i].size);

            FLASH_EraseInitTypeDef erase_init;
            uint32_t sector_error = 0;
            erase_init.TypeErase    = FLASH_TYPEERASE_SECTORS;
            erase_init.Sector       = sector_descs[i].sector;
            erase_init.NbSectors    = 1;
            erase_init.VoltageRange = FLASH_VOLTAGE_RANGE_3;
            if (HAL_FLASHEx_Erase(&erase_init, &sector_error) != HAL_OK)
            {
                log_e("flash erase error");
            }
        }

        addr += sector_descs[i].size;
    }
}

void stm32_flash_program(uint32_t address, const uint8_t *data, uint32_t size)
{
    for (uint32_t i = 0; i < size; i += 4)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, address + i,
                              (uint64_t)(*(uint32_t *)(data + i))) != HAL_OK)
        {
            log_e("flash program error at address 0x%08lX", address + i);
        }
    }
}
