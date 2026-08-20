/**
 * @file wifi_page.c
 * @brief WeatherClock Wi-Fi连接提示页面。
 */

#include "page.h"

#include <string.h>

#include "ui.h"
#include "wifi.h"

void wifi_page_display(void)
{
    static const char *ssid = WIFI_SSID;
    uint16_t ssid_start_x = 0u;
    uint32_t ssid_width = strlen(ssid) * font20_maple_bold.size / 2u;
    const uint16_t color_bg = UI_RGB565(0, 0, 0);

    if (ssid_width < UI_WIDTH)
        ssid_start_x = (uint16_t)((UI_WIDTH - ssid_width + 1u) / 2u);

    ui_fill_color(0, 0, UI_WIDTH - 1, UI_HEIGHT - 1, color_bg);
    ui_draw_image(30, 15, &img_wifi);
    ui_write_string(88, 191, "WiFi", UI_RGB565(0, 255, 234), color_bg,
                    &font32_maple_bold);
    ui_write_string(ssid_start_x, 231, ssid, UI_RGB565(255, 255, 255),
                    color_bg, &font20_maple_bold);
    ui_write_string(84, 263, "连接中", UI_RGB565(148, 198, 255), color_bg,
                    &font24_maple_bold);
}
