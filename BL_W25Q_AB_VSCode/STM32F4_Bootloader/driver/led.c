/**
 * @file led.c
 * @brief Bootloader状态指示灯的GPIO输出驱动。
 */

#include "led.h"

#include "led_desc.h"
#include "stm32f4xx.h"

void led_init(led_desc_t led)
{
    GPIO_InitTypeDef gpio;

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = led->pin;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(led->port, &gpio);

    GPIO_WriteBit(led->port, led->pin, led->off_level);
}

void led_set(led_desc_t led, bool onoff)
{
    GPIO_WriteBit(led->port, led->pin, onoff ? led->on_level : led->off_level);
}

void led_on(led_desc_t led)
{
    GPIO_WriteBit(led->port, led->pin, led->on_level);
}

void led_off(led_desc_t led)
{
    GPIO_WriteBit(led->port, led->pin, led->off_level);
}
