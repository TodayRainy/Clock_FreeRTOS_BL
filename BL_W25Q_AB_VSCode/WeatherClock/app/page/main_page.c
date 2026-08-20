/**
 * @file main_page.c
 * @brief 天气时钟主页面布局和按字段增量刷新。
 *
 * 页面函数只生成UI消息，不直接访问LCD。时间、日期和传感器字段保留各自的显示
 * 缓存，减少周期刷新产生的SPI像素传输。
 */

#include "page.h"

#include <stdio.h>
#include <string.h>

#include "ui.h"
#include "wifi.h"

static const uint16_t color_bg_time = UI_RGB565(248, 248, 248);
static const uint16_t color_bg_inner = UI_RGB565(136, 217, 234);
static const uint16_t color_bg_outdoor = UI_RGB565(254, 135, 75);

void main_page_display(void)
{
    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, UI_RGB565(0, 0, 0));

    ui_fill_color(15, 15, 224, 154, color_bg_time);
    ui_draw_image(23, 20, &icon_wifi);
    main_page_redraw_wifi_ssid(WIFI_SSID);
    ui_write_string(25, 42, "--:--", UI_RGB565(0, 0, 0), color_bg_time,
                    &font76_maple_extrabold);
    ui_write_string(35, 121, "----/--/-- 星期四", UI_RGB565(143, 143, 143),
                    color_bg_time, &font20_maple_bold);

    ui_fill_color(15, 165, 114, 304, color_bg_inner);
    ui_write_string(19, 170, "室内环境", UI_RGB565(0, 0, 0), color_bg_inner,
                    &font24_maple_semibold);
    ui_write_string(86, 191, "C", UI_RGB565(0, 0, 0), color_bg_inner,
                    &font32_maple_bold);
    ui_write_string(91, 262, "%", UI_RGB565(0, 0, 0), color_bg_inner,
                    &font32_maple_bold);
    main_page_redraw_inner_temperature(INT16_MAX);
    main_page_redraw_inner_humidity(UINT16_MAX);

    ui_fill_color(125, 165, 224, 304, color_bg_outdoor);
    ui_write_string(192, 189, "C", UI_RGB565(0, 0, 0), color_bg_outdoor,
                    &font32_maple_bold);
    ui_draw_image(139, 239, &icon_wenduji);
    main_page_redraw_outdoor_city("合肥");
    main_page_redraw_outdoor_temperature(INT16_MAX);
    main_page_redraw_outdoor_weather_icon(-1);
}

void main_page_redraw_wifi_ssid(const char *ssid)
{
    char str[21];
    snprintf(str, sizeof(str), "%20s", ssid);
    ui_write_string(50, 23, str, UI_RGB565(143, 143, 143), color_bg_time, &font16_maple);
}

void main_page_redraw_time(const rtc_date_time_t *time)
{
    static uint8_t last_hour = UINT8_MAX;
    static uint8_t last_minute = UINT8_MAX;
    static char last_separator;
    char str[3];
    char separator;

    /*
     * 76号ASCII字形宽38像素。把HH、冒号和MM分别刷新后，普通的一秒更新
     * 只需要重画位于x=101的冒号，不再重复发送整行HH:MM像素。
     */
    if (time->hour != last_hour)
    {
        str[0] = (char)('0' + (time->hour / 10u) % 10u);
        str[1] = (char)('0' + time->hour % 10u);
        str[2] = '\0';
        ui_write_string(25, 42, str, UI_RGB565(0, 0, 0), color_bg_time, &font76_maple_extrabold);
        last_hour = time->hour;
    }

    if (time->minute != last_minute)
    {
        str[0] = (char)('0' + (time->minute / 10u) % 10u);
        str[1] = (char)('0' + time->minute % 10u);
        str[2] = '\0';
        ui_write_string(139, 42, str, UI_RGB565(0, 0, 0), color_bg_time, &font76_maple_extrabold);
        last_minute = time->minute;
    }

    separator = (time->second % 2u == 0u) ? ':' : ' ';
    if (separator != last_separator)
    {
        str[0] = separator;
        str[1] = '\0';
        ui_write_string(101, 42, str, UI_RGB565(0, 0, 0), color_bg_time, &font76_maple_extrabold);
        last_separator = separator;
    }
}

