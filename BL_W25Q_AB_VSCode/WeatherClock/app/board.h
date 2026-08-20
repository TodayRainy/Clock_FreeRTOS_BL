/**
 * @file board.h
 * @brief WeatherClock 板级初始化与板载设备描述。
 */

#ifndef CLOCK_F407_WEATHER_BOARD_H
#define CLOCK_F407_WEATHER_BOARD_H

#include "led.h"

extern led_desc_t led1;
extern led_desc_t led2;
extern led_desc_t led3;

/** @brief 设置应用向量表并开启板级外设时钟。 */
void board_lowlevel_init(void);

/** @brief 初始化应用运行期的时间基准、控制台、RTC和AHT20。 */
void board_init(void);

#endif /* CLOCK_F407_WEATHER_BOARD_H */
