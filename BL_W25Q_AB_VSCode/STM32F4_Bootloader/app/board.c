/**
 * @file board.c
 * @brief Bootloader板级时钟、日志输出和LED/按键描述对象。
 *
 * 各驱动仍负责配置自己的外设寄存器；本模块只集中板级资源映射，避免协议和
 * 状态机直接依赖GPIO端口与引脚。
 */

#include "board.h"

#include <stdint.h>
#include <stdio.h>

#include "stm32f4xx.h"
#include "key_desc.h"
#include "led_desc.h"

void board_lowlevel_init(void)
{
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
}

/**
 * @brief GCC/Newlib 与 ARMCC 共用的串口单字符输出入口。
 * @param ch 待发送字符。
 * @return 已发送的字符。
 */
int __io_putchar(int ch)
{
    USART_SendData(USART1, (uint8_t)ch);
    while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
    return ch;
}

int fputc(int ch, FILE *f)
{
    (void)f;
    return __io_putchar(ch);
}

static struct led_desc led1_desc = {GPIOE, GPIO_Pin_9, Bit_RESET, Bit_SET};
static struct led_desc led2_desc = {GPIOB, GPIO_Pin_0, Bit_RESET, Bit_SET};
static struct led_desc led3_desc = {GPIOB, GPIO_Pin_1, Bit_RESET, Bit_SET};
led_desc_t led1 = &led1_desc;
led_desc_t led2 = &led2_desc;
led_desc_t led3 = &led3_desc;

static struct key_desc key1_desc = {GPIOA, GPIO_Pin_0, GPIO_PuPd_DOWN, Bit_SET};
/*
 * 板上 KEY2 松开时为高电平，按下后把 PC4 拉低。
 * 使用内部上拉可以让按键线路在外部上拉缺失时仍保持确定的松开状态。
 */
static struct key_desc key2_desc = {GPIOC, GPIO_Pin_4, GPIO_PuPd_UP, Bit_RESET};
static struct key_desc key3_desc = {GPIOC, GPIO_Pin_5, GPIO_PuPd_DOWN, Bit_SET};
key_desc_t key1 = &key1_desc;
key_desc_t key2 = &key2_desc;
key_desc_t key3 = &key3_desc;
