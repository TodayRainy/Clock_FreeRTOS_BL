/**
 * @file key.c
 * @brief WeatherClock按键GPIO与EXTI描述驱动。
 *
 * 本驱动只配置单个描述对象并保存回调；具体EXTI中断入口由板级代码决定。
 */

#include "key.h"

#include "key_desc.h"
#include "stm32f4xx.h"

void key_init(key_desc_t key)
{
    GPIO_InitTypeDef gpio;
    EXTI_InitTypeDef exti;
    NVIC_InitTypeDef nvic;

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_IN;
    gpio.GPIO_PuPd = GPIO_PuPd_DOWN;
    gpio.GPIO_Speed = GPIO_Medium_Speed;
    gpio.GPIO_Pin = key->pin;
    GPIO_Init(key->port, &gpio);

    SYSCFG_EXTILineConfig(key->exti_port_src, key->exti_pin_src);

    EXTI_StructInit(&exti);
    exti.EXTI_Mode = EXTI_Mode_Interrupt;
    exti.EXTI_Trigger = EXTI_Trigger_Rising;
    exti.EXTI_LineCmd = ENABLE;
    exti.EXTI_Line = key->exti_line;
    EXTI_Init(&exti);

    nvic.NVIC_IRQChannelPreemptionPriority = 5u;
    nvic.NVIC_IRQChannelSubPriority = 0u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    nvic.NVIC_IRQChannel = key->irqn;
    NVIC_Init(&nvic);
}

bool key_read(key_desc_t key)
{
    return GPIO_ReadInputDataBit(key->port, key->pin) == Bit_SET;
}

void key_press_callback_register(key_desc_t key, key_func_t func)
{
    key->func = func;
}
