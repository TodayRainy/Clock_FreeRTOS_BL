#ifndef CLOCK_F407_UI_H
#define CLOCK_F407_UI_H

#include <stdint.h>

#include "font.h"
#include "image.h"

#define UI_WIDTH    240u
#define UI_HEIGHT   320u

#define UI_RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/** @brief 创建LCD独占任务及其绘图命令队列。 */
void ui_init(void);

/** @brief 将矩形填充命令复制到UI队列。 */
void ui_fill_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                   uint16_t color);

/** @brief 将文本及其样式复制到UI队列。 */
void ui_write_string(uint16_t x, uint16_t y, const char *str,
                     uint16_t color, uint16_t bg_color, const font_t *font);

/** @brief 将不可变图片资源的绘制命令复制到UI队列。 */
void ui_draw_image(uint16_t x, uint16_t y, const image_t *image);

#endif /* CLOCK_F407_UI_H */
