#include <stdint.h>
#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "tim_delay.h"
#include "console.h"
#include "rtc.h"
#include "aht20.h"
#include "board.h"
#include "led_desc.h"

/* 三颗板载 LED 均为低电平点亮。 */
static struct led_desc board_led1 = { GPIOE, GPIO_Pin_9, Bit_RESET, Bit_SET };
static struct led_desc board_led2 = { GPIOB, GPIO_Pin_0, Bit_RESET, Bit_SET };
static struct led_desc board_led3 = { GPIOB, GPIO_Pin_1, Bit_RESET, Bit_SET };
led_desc_t led1 = &board_led1;
led_desc_t led2 = &board_led2;
led_desc_t led3 = &board_led3;

void board_lowlevel_init(void)
{
    SCB->VTOR = 0x08010000;
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOD, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOE, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_I2C2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA2, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    RCC_LSEConfig(RCC_LSE_ON);
    while(RCC_GetFlagStatus(RCC_FLAG_LSERDY) == RESET);
    RCC_RTCCLKConfig(RCC_RTCCLKSource_LSE);
}

void board_init(void)
{
    tim_delay_init();
    console_init();
    printf("[SYS] Build Date: %s %s\n", __DATE__, __TIME__);
    
    rtc_init();
    aht20_init();
}

/**
 * @brief GCC/Newlib 与 ARMCC 共用的 USART1 单字符输出入口。
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

void vAssertCalled(const char *file, int line)
{
    portDISABLE_INTERRUPTS();
    printf("Assert Called: %s(%d)\n", file, line);

    /* 断言后禁止继续执行；保持不喂狗，由IWDG完成系统复位。 */
    while (1)
    {
        ;
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    printf("Stack Overflowed: %s\n", pcTaskName);
    configASSERT(0);
}

void vApplicationMallocFailedHook(void)
{
    printf("Malloc Failed\n");
    configASSERT(0);
}
