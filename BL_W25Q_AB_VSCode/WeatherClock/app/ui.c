/**
 * @file ui.c
 * @brief 串行化LCD绘图操作的FreeRTOS消息队列和独占任务。
 *
 * 业务任务只提交值类型命令，唯一UI任务负责访问ST7789。文本被复制进消息，图片
 * 和字体只保存指向静态只读资源的指针，因此队列中不存在临时对象生命周期问题。
 */

#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include "font.h"
#include "image.h"
#include "st7789.h"

#define UI_TEXT_CAPACITY 40u

typedef enum
{
    UI_ACTION_FILL_COLOR,
    UI_ACTION_WRITE_STRING,
    UI_ACTION_DRAW_IMAGE,
} ui_action_t;

typedef struct
{
    ui_action_t action;
    union
    {
        struct
        {
            uint16_t x;
            uint16_t y;
            uint16_t width;
            uint16_t height;
            uint16_t color;
        } fill_color;
        struct
        {
            uint16_t x;
            uint16_t y;
            char str[UI_TEXT_CAPACITY];
            uint16_t color;
            uint16_t bg_color;
            const font_t *font;
        } write_string;
        struct
        {
            uint16_t x;
            uint16_t y;
            const image_t *image;
        } draw_image;
    };
} ui_message_t;

static QueueHandle_t ui_queue;

static void ui_task(void *parameter)
{
    ui_message_t message;

    (void)parameter;
    st7789_init();

    while (1)
    {
        xQueueReceive(ui_queue, &message, portMAX_DELAY);

        switch (message.action)
        {
            case UI_ACTION_FILL_COLOR:
                st7789_fill_color(message.fill_color.x,
                                  message.fill_color.y,
                                  message.fill_color.width,
                                  message.fill_color.height,
                                  message.fill_color.color);
                break;

            case UI_ACTION_WRITE_STRING:
                st7789_write_string(message.write_string.x,
                                    message.write_string.y,
                                    message.write_string.str,
                                    message.write_string.color,
                                    message.write_string.bg_color,
                                    message.write_string.font);
                break;

            case UI_ACTION_DRAW_IMAGE:
                st7789_draw_image(message.draw_image.x,
                                  message.draw_image.y,
                                  message.draw_image.image);
                break;

            default:
                printf("Unknown UI action: %d\n", message.action);
                break;
        }
    }
}

void ui_init(void)
{
    ui_queue = xQueueCreate(16, sizeof(ui_message_t));
    configASSERT(ui_queue != NULL);
    configASSERT(xTaskCreate(ui_task, "ui", 1024, NULL, 8, NULL) == pdPASS);
}

void ui_fill_color(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                   uint16_t color)
{
    ui_message_t message;

    message.action = UI_ACTION_FILL_COLOR;
    message.fill_color.x = x;
    message.fill_color.y = y;
    message.fill_color.width = width;
    message.fill_color.height = height;
    message.fill_color.color = color;

    xQueueSend(ui_queue, &message, portMAX_DELAY);
}

void ui_write_string(uint16_t x, uint16_t y, const char *str,
                     uint16_t color, uint16_t bg_color, const font_t *font)
{
    ui_message_t message;

    message.action = UI_ACTION_WRITE_STRING;
    message.write_string.x = x;
    message.write_string.y = y;
    /* UI现有文本最长为32字节SSID，直接随队列消息复制，避免周期刷新反复申请堆。 */
    strncpy(message.write_string.str, str,
            sizeof(message.write_string.str) - 1u);
    message.write_string.str[sizeof(message.write_string.str) - 1u] = '\0';
    message.write_string.color = color;
    message.write_string.bg_color = bg_color;
    message.write_string.font = font;

    xQueueSend(ui_queue, &message, portMAX_DELAY);
}

void ui_draw_image(uint16_t x, uint16_t y, const image_t *image)
{
    ui_message_t message;

    message.action = UI_ACTION_DRAW_IMAGE;
    message.draw_image.x = x;
    message.draw_image.y = y;
    message.draw_image.image = image;

    xQueueSend(ui_queue, &message, portMAX_DELAY);
}
