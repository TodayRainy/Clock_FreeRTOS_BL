/**
 * @file led_desc.h
 * @brief LED GPIO 与有效电平描述。
 */

#ifndef CLOCK_F407_APP_LED_DESC_H
#define CLOCK_F407_APP_LED_DESC_H

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx.h"

struct led_desc
{
    GPIO_TypeDef *port;
    uint32_t pin;
    BitAction on_level;
    BitAction off_level;
};

#endif /* CLOCK_F407_APP_LED_DESC_H */
