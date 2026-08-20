#ifndef CLOCK_F407_IMAGE_H
#define CLOCK_F407_IMAGE_H

#include <stdint.h>

/** @brief Image storage format understood by the ST7789 driver. */
typedef enum
{
    IMAGE_ENCODING_RGB565 = 0,
    IMAGE_ENCODING_RGB565_RLE,
} image_encoding_t;

/**
 * @brief Immutable image resource descriptor.
 *
 * Raw RGB565 images leave data_size at zero because their byte count follows
 * directly from width and height. Compressed images provide the encoded byte
 * count so the stream decoder never reads beyond the generated array.
 */
typedef struct
{
    uint16_t width;
    uint16_t height;
    uint32_t data_size;
    const uint8_t *data;
    image_encoding_t encoding;
} image_t;

extern const image_t img_meihua;
extern const image_t img_error;
extern const image_t img_wifi;
extern const image_t icon_wenduji;
extern const image_t icon_wifi;
extern const image_t icon_duoyun;
extern const image_t icon_leizhenyu;
extern const image_t icon_qing;
extern const image_t icon_yintian;
extern const image_t icon_yueliang;
extern const image_t icon_zhongxue;
extern const image_t icon_zhongyu;
extern const image_t icon_na;

#endif /* CLOCK_F407_IMAGE_H */
