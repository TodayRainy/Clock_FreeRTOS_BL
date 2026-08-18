#ifndef __TIM_DELAY_H__
#define __TIM_DELAY_H__

#include <stdint.h>

typedef void (*tim_periodic_callback_t)(void);

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

/**
 * @brief 注册TIM6周期回调。
 * @param[in] callback 在TIM6更新中断中调用的函数，传NULL表示取消。
 */
void tim_register_periodic_callback(tim_periodic_callback_t callback);

#endif /* __TIM_DELAY_H__ */
