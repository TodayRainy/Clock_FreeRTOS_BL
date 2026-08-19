/**
 * @file sha256.c
 * @brief FIPS 180-4 SHA-256的直接实现。
 *
 * Bootloader只需要哈希固件和固定大小Header，因此这里保留最小的增量接口，
 * 不引入动态内存、文件抽象或与本项目无关的算法封装。
 */

#include <string.h>
#include "sha256.h"

static const uint32_t round_constants[64] =
{
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static uint32_t rotate_right(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32u - count));
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static void write_be32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value >> 24);
    data[1] = (uint8_t)(value >> 16);
    data[2] = (uint8_t)(value >> 8);
    data[3] = (uint8_t)value;
}

/**
 * @brief 压缩一个完整的64字节消息块。
 */
static void transform(sha256_context_t *context, const uint8_t block[64])
{
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t i;

    for (i = 0; i < 16; ++i)
        schedule[i] = read_be32(block + i * 4u);
    for (; i < 64; ++i)
    {
        uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                      rotate_right(schedule[i - 15], 18) ^
                      (schedule[i - 15] >> 3);
        uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                      rotate_right(schedule[i - 2], 19) ^
                      (schedule[i - 2] >> 10);
        schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];

    for (i = 0; i < 64; ++i)
    {
        uint32_t choice = (e & f) ^ (~e & g);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                        rotate_right(a, 22);
        uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                        rotate_right(e, 25);
        uint32_t temp1 = h + sum1 + choice + round_constants[i] + schedule[i];
        uint32_t temp2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

void sha256_init(sha256_context_t *context)
{
    static const uint32_t initial_state[8] =
    {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
    };

    memcpy(context->state, initial_state, sizeof(initial_state));
    context->total_length = 0;
    context->buffer_length = 0;
}

void sha256_update(sha256_context_t *context, const uint8_t *data,
                   uint32_t length)
{
    context->total_length += length;

    while (length)
    {
        uint32_t space = SHA256_BLOCK_SIZE - context->buffer_length;
        uint32_t chunk = length < space ? length : space;

        memcpy(context->buffer + context->buffer_length, data, chunk);
        context->buffer_length += chunk;
        data += chunk;
        length -= chunk;

        if (context->buffer_length == SHA256_BLOCK_SIZE)
        {
            transform(context, context->buffer);
            context->buffer_length = 0;
        }
    }
}

void sha256_final(sha256_context_t *context,
                  uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bit_length = context->total_length * 8u;
    uint32_t i;

    context->buffer[context->buffer_length++] = 0x80u;
    if (context->buffer_length > 56u)
    {
        memset(context->buffer + context->buffer_length, 0,
               SHA256_BLOCK_SIZE - context->buffer_length);
        transform(context, context->buffer);
        context->buffer_length = 0;
    }

    memset(context->buffer + context->buffer_length, 0,
           56u - context->buffer_length);
    for (i = 0; i < 8; ++i)
        context->buffer[56u + i] = (uint8_t)(bit_length >> (56u - i * 8u));
    transform(context, context->buffer);

    for (i = 0; i < 8; ++i)
        write_be32(digest + i * 4u, context->state[i]);

    /* 摘要计算后清除内部消息块，避免调用方误以为上下文还能继续追加。 */
    memset(context, 0, sizeof(*context));
}

void sha256(const uint8_t *data, uint32_t length,
            uint8_t digest[SHA256_DIGEST_SIZE])
{
    sha256_context_t context;

    sha256_init(&context);
    sha256_update(&context, data, length);
    sha256_final(&context, digest);
}
