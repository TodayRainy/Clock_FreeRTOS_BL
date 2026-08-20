#include <stdint.h>
#include "stm32f4xx.h"
#include "tim_delay.h"

#define TIM6_COUNTER_PERIOD_US 65536u

static volatile uint32_t tim_overflow_count;

void tim_delay_init(void)
{
    RCC_ClocksTypeDef RCC_ClocksStruct;
    RCC_GetClocksFreq(&RCC_ClocksStruct);
    uint32_t apb1_tim_freq_mhz = RCC_ClocksStruct.PCLK1_Frequency / 1000 / 1000 * 2;
    
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    TIM_TimeBaseStructInit(&TIM_TimeBaseStructure);
    TIM_TimeBaseStructure.TIM_Prescaler = apb1_tim_freq_mhz - 1;
    TIM_TimeBaseStructure.TIM_Period = TIM6_COUNTER_PERIOD_US - 1u;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM6, &TIM_TimeBaseStructure);
    TIM_SetCounter(TIM6, 0u);
    TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);
    
    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = TIM6_DAC_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 5;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    tim_overflow_count = 0u;
    TIM_Cmd(TIM6, ENABLE);
}

uint64_t tim_now(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t overflows;
    uint16_t counter;

    /* 读取期间短暂屏蔽中断，避免16位计数器溢出时得到前后两代数据。 */
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
    uint64_t now = tim_now();
    while (tim_now() - now < (uint64_t)us);
}

void tim_delay_ms(uint32_t ms)
{
    uint64_t now = tim_now();
    while (tim_now() - now < (uint64_t)ms * 1000);
}

void TIM6_DAC_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM6, TIM_IT_Update) != RESET)
    {
        TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
        ++tim_overflow_count;
    }
}
