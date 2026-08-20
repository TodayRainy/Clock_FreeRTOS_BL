/**
 * @file bl24c512.h
 * @brief BL24C512 EEPROM 读写接口。
 */

#ifndef CLOCK_F407_APP_BL24C512_H
#define CLOCK_F407_APP_BL24C512_H

#include <stdbool.h>
#include <stdint.h>

#define BL24C512_PAGE_SIZE 128u

/** @brief 初始化 PB6/PB7 软件 I2C 总线。 */
void bl24c512_init(void);

/**
 * @brief 从 EEPROM 连续读取数据。
 * @return 完整读取成功时返回 true。
 */
bool bl24c512_read(uint16_t address, uint8_t *data, uint32_t size);

/**
 * @brief 连续写入数据，并在驱动内按 128 字节页边界拆分。
 * @return 所有页面写入并完成内部编程时返回 true。
 */
bool bl24c512_write(uint16_t address, const uint8_t *data, uint32_t size);

#endif /* CLOCK_F407_APP_BL24C512_H */
