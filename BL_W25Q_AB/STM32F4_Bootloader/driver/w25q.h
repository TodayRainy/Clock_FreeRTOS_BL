#ifndef __W25Q_H__
#define __W25Q_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 板上两颗共享SPI1的W25Q128。
 */
typedef enum
{
    W25Q_CHIP_A = 0,    /* U31，CS = PE13 */
    W25Q_CHIP_B = 1,    /* U32，CS = PE14 */
    W25Q_CHIP_COUNT,
} w25q_chip_t;

#define W25Q_FLASH_SIZE     (16u * 1024u * 1024u)
#define W25Q_SECTOR_SIZE    4096u
#define W25Q_PAGE_SIZE      256u
#define W25Q_JEDEC_W25Q128  0x00EF4018u

/**
 * @brief 初始化SPI1、PE13/PE14片选并复位两颗W25Q串行接口。
 *
 * 芯片复位命令不擦除阵列数据。
 */
void w25q_init(void);

/** @brief 读取指定芯片的24位JEDEC ID。 */
uint32_t w25q_read_jedec_id(w25q_chip_t chip);

/** @brief 读取状态寄存器1。 */
uint8_t w25q_read_status1(w25q_chip_t chip);

/** @brief 读取状态寄存器2。 */
uint8_t w25q_read_status2(w25q_chip_t chip);

/**
 * @brief 读取64位唯一标识。
 * @param[in] chip 目标芯片。
 * @param[out] uid 接收8字节UID的缓冲区。
 */
void w25q_read_unique_id(w25q_chip_t chip, uint8_t uid[8]);

/**
 * @brief 使用普通读命令读取连续数据。
 * @param[in] chip 目标芯片。
 * @param[in] address 芯片内24位地址。
 * @param[out] data 接收数据的缓冲区。
 * @param[in] size 读取字节数。
 */
void w25q_read(w25q_chip_t chip, uint32_t address, uint8_t *data, uint32_t size);

/**
 * @brief 按4 KiB扇区擦除一段区域。
 * @param[in] chip 目标芯片。
 * @param[in] address 必须4 KiB对齐的起始地址。
 * @param[in] size 必须是4 KiB整数倍的长度。
 * @return 所有扇区在超时前完成擦除时返回 true。
 */
bool w25q_erase(w25q_chip_t chip, uint32_t address, uint32_t size);

/**
 * @brief 连续写入数据，内部自动按256字节页边界拆分。
 *
 * 调用方负责事先擦除目标区域；本函数只等待每次Page Program完成，不做全量
 * 读回校验，固件槽层会在提交前执行CRC32验证。
 *
 * @param[in] chip 目标芯片。
 * @param[in] address 芯片内起始地址。
 * @param[in] data 待写数据。
 * @param[in] size 写入字节数。
 * @return 所有页在超时前完成编程时返回 true。
 */
bool w25q_program(w25q_chip_t chip, uint32_t address, const uint8_t *data, uint32_t size);

#endif /* __W25Q_H__ */
