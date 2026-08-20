/**
 * @file error_page.c
 * @brief WeatherClock错误提示页面。
 */

#include "page.h"

#include <string.h>

#include "ui.h"

void error_page_display(const char *msg)
{
    const uint16_t color_bg = UI_RGB565(0, 0, 0);
    uint16_t start_x = 0u;
    uint32_t text_width = strlen(msg) * font20_maple_bold.size / 2u;

    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    ui_draw_image(40, 37, &img_error);

    if (text_width < UI_WIDTH)
        start_x = (uint16_t)((UI_WIDTH - text_width + 1u) / 2u);
    ui_write_string(start_x, 245, msg, UI_RGB565(255, 255, 0), color_bg,
                    &font20_maple_bold);
}
