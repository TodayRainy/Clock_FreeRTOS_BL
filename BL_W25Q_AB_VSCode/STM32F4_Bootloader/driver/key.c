/**
 * @file key.c
 * @brief Bootloader板级按键的GPIO输入驱动。
 */

#include "key.h"

#include "key_desc.h"
#include "stm32f4xx.h"

void key_init(key_desc_t key)
{
    GPIO_InitTypeDef gpio;

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_IN;
    gpio.GPIO_PuPd = key->pupd;
    gpio.GPIO_Speed = GPIO_Medium_Speed;
    gpio.GPIO_Pin = key->pin;
    GPIO_Init(key->port, &gpio);
}

bool key_read(key_desc_t key)
{
    return GPIO_ReadInputDataBit(key->port, key->pin) == key->press_level;
}
