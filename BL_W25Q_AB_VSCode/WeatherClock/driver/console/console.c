/**
 * @file console.c
 * @brief USART1 调试控制台与字节接收回调实现。
 *
 * USART1 RX 在中断中逐字节通知上层，TX 由 board.c 的轮询字符输出供
 * printf 使用。本模块不解析接收数据。
 */

#include "console.h"

#include <stddef.h>
#include <stdint.h>

#include "stm32f4xx.h"

#define CONSOLE_BAUD_RATE             115200u
#define CONSOLE_INTERRUPT_PRIORITY    5u

static console_received_func_t received_func;

/** @brief 配置 USART1 使用的 PA9/PA10 复用引脚。 */
static void console_io_init(void)
{
    GPIO_InitTypeDef gpio_init;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_StructInit(&gpio_init);
    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_Speed = GPIO_High_Speed;
    gpio_init.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_Init(GPIOA, &gpio_init);
}

/** @brief 配置 USART1 参数并开启 RXNE 中断。 */
static void console_usart_init(void)
{
    USART_InitTypeDef usart_init;

    USART_StructInit(&usart_init);
    usart_init.USART_BaudRate = CONSOLE_BAUD_RATE;
    usart_init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart_init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usart_init.USART_Parity = USART_Parity_No;
    usart_init.USART_StopBits = USART_StopBits_1;
    usart_init.USART_WordLength = USART_WordLength_8b;

    USART_Init(USART1, &usart_init);
    USART_ITConfig(USART1, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART1, ENABLE);
}

/** @brief 配置 USART1 接收中断。 */
static void console_interrupt_init(void)
{
    NVIC_InitTypeDef nvic_init;

    nvic_init.NVIC_IRQChannel = USART1_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = CONSOLE_INTERRUPT_PRIORITY;
    nvic_init.NVIC_IRQChannelSubPriority = 0u;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);
    NVIC_SetPriority(USART1_IRQn, CONSOLE_INTERRUPT_PRIORITY);
}

void console_init(void)
{
    console_usart_init();
    console_interrupt_init();
    console_io_init();
}

void console_received_register(console_received_func_t callback)
{
    received_func = callback;
}

void USART1_IRQHandler(void)
{
    if (USART_GetITStatus(USART1, USART_IT_RXNE) == SET)
    {
        /* 读取 DR 才能清除 RXNE；即使没有注册回调也必须消费该字节。 */
        uint8_t byte = (uint8_t)USART_ReceiveData(USART1);

        if (received_func != NULL)
            received_func(byte);
    }
}
