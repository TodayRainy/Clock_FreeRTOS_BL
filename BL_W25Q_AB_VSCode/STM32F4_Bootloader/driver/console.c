/**
 * @file console.c
 * @brief Bootloader调试日志使用的USART1轮询输出驱动。
 *
 * USART1只承载日志，不参与USART3固件升级协议，因此这里保留简单的阻塞发送。
 */

#include "console.h"

#include "stm32f4xx.h"

static void console_io_init(void)
{
    GPIO_InitTypeDef gpio;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource9, GPIO_AF_USART1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource10, GPIO_AF_USART1);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_High_Speed;
    gpio.GPIO_Pin = GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_Init(GPIOA, &gpio);
}

static void console_usart_init(void)
{
    USART_InitTypeDef usart;

    USART_StructInit(&usart);
    usart.USART_BaudRate = 115200u;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_WordLength = USART_WordLength_8b;

    USART_Init(USART1, &usart);
    USART_Cmd(USART1, ENABLE);
}

void console_init(void)
{
    console_usart_init();
    console_io_init();
}

void console_write(const char str[], uint32_t length)
{
    uint32_t index;

    for (index = 0u; index < length; ++index)
    {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET);
        USART_SendData(USART1, (uint16_t)str[index]);
    }
}
