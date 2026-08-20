/**
 * @file tim_delay.c
 * @brief 基于TIM6的WeatherClock微秒时间基准和短时忙等待。
 *
 * 16位硬件计数器以1 MHz运行，更新中断扩展高位。驱动层的短硬件时序使用
 * 该计数；任务级等待仍应使用FreeRTOS延时。
 */

#include "tim_delay.h"

#include "stm32f4xx.h"

#define TIM6_COUNTER_PERIOD_US 65536u

static volatile uint32_t tim_overflow_count;

void tim_delay_init(void)
{
    RCC_ClocksTypeDef clocks;
    TIM_TimeBaseInitTypeDef time_base;
    NVIC_InitTypeDef nvic;
    uint32_t apb1_timer_frequency_mhz;

    RCC_GetClocksFreq(&clocks);
    apb1_timer_frequency_mhz = clocks.PCLK1_Frequency / 1000u / 1000u * 2u;

    TIM_TimeBaseStructInit(&time_base);
    time_base.TIM_Prescaler = apb1_timer_frequency_mhz - 1u;
    time_base.TIM_Period = TIM6_COUNTER_PERIOD_US - 1u;
    time_base.TIM_ClockDivision = TIM_CKD_DIV1;
    time_base.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM6, &time_base);
    TIM_SetCounter(TIM6, 0u);
    TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM6_DAC_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 5u;
    nvic.NVIC_IRQChannelSubPriority = 0u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    tim_overflow_count = 0u;
    TIM_Cmd(TIM6, ENABLE);
}

uint64_t tim_now(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t overflows;
    uint16_t counter;

    /*
     * 读取期间短暂屏蔽中断。若更新标志已经置位但ISR尚未执行，本次快照先补上
     * 该次溢出，避免返回的高低位来自计数器的前后两代。
     */
    __disable_irq();
    overflows = tim_overflow_count;
    counter = (uint16_t)TIM6->CNT;
    if ((TIM6->SR & TIM_IT_Update) != 0u)
    {
        ++overflows;
        counter = (uint16_t)TIM6->CNT;
    }
    __set_PRIMASK(primask);

    return ((uint64_t)overflows << 16) + counter;
}

uint64_t tim_get_us(void)
{
    return tim_now();
}

uint64_t tim_get_ms(void)
{
    return tim_now() / 1000;
}

void tim_delay_us(uint32_t us)
{
    uint64_t start = tim_now();

    while (tim_now() - start < (uint64_t)us);
}

void tim_delay_ms(uint32_t ms)
{
    uint64_t start = tim_now();

    while (tim_now() - start < (uint64_t)ms * 1000u);
}

void TIM6_DAC_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM6, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
        ++tim_overflow_count;
    }
}
