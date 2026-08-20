/**
 * @file led.h
 * @brief GPIO LED 控制接口。
 */

#ifndef CLOCK_F407_APP_LED_H
#define CLOCK_F407_APP_LED_H

#include <stdbool.h>
#include <stdint.h>

struct led_desc;
typedef struct led_desc *led_desc_t;

/** @brief 配置 LED GPIO 并先置为熄灭电平。 */
void led_init(led_desc_t led);

/** @brief 根据布尔状态点亮或熄灭 LED。 */
void led_set(led_desc_t led, bool on);

/** @brief 点亮 LED。 */
void led_on(led_desc_t led);

/** @brief 熄灭 LED。 */
void led_off(led_desc_t led);

#endif /* CLOCK_F407_APP_LED_H */
