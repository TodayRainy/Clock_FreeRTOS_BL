#ifndef __BL_USART_H__
#define __BL_USART_H__

#include <stdint.h>

typedef void (*bl_usart_rx_callback_t)(const uint8_t *data, uint32_t size);

/** @brief 初始化USART3为115200 8-N-1并开启RXNE中断。 */
void bl_usart_init(void);

/**
 * @brief 轮询发送一段协议数据，并等待最后一个停止位发送完成。
 * @param[in] data 待发送数据。
 * @param[in] size 数据字节数。
 */
void bl_usart_write(const uint8_t *data, uint32_t size);

/**
 * @brief 注册USART3接收中断回调。
 * @param[in] callback 每收到一个字节调用一次的回调。
 */
void bl_usart_register_rx_callback(bl_usart_rx_callback_t callback);

#endif /* __BL_USART_H__ */
