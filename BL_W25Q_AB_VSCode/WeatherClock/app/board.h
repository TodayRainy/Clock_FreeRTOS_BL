/**
 * @file board.h
 * @brief WeatherClock 板级初始化与板载设备描述。
 */

#ifndef __WEATHER_CLOCK_BOARD_H__
#define __WEATHER_CLOCK_BOARD_H__

#include "led.h"

extern led_desc_t led1;
extern led_desc_t led2;
extern led_desc_t led3;

void board_lowlevel_init(void);
void board_init(void);

#endif /* __WEATHER_CLOCK_BOARD_H__ */
