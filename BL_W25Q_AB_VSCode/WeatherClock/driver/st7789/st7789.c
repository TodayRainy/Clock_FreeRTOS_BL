/**
 * @file st7789.c
 * @brief ST7789 LCD 的 SPI2/DMA 驱动与基础绘图实现。
 *
 * 寄存器命令使用 8 位 SPI 轮询发送，连续像素使用 16 位 SPI 和 DMA1 Stream4。
 * UI 任务串行调用本模块，因此字体展开与 RLE 解码可共用一个静态缓冲区。
 */

#include "st7789.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx.h"
#include "task.h"

#define CS_PORT       GPIOE
#define CS_PIN        GPIO_Pin_2
#define RESET_PORT    GPIOE
#define RESET_PIN     GPIO_Pin_3
#define DC_PORT       GPIOE
#define DC_PIN        GPIO_Pin_4
#define BL_PORT       GPIOE
#define BL_PIN        GPIO_Pin_5

#define ST7789_DMA_MAX_TRANSFER      65535u
#define ST7789_DMA_IRQ_PRIORITY      5u

/* 当前最大字形是 76 像素字体的一个 ASCII 字符，缓冲区同时供 RLE 解码使用。 */
#define ST7789_DRAW_BUFFER_SIZE      (38u * 76u * 2u)

static SemaphoreHandle_t write_gram_semaphore;
static uint8_t draw_buffer[ST7789_DRAW_BUFFER_SIZE] __attribute__((aligned(4)));

static void st7789_init_display(void);

/** @brief 配置 LCD 控制引脚与 SPI2 引脚复用。 */
static void st7789_io_init(void)
{
    GPIO_InitTypeDef gpio_init;

    GPIO_StructInit(&gpio_init);
    
    GPIO_SetBits(GPIOE, GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5);
    GPIO_ResetBits(BL_PORT, BL_PIN);
    
    gpio_init.GPIO_Mode = GPIO_Mode_OUT;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_High_Speed;
    gpio_init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5;
    GPIO_Init(GPIOE, &gpio_init);
    
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource13, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource2, GPIO_AF_SPI2);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource3, GPIO_AF_SPI2);

    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_High_Speed;
    gpio_init.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio_init.GPIO_Pin = GPIO_Pin_13;
    GPIO_Init(GPIOB, &gpio_init);
    gpio_init.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOC, &gpio_init);
}

/** @brief 配置 SPI2 为 8 位主机模式并允许 TX DMA 请求。 */
static void st7789_spi_init(void)
{
    SPI_InitTypeDef spi_init;

    SPI_StructInit(&spi_init);
    spi_init.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi_init.SPI_Mode = SPI_Mode_Master;
    spi_init.SPI_DataSize = SPI_DataSize_8b;
    spi_init.SPI_CPOL = SPI_CPOL_Low;
    spi_init.SPI_CPHA = SPI_CPHA_1Edge;
    spi_init.SPI_NSS = SPI_NSS_Soft;
    spi_init.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    spi_init.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(SPI2, &spi_init);
    SPI_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);
    SPI_Cmd(SPI2, ENABLE);
}

/** @brief 配置 SPI2 TX 对应的 DMA1 Stream4/Channel0。 */
static void st7789_dma_init(void)
{
    DMA_InitTypeDef dma_init;

    DMA_StructInit(&dma_init);
    dma_init.DMA_Channel = DMA_Channel_0;
    dma_init.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    dma_init.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    dma_init.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma_init.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    dma_init.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    dma_init.DMA_Mode = DMA_Mode_Normal;
    dma_init.DMA_Priority = DMA_Priority_High;
    dma_init.DMA_FIFOMode = DMA_FIFOMode_Enable;
    dma_init.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    dma_init.DMA_MemoryBurst = DMA_MemoryBurst_INC8;
    dma_init.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_ITConfig(DMA1_Stream4, DMA_IT_TC, ENABLE);
    DMA_Init(DMA1_Stream4, &dma_init);
}

/** @brief 配置 SPI2 TX DMA 完成中断。 */
static void st7789_int_init(void)
{
    NVIC_InitTypeDef nvic_init;

    nvic_init.NVIC_IRQChannel = DMA1_Stream4_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = ST7789_DMA_IRQ_PRIORITY;
    nvic_init.NVIC_IRQChannelSubPriority = 0u;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);
    NVIC_SetPriority(DMA1_Stream4_IRQn, ST7789_DMA_IRQ_PRIORITY);
}

