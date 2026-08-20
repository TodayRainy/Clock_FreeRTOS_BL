#ifndef CLOCK_F407_LED_H
#define CLOCK_F407_LED_H

#include <stdbool.h>
#include <stdint.h>

struct led_desc;
typedef struct led_desc *led_desc_t;

/** @brief 按描述对象配置LED输出，并先置为熄灭状态。 */
void led_init(led_desc_t led);

/** @brief 根据布尔状态点亮或熄灭LED。 */
void led_set(led_desc_t led, bool onoff);

/** @brief 点亮LED。 */
void led_on(led_desc_t led);

/** @brief 熄灭LED。 */
void led_off(led_desc_t led);

#endif /* CLOCK_F407_LED_H */
