/**
 * @file welcome_page.c
 * @brief WeatherClock开机欢迎页面。
 */

#include "page.h"

#include "ui.h"

void welcome_page_display(void)
{
    const uint16_t color_bg = UI_RGB565(0, 0, 0);
    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    ui_draw_image(30, 10, &img_meihua);
    ui_write_string(40, 205, "梅花嵌入式", UI_RGB565(237, 128, 147),
                    color_bg, &font32_maple_bold);
    ui_write_string(56, 233, "天气时钟", UI_RGB565(86, 165, 255),
                    color_bg, &font32_maple_bold);
    ui_write_string(60, 285, "loading...", UI_RGB565(255, 255, 255),
                    color_bg, &font24_maple_bold);
}