void st7789_init(void)
{
    write_gram_semaphore = xSemaphoreCreateBinary();
    configASSERT(write_gram_semaphore != NULL);
    
    st7789_spi_init();
    st7789_dma_init();
    st7789_int_init();
    st7789_io_init();
    
    st7789_init_display();
}

/** @brief 以 8 位 SPI 时序写一个控制器寄存器及其参数。 */
static void st7789_write_register(uint8_t reg, const uint8_t *data, uint16_t length)
{
    SPI_DataSizeConfig(SPI2, SPI_DataSize_8b);
    
    GPIO_ResetBits(CS_PORT, CS_PIN);
    
    GPIO_ResetBits(DC_PORT, DC_PIN);
    SPI_SendData(SPI2, reg);
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
    {
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
    {
    }
    
    GPIO_SetBits(DC_PORT, DC_PIN);
    for (uint16_t index = 0u; index < length; index++)
    {
        SPI_SendData(SPI2, data[index]);
        while (SPI_GetFlagStatus(SPI2, SPI_FLAG_TXE) == RESET)
        {
        }
    }
    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
    {
    }
    
    GPIO_SetBits(CS_PORT, CS_PIN);
}

/**
 * @brief 使用 16 位 SPI 和 DMA 向已设置的 GRAM 窗口发送像素。
 *
 * @param data RGB565 数据；single_color 为 true 时只读取首个颜色。
 * @param length 数据字节数，必须为偶数。
 * @param single_color 是否重复发送同一个颜色。
 */
static void st7789_write_gram(const uint8_t *data, uint32_t length, bool single_color)
{
    uint32_t pixels = length / 2u;

    SPI_DataSizeConfig(SPI2, SPI_DataSize_16b);

    GPIO_ResetBits(CS_PORT, CS_PIN);
    GPIO_SetBits(DC_PORT, DC_PIN);

    while (pixels > 0u)
    {
        uint32_t chunk_size = pixels < ST7789_DMA_MAX_TRANSFER
                                  ? pixels
                                  : ST7789_DMA_MAX_TRANSFER;

        if (single_color)
            DMA1_Stream4->CR &= ~DMA_SxCR_MINC;
        else
            DMA1_Stream4->CR |= DMA_SxCR_MINC;
        DMA1_Stream4->M0AR = (uint32_t)data;
        DMA1_Stream4->NDTR = chunk_size;

        DMA_Cmd(DMA1_Stream4, ENABLE);
        xSemaphoreTake(write_gram_semaphore, portMAX_DELAY);

        if (!single_color)
            data += chunk_size * 2u;
        pixels -= chunk_size;
    }

    while (SPI_GetFlagStatus(SPI2, SPI_FLAG_BSY) != RESET)
    {
    }

    GPIO_SetBits(CS_PORT, CS_PIN);
}

static void st7789_reset(void)
{
    GPIO_ResetBits(RESET_PORT, RESET_PIN);
    /* 控制器要求复位低脉冲至少 20 us；任务延时取整为 2 ms。 */
    vTaskDelay(pdMS_TO_TICKS(2u));
    GPIO_SetBits(RESET_PORT, RESET_PIN);
    vTaskDelay(pdMS_TO_TICKS(120u));
}

static void st7789_set_backlight(bool on)
{
    GPIO_WriteBit(BL_PORT, BL_PIN, on ? Bit_SET : Bit_RESET);
}

static void st7789_init_display(void)
{
    st7789_reset();
    
    st7789_write_register(0x11u, NULL, 0u);
    vTaskDelay(pdMS_TO_TICKS(5u));
    
    st7789_write_register(0x36u, (const uint8_t[]){0x00u}, 1u);
    st7789_write_register(0x3Au, (const uint8_t[]){0x55u}, 1u);
    st7789_write_register(0xB7u, (const uint8_t[]){0x46u}, 1u);
    st7789_write_register(0xBBu, (const uint8_t[]){0x1Bu}, 1u);
    st7789_write_register(0xC0u, (const uint8_t[]){0x2Cu}, 1u);
    st7789_write_register(0xC2u, (const uint8_t[]){0x01u}, 1u);
    st7789_write_register(0xC4u, (const uint8_t[]){0x20u}, 1u);
    st7789_write_register(0xC6u, (const uint8_t[]){0x0Fu}, 1u);
    st7789_write_register(0xD0u, (const uint8_t[]){0xA4u, 0xA1u}, 2u);
    st7789_write_register(0xD6u, (const uint8_t[]){0xA1u}, 1u);
    st7789_write_register(
        0xE0u,
        (const uint8_t[]){0xF0u, 0x00u, 0x06u, 0x04u, 0x05u, 0x05u, 0x31u,
                          0x44u, 0x48u, 0x36u, 0x12u, 0x12u, 0x2Bu, 0x34u},
        14u);
    st7789_write_register(
        0xE0u,
        (const uint8_t[]){0xF0u, 0x0Bu, 0x0Fu, 0x0Fu, 0x0Du, 0x26u, 0x31u,
                          0x43u, 0x47u, 0x38u, 0x14u, 0x14u, 0x2Cu, 0x32u},
        14u);
    st7789_write_register(0x21u, NULL, 0u);
    st7789_write_register(0x29u, NULL, 0u);
    
    st7789_fill_color(0u, 0u, ST7789_WIDTH - 1u, ST7789_HEIGHT - 1u, 0x0000u);
    st7789_set_backlight(true);
}

/** @brief 判断闭区间矩形是否完整位于屏幕范围内。 */
static bool in_screen_range(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    if (x1 >= ST7789_WIDTH || y1 >= ST7789_HEIGHT)
        return false;
    if (x2 >= ST7789_WIDTH || y2 >= ST7789_HEIGHT)
        return false;
    if (x1 > x2 || y1 > y2)
        return false;

    return true;
}

/** @brief 设置 LCD 写入窗口并进入 GRAM 写入状态。 */
static void st7789_set_range_and_prepare_gram(uint16_t x1,
                                               uint16_t y1,
                                               uint16_t x2,
                                               uint16_t y2)
{
    const uint8_t columns[] = {
        (uint8_t)(x1 >> 8u), (uint8_t)x1, (uint8_t)(x2 >> 8u), (uint8_t)x2};
    const uint8_t rows[] = {
        (uint8_t)(y1 >> 8u), (uint8_t)y1, (uint8_t)(y2 >> 8u), (uint8_t)y2};

    st7789_write_register(0x2Au, columns, sizeof(columns));
    st7789_write_register(0x2Bu, rows, sizeof(rows));
    st7789_write_register(0x2Cu, NULL, 0u);
}

void st7789_fill_color(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint32_t pixels;

    if (!in_screen_range(x1, y1, x2, y2))
        return;
    
    st7789_set_range_and_prepare_gram(x1, y1, x2, y2);

    pixels = (uint32_t)(x2 - x1 + 1u) * (y2 - y1 + 1u);
    st7789_write_gram((const uint8_t *)&color, pixels * 2u, true);
}

/** @brief 将一个字形展开为 RGB565 像素后通过 DMA 绘制。 */
static void st7789_draw_font(uint16_t x,
                             uint16_t y,
                             uint16_t width,
                             uint16_t height,
                             const uint8_t *model,
                             uint16_t color,
                             uint16_t bg_color)
{
    uint16_t bytes_per_row = (width + 7u) / 8u;
    uint32_t image_bytes = (uint32_t)width * height * 2u;
    uint8_t *buffer = draw_buffer;
    
    if (image_bytes > sizeof(draw_buffer))
        return;

    for (uint16_t row = 0u; row < height; row++)
    {
        const uint8_t *row_data = model + row * bytes_per_row;

        for (uint16_t column = 0u; column < width; column++)
        {
            uint8_t pixel = row_data[column / 8u] & (1u << (7u - column % 8u));
            uint16_t pixel_color = pixel ? color : bg_color;

            *buffer++ = (uint8_t)pixel_color;
            *buffer++ = (uint8_t)(pixel_color >> 8u);
        }
    }
    
    st7789_set_range_and_prepare_gram(x, y, x + width - 1u, y + height - 1u);
    st7789_write_gram(draw_buffer, (uint32_t)(buffer - draw_buffer), false);
}

/** @brief 查找 ASCII 字符对应的字模。 */
static const uint8_t *ascii_get_model(char character, const font_t *font)
{
    uint16_t bytes_per_row = (font->size / 2u + 7u) / 8u;
    uint16_t bytes_per_char = font->size * bytes_per_row;
    
    if (font->ascii_map != NULL)
    {
        for (const char *map = font->ascii_map; *map != '\0'; map++)
        {
            if (*map == character)
            {
                return font->ascii_model + (map - font->ascii_map) * bytes_per_char;
            }
        }
    }
    else
    {
        return font->ascii_model + (character - ' ') * bytes_per_char;
    }
    
    return NULL;
}

/** @brief 绘制一个 ASCII 字符。 */
static void st7789_write_ascii(uint16_t x,
                               uint16_t y,
                               char character,
                               uint16_t color,
                               uint16_t bg_color,
                               const font_t *font)
{
    uint16_t font_height = font->size;
    uint16_t font_width = font->size / 2u;
    const uint8_t *model;

    if (!in_screen_range(x, y, x + font_width - 1u, y + font_height - 1u))
        return;
    
    if (character < 0x20 || character > 0x7E)
        return;

    model = ascii_get_model(character, font);
    if (model != NULL)
        st7789_draw_font(x, y, font_width, font_height, model, color, bg_color);
}

/** @brief 查找并绘制一个双字节 GB2312 汉字。 */
static void st7789_write_chinese(uint16_t x,
                                 uint16_t y,
                                 const char *character,
                                 uint16_t color,
                                 uint16_t bg_color,
                                 const font_t *font)
{
    uint16_t font_height = font->size;
    uint16_t font_width = font->size;
    const font_chinese_t *chinese = font->chinese;

    if (!in_screen_range(x, y, x + font_width - 1u, y + font_height - 1u))
        return;

    for (; chinese->name != NULL; chinese++)
    {
        if (strcmp(chinese->name, character) == 0)
            break;
    }
    if (chinese->name == NULL)
        return;

    st7789_draw_font(x, y, font_width, font_height, chinese->model, color, bg_color);
}

/** @brief 判断一个字节是否为 GB2312 双字节字符的首字节。 */
static bool is_gb2312_lead_byte(char byte)
{
    return (uint8_t)byte >= 0xA1u && (uint8_t)byte <= 0xF7u;
}

void st7789_write_string(uint16_t x,
                         uint16_t y,
                         const char *text,
                         uint16_t color,
                         uint16_t bg_color,
                         const font_t *font)
{
    if (text == NULL || font == NULL)
        return;

    while (*text != '\0')
    {
        if (!is_gb2312_lead_byte(*text))
        {
            st7789_write_ascii(x, y, *text, color, bg_color, font);
            text++;
            x += font->size / 2u;
        }
        else
        {
            /* 字形名称是两个字节的 C 字符串，末尾必须显式补零。 */
            char character[3] = {0};

            memcpy(character, text, 2u);
            st7789_write_chinese(x, y, character, color, bg_color, font);
            text += 2;
            x += font->size;
        }
    }
}

/**
 * @brief 将 PackBits 风格的 RGB565 数据直接展开到 LCD 窗口。
 *
 * 共用绘图缓冲区可保持较大的 DMA 块，同时避免额外常驻 RAM。所有绘图均由
 * ui_task 串行执行，字体绘制和图像解码不会并发使用该缓冲区。
 */
static void st7789_draw_rle_image(const image_t *image)
{
    const uint8_t *source = image->data;
    const uint8_t *end = source + image->data_size;
    uint32_t pixels_remaining = (uint32_t)image->width * image->height;
    uint32_t buffered = 0u;

    while (pixels_remaining > 0u && source < end)
    {
        uint8_t control = *source++;
        uint32_t count = (control & 0x7Fu) + 1u;
        bool repeated = (control & 0x80u) != 0u;
        uint32_t packet_bytes = repeated ? 2u : count * 2u;

        if (count > pixels_remaining || (uint32_t)(end - source) < packet_bytes)
            return;

        const uint8_t *pixel = source;
        source += packet_bytes;
        pixels_remaining -= count;

        while (count-- > 0u)
        {
            draw_buffer[buffered++] = pixel[0];
            draw_buffer[buffered++] = pixel[1];
            if (!repeated)
                pixel += 2;

            if (buffered == sizeof(draw_buffer))
            {
                st7789_write_gram(draw_buffer, buffered, false);
                buffered = 0u;
            }
        }
    }

    if (pixels_remaining == 0u && buffered > 0u)
        st7789_write_gram(draw_buffer, buffered, false);
}

void st7789_draw_image(uint16_t x, uint16_t y, const image_t *image)
{
    if (!in_screen_range(x,
                         y,
                         x + image->width - 1u,
                         y + image->height - 1u))
        return;
    
    st7789_set_range_and_prepare_gram(x,
                                      y,
                                      x + image->width - 1u,
                                      y + image->height - 1u);
    if (image->encoding == IMAGE_ENCODING_RGB565_RLE)
        st7789_draw_rle_image(image);
    else
        st7789_write_gram(image->data, (uint32_t)image->width * image->height * 2u, false);
}

void DMA1_Stream4_IRQHandler(void)
{
    if (DMA_GetITStatus(DMA1_Stream4, DMA_IT_TCIF4) == SET)
    {
        BaseType_t higher_priority_task_woken = pdFALSE;

        DMA_ClearITPendingBit(DMA1_Stream4, DMA_IT_TCIF4);
        xSemaphoreGiveFromISR(write_gram_semaphore, &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}
