#ifndef __TIM_DELAY_H__
#define __TIM_DELAY_H__

#include <stdint.h>

void tim_delay_init(void);
uint64_t tim_now(void);
uint64_t tim_get_us(void);
uint64_t tim_get_ms(void);
void tim_delay_us(uint32_t us);
void tim_delay_ms(uint32_t ms);

#endif /* __TIM_DELAY_H__ */
