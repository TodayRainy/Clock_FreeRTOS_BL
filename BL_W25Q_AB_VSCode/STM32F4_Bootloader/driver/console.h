#ifndef CLOCK_F407_CONSOLE_H
#define CLOCK_F407_CONSOLE_H

#include <stdint.h>

/** @brief 初始化USART1调试日志控制台。 */
void console_init(void);

/**
 * @brief 轮询发送一段调试日志。
 * @param[in] str 待发送的字符缓冲区。
 * @param[in] length 字符数。
 */
void console_write(const char str[], uint32_t length);

#endif /* CLOCK_F407_CONSOLE_H */
