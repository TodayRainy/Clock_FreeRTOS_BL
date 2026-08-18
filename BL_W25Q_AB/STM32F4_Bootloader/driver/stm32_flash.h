#ifndef __STM32_FLASH_H__
#define __STM32_FLASH_H__

#include <stdbool.h>
#include <stdint.h>

#define STM32_FLASH_BASE       0x08000000U
#define STM32_FLASH_SIZE       (512 * 1024)

/** @brief 锁定STM32内部Flash控制器。 */
void stm32_flash_lock(void);

/** @brief 解锁STM32内部Flash控制器。 */
void stm32_flash_unlock(void);

/**
 * @brief 擦除与给定地址区间相交的所有内部Flash扇区。
 *
 * 本函数按STM32F407真实扇区边界工作。调用方必须先解锁Flash，并保证地址范围
 * 已经过布局校验。
 *
 * @param[in] address 要覆盖的起始地址。
 * @param[in] size 要覆盖的字节数。
 * @return 所有相关扇区擦除成功时返回 true。
 */
bool stm32_flash_erase(uint32_t address, uint32_t size);

/**
 * @brief 以32位字为单位写入STM32内部Flash。
 *
 * 最后一组不足4字节时以0xFF补齐。调用方必须先擦除并解锁目标区域。
 *
 * @param[in] address 写入起始地址。
 * @param[in] data 待写数据。
 * @param[in] size 数据字节数。
 * @return 每个字均编程成功时返回 true。
 */
bool stm32_flash_program(uint32_t address, const uint8_t *data, uint32_t size);

#endif /* __STM32_FLASH_H__ */
