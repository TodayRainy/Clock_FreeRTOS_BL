/**
 * @file key_desc.h
 * @brief 按键 GPIO、EXTI 与回调描述。
 */

#ifndef CLOCK_F407_APP_KEY_DESC_H
#define CLOCK_F407_APP_KEY_DESC_H

#include <stdint.h>
#include "stm32f4xx.h"
#include "key.h"

struct key_desc
{
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t exti_port_src;
    uint8_t exti_pin_src;
    uint32_t exti_line;
    uint8_t irqn;
    key_func_t func;
};

#endif /* CLOCK_F407_APP_KEY_DESC_H */
