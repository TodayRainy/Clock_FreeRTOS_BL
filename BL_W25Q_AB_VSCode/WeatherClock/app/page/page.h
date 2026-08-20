#ifndef CLOCK_F407_PAGE_H
#define CLOCK_F407_PAGE_H

#include <stdint.h>

#include "rtc.h"

/** @brief 绘制开机欢迎页面。 */
void welcome_page_display(void);

/** @brief 绘制不可恢复错误页面。 */
void error_page_display(const char *msg);

/** @brief 绘制Wi-Fi连接等待页面。 */
void wifi_page_display(void);

/** @brief 绘制天气时钟主页面及初始占位内容。 */
void main_page_display(void);

void main_page_redraw_wifi_ssid(const char *ssid);
void main_page_redraw_time(const rtc_date_time_t *time);
void main_page_redraw_date(const rtc_date_time_t *date);
void main_page_redraw_inner_temperature(int16_t temperature_x10);
void main_page_redraw_inner_humidity(uint16_t humidity_x10);
void main_page_redraw_outdoor_city(const char *city);
void main_page_redraw_outdoor_temperature(int16_t temperature_x10);
void main_page_redraw_outdoor_weather_icon(int code);

#endif /* CLOCK_F407_PAGE_H */
