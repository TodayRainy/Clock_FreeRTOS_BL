#ifndef CLOCK_F407_TIM_DELAY_H
#define CLOCK_F407_TIM_DELAY_H

#include <stdint.h>

/** @brief 初始化1 MHz自由运行的TIM6时间基准。 */
void tim_delay_init(void);

/** @brief 返回TIM6扩展后的微秒计数。 */
uint64_t tim_now(void);

/** @brief 返回自计时器初始化后的微秒数。 */
uint64_t tim_get_us(void);

/** @brief 返回自计时器初始化后的毫秒数。 */
uint64_t tim_get_ms(void);

/** @brief 忙等待指定微秒数。 */
void tim_delay_us(uint32_t us);

/** @brief 忙等待指定毫秒数。 */
void tim_delay_ms(uint32_t ms);

#endif /* CLOCK_F407_TIM_DELAY_H */
