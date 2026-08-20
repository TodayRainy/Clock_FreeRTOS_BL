#ifndef CLOCK_F407_KEY_DESC_H
#define CLOCK_F407_KEY_DESC_H

#include <stdint.h>
#include "stm32f4xx.h"
#include "key.h"

struct key_desc
{
    GPIO_TypeDef *port;
    uint16_t pin;
    GPIOPuPd_TypeDef pupd;
    BitAction press_level;
};

#endif /* CLOCK_F407_KEY_DESC_H */
