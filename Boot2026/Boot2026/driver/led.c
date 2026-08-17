#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx_ll_gpio.h"
#include "led_desc.h"
#include "led.h"

static void write_pin(GPIO_TypeDef *port, uint32_t pin, uint8_t level)
{
    if (level)
        LL_GPIO_SetOutputPin(port, pin);
    else
        LL_GPIO_ResetOutputPin(port, pin);
}

void led_init(led_desc_t led)
{
    LL_GPIO_InitTypeDef GPIO_InitStructure = {0};

    GPIO_InitStructure.Pin = led->Pin;
    GPIO_InitStructure.Mode = LL_GPIO_MODE_OUTPUT;
    GPIO_InitStructure.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStructure.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStructure.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(led->Port, &GPIO_InitStructure);

    write_pin(led->Port, led->Pin, led->OffBit);
}

void led_set(led_desc_t led, bool onoff)
{
    write_pin(led->Port, led->Pin, onoff ? led->OnBit : led->OffBit);
}

void led_on(led_desc_t led)
{
    write_pin(led->Port, led->Pin, led->OnBit);
}

void led_off(led_desc_t led)
{
    write_pin(led->Port, led->Pin, led->OffBit);
}
