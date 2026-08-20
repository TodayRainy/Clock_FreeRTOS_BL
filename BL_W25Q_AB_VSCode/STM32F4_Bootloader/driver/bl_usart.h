#ifndef CLOCK_F407_BL_USART_H
#define CLOCK_F407_BL_USART_H

#include <stdbool.h>
#include <stdint.h>

#define BL_USART_DEFAULT_BAUD_RATE 115200u

/**
 * @brief 初始化USART3及其循环接收DMA。
 *
 * USART3使用PB10/TX、PB11/RX和8-N-1格式。每次复位均从115200 baud
 * 开始，收到SET_BAUD命令后才切换到高速档。接收数据由DMA1 Stream1
 * 搬运到驱动内部的循环缓冲区，协议层无需逐字节响应中断。
 */
void bl_usart_init(void);

/**
 * @brief 关闭USART3接收DMA和外设。
 *
 * 跳转应用前调用，避免DMA继续改写Bootloader的RAM，也把USART3留给应用
 * 按自己的配置重新初始化。
 */
void bl_usart_deinit(void);

/**
 * @brief 在协议已经应答后切换USART3波特率。
 *
 * 本函数会等待当前响应的停止位发送完成，再短暂关闭USART、更新BRR并恢复
 * 循环DMA接收请求。Bootloader每次复位仍由 @ref bl_usart_init 从115200开始。
 *
 * @param[in] baud_rate 新波特率。
 */
void bl_usart_set_baud(uint32_t baud_rate);

/**
 * @brief 轮询发送一段协议数据，并等待最后一个停止位发送完成。
 * @param[in] data 待发送数据。
 * @param[in] size 数据字节数。
 */
void bl_usart_write(const uint8_t *data, uint32_t size);

/**
 * @brief 从循环DMA缓冲区取出一个字节。
 * @param[out] data 接收字节的存放地址。
 * @return 有数据可取时返回true，否则返回false。
 */
bool bl_usart_read_byte(uint8_t *data);

/**
 * @brief 判断循环DMA缓冲区中是否存在尚未处理的数据。
 * @return 至少有一个字节待处理时返回true。
 */
bool bl_usart_data_pending(void);

#endif /* CLOCK_F407_BL_USART_H */
