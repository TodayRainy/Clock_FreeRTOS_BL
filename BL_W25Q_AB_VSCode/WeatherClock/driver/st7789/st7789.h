/**
 * @file st7789.h
 * @brief ST7789 LCD 绘图接口。
 */

#ifndef WEATHER_CLOCK_ST7789_H
#define WEATHER_CLOCK_ST7789_H

#include <stdbool.h>
#include <stdint.h>
#include "font.h"
#include "image.h"

#define ST7789_WIDTH     240u
#define ST7789_HEIGHT    320u

/** @brief 初始化 LCD 控制器、SPI2、TX DMA 和背光。 */
void st7789_init(void);

/**
 * @brief 用 RGB565 颜色填充闭区间矩形。
 *
 * @param x1 左上角横坐标。
 * @param y1 左上角纵坐标。
 * @param x2 右下角横坐标。
 * @param y2 右下角纵坐标。
 * @param color RGB565 颜色值。
 */
void st7789_fill_color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/**
 * @brief 使用指定点阵字体绘制 GB2312/ASCII 字符串。
 *
 * @param x 起始横坐标。
 * @param y 起始纵坐标。
 * @param text 以零结尾的 GB2312/ASCII 字符串。
 * @param color 前景色。
 * @param bg_color 背景色。
 * @param font 字体描述符。
 */
void st7789_write_string(uint16_t x,
                         uint16_t y,
                         const char *text,
                         uint16_t color,
                         uint16_t bg_color,
                         const font_t *font);

/**
 * @brief 绘制 RGB565 原始或 RLE 压缩图像。
 *
 * @param x 图像左上角横坐标。
 * @param y 图像左上角纵坐标。
 * @param image 图像描述符。
 */
void st7789_draw_image(uint16_t x, uint16_t y, const image_t *image);

#endif /* WEATHER_CLOCK_ST7789_H */