void main_page_redraw_date(const rtc_date_time_t *date)
{
    char str[18];
    const char *weekday = date->weekday == 1 ? "一" :
                          date->weekday == 2 ? "二" :
                          date->weekday == 3 ? "三" :
                          date->weekday == 4 ? "四" :
                          date->weekday == 5 ? "五" :
                          date->weekday == 6 ? "六" :
                          date->weekday == 7 ? "天" : "X";

    /* 固定版式直接写入数字，避免通用格式化函数反复解析格式字符串。 */
    str[0] = (char)('0' + (date->year / 1000u) % 10u);
    str[1] = (char)('0' + (date->year / 100u) % 10u);
    str[2] = (char)('0' + (date->year / 10u) % 10u);
    str[3] = (char)('0' + date->year % 10u);
    str[4] = '/';
    str[5] = (char)('0' + (date->month / 10u) % 10u);
    str[6] = (char)('0' + date->month % 10u);
    str[7] = '/';
    str[8] = (char)('0' + (date->day / 10u) % 10u);
    str[9] = (char)('0' + date->day % 10u);
    str[10] = ' ';
    memcpy(str + 11, "星期", sizeof("星期") - 1u);
    strcpy(str + 11 + sizeof("星期") - 1u, weekday);
    ui_write_string(35, 121, str, UI_RGB565(143, 143, 143), color_bg_time, &font20_maple_bold);
}

/** @brief 把十分之一摄氏度四舍五入为主界面的整数温度。 */
static int16_t round_temperature(int16_t temperature_x10)
{
    return temperature_x10 >= 0 ? (temperature_x10 + 5) / 10 :
                                  (temperature_x10 - 5) / 10;
}

void main_page_redraw_inner_temperature(int16_t temperature_x10)
{
    char str[3] = {'-', '-', '\0'};
    int16_t temperature = round_temperature(temperature_x10);

    /* 当前版面只为两位字符预留空间，范围外继续显示占位符。 */
    if (temperature >= -9 && temperature <= 99)
        snprintf(str, sizeof(str), "%2d", temperature);
    ui_write_string(30, 192, str, UI_RGB565(0, 0, 0), color_bg_inner, &font54_maple_semibold);
}
    
void main_page_redraw_inner_humidity(uint16_t humidity_x10)
{
    char str[3] = {'-', '-', '\0'};

    if (humidity_x10 <= 1000u)
    {
        /* 两位版面无法容纳100%，因此在最高端显示99而不是截断字符串。 */
        uint16_t humidity = (uint16_t)((humidity_x10 + 5u) / 10u);
        if (humidity > 99u)
            humidity = 99u;
        snprintf(str, sizeof(str), "%2u", (unsigned int)humidity);
    }
    ui_write_string(25, 239, str, UI_RGB565(0, 0, 0), color_bg_inner, &font64_maple_extrabold);
}

void main_page_redraw_outdoor_city(const char *city)
{
    char str[9];
    snprintf(str, sizeof(str), "%s", city);
    ui_write_string(127, 170, str, UI_RGB565(0, 0, 0), color_bg_outdoor, &font24_maple_semibold);
}

void main_page_redraw_outdoor_temperature(int16_t temperature_x10)
{
    char str[3] = {'-', '-', '\0'};
    int16_t temperature = round_temperature(temperature_x10);

    if (temperature >= -9 && temperature <= 99)
        snprintf(str, sizeof(str), "%2d", temperature);
    ui_write_string(135, 190, str, UI_RGB565(0, 0, 0), color_bg_outdoor, &font54_maple_bold);
}

void main_page_redraw_outdoor_weather_icon(int code)
{
    const image_t *icon;
    if (code == 0 || code == 2 || code == 38)
        icon = &icon_qing;
    else if (code == 1 || code == 3)
        icon = &icon_yueliang;
    else if (code == 4 || code == 9)
        icon = &icon_yintian;
    else if (code == 5 || code == 6 || code == 7 || code == 8)
        icon = &icon_duoyun;
    else if (code == 10 || code == 13 || code == 14 || code == 15 ||
             code == 16 || code == 17 || code == 18 || code == 19)
        icon = &icon_zhongyu;
    else if (code == 11 || code == 12)
        icon = &icon_leizhenyu;
    else if (code == 20 || code == 21 || code == 22 || code == 23 ||
             code == 24 || code == 25)
        icon = &icon_zhongxue;
    else /* 扬沙、龙卷风等未单独配图的天气。 */
        icon = &icon_na;
    ui_draw_image(166, 240, icon);
}
