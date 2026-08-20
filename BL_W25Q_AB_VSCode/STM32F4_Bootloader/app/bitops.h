#ifndef CLOCK_F407_BITOPS_H
#define CLOCK_F407_BITOPS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/**
 * @file bitops.h
 * @brief Bootloader串口协议使用的小端字节流读写辅助函数。
 *
 * 协议缓冲区中的字段不保证自然对齐，因此这里逐字节组合整数，避免把字节指针
 * 强制转换成宽整数指针后产生未对齐访问或违反严格别名规则。
 */

static inline uint8_t get_u8(const uint8_t *data)
{
    return data[0];
}

static inline uint16_t get_u16(const uint8_t *data)
{
    return (uint16_t)data[0] |
           ((uint16_t)data[1] << 8);
}

static inline uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static inline void put_u8(uint8_t *data, uint8_t value)
{
    data[0] = value;
}

static inline void put_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static inline void put_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static inline void put_u8_inc(uint8_t **cursor, uint8_t value)
{
    put_u8(*cursor, value);
    *cursor += sizeof(value);
}

static inline void put_u16_inc(uint8_t **cursor, uint16_t value)
{
    put_u16(*cursor, value);
    *cursor += sizeof(value);
}

static inline void put_u32_inc(uint8_t **cursor, uint32_t value)
{
    put_u32(*cursor, value);
    *cursor += sizeof(value);
}

static inline void put_bytes_inc(uint8_t **cursor, const uint8_t *data,
                                 size_t length)
{
    if (length == 0u)
        return;

    memcpy(*cursor, data, length);
    *cursor += length;
}

#endif /* CLOCK_F407_BITOPS_H */
