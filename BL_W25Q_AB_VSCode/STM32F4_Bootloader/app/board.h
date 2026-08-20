#ifndef CLOCK_F407_BOARD_H
#define CLOCK_F407_BOARD_H

#include "led.h"
#include "key.h"

extern led_desc_t led1;
extern led_desc_t led2;
extern led_desc_t led3;

extern key_desc_t key1;
extern key_desc_t key2;
extern key_desc_t key3;

/**
 * @brief 开启Bootloader使用的GPIO、DMA、USART和TIM外设时钟。
 *
 * 本函数只准备公共时钟，具体引脚和外设参数由各驱动初始化函数配置。
 */
void board_lowlevel_init(void);

#endif /* CLOCK_F407_BOARD_H */
