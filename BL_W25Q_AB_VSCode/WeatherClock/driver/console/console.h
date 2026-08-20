/**
 * @file console.h
 * @brief USART1 调试控制台接口。
 */

#ifndef WEATHER_CLOCK_CONSOLE_H
#define WEATHER_CLOCK_CONSOLE_H

#include <stdint.h>

/**
 * @brief 控制台接收回调类型。
 *
 * 回调直接在 USART1 中断上下文中执行，只能进行短小的非阻塞处理。
 *
 * @param byte 接收到的一个字节。
 */
typedef void (*console_received_func_t)(uint8_t byte);

/** @brief 初始化 USART1 控制台和 RX 中断。 */
void console_init(void);

/**
 * @brief 注册 USART1 字节接收回调。
 *
 * @param callback 中断上下文中调用的回调；传入 NULL 可取消注册。
 */
void console_received_register(console_received_func_t callback);

#endif /* WEATHER_CLOCK_CONSOLE_H */
