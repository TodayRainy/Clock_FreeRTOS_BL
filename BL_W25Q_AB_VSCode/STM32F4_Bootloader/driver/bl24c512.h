#ifndef CLOCK_F407_BL24C512_H
#define CLOCK_F407_BL24C512_H

#include <stdbool.h>
#include <stdint.h>

#define BL24C512_SIZE       (64u * 1024u)
#define BL24C512_PAGE_SIZE  128u

/**
 * @brief 初始化PB6/PB7软件I2C总线。
 */
void bl24c512_init(void);

/**
 * @brief 通过地址应答检测EEPROM。
 * @return BL24C512响应0x50七位地址时返回 true。
 */
bool bl24c512_is_present(void);

/**
 * @brief 从EEPROM当前地址空间连续读取数据。
 * @param[in] address 16位EEPROM起始地址。
 * @param[out] data 接收数据的缓冲区。
 * @param[in] size 读取字节数。
 * @return 地址阶段和读取阶段均得到应答时返回 true。
 */
bool bl24c512_read(uint16_t address, uint8_t *data, uint32_t size);

/**
 * @brief 向EEPROM写入连续数据，内部自动按128字节页边界拆分。
 * @param[in] address 16位EEPROM起始地址。
 * @param[in] data 待写数据。
 * @param[in] size 写入字节数。
 * @return 所有页写入并在超时前完成内部编程时返回 true。
 */
bool bl24c512_write(uint16_t address, const uint8_t *data, uint32_t size);

#endif /* CLOCK_F407_BL24C512_H */
