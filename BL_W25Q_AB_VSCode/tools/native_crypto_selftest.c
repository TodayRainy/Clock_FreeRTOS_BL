/**
 * @file native_crypto_selftest.c
 * @brief 在 PC 上直接运行 Bootloader 同一份 SHA-256 和 micro-ecc 验签代码。
 *
 * 该测试不模拟 STM32 外设，只验证最重要的密码学边界：C 实现必须接受构建工具
 * 生成的 XBIN Formal，并拒绝摘要、Header 或签名发生变化的镜像。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"
#include "firmware_auth.h"

static int load_file(const char *path, uint8_t **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return 0;
    length = ftell(file);
    if (length <= 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }

    *data = malloc((size_t)length);
    if (*data == NULL || fread(*data, 1, (size_t)length, file) != (size_t)length)
    {
        free(*data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

int main(int argc, char **argv)
{
    static const uint8_t abc_digest[SHA256_DIGEST_SIZE] =
    {
        0xBA, 0x78, 0x16, 0xBF, 0x8F, 0x01, 0xCF, 0xEA,
        0x41, 0x41, 0x40, 0xDE, 0x5D, 0xAE, 0x22, 0x23,
        0xB0, 0x03, 0x61, 0xA3, 0x96, 0x17, 0x7A, 0x9C,
        0xB4, 0x10, 0xFF, 0x61, 0xF2, 0x00, 0x15, 0xAD,
    };
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t *xbin;
    uint8_t *application;
    size_t xbin_size;
    size_t application_size;
    magic_header_t header;

    if (argc != 3 || !load_file(argv[1], &xbin, &xbin_size) ||
        !load_file(argv[2], &application, &application_size))
    {
        fprintf(stderr,
                "usage: native_crypto_selftest <firmware.xbin> <application.bin>\n");
        return 2;
    }
    if (xbin_size < MAGIC_HEADER_AREA_SIZE + 1u)
        return 3;

    sha256((const uint8_t *)"abc", 3, digest);
    if (memcmp(digest, abc_digest, sizeof(digest)) != 0)
    {
        fprintf(stderr, "SHA-256 known-answer test failed\n");
        return 4;
    }

    memcpy(&header, xbin, sizeof(header));
    if (!firmware_authenticate_header(&header))
    {
        fprintf(stderr, "C ECDSA verifier rejected the valid XBIN\n");
        return 5;
    }

    if (application_size != header.data_length)
        return 6;
    sha256(application, (uint32_t)application_size, digest);
    if (!firmware_hash_matches(&header, digest))
    {
        fprintf(stderr, "C SHA-256 rejected the valid XBIN payload\n");
        return 7;
    }

    header.signature[0] ^= 1u;
    if (firmware_authenticate_header(&header))
    {
        fprintf(stderr, "C ECDSA verifier accepted a modified signature\n");
        return 8;
    }

    digest[0] ^= 1u;
    if (firmware_hash_matches((const magic_header_t *)xbin, digest))
    {
        fprintf(stderr, "C SHA-256 comparison accepted a modified digest\n");
        return 9;
    }

    free(xbin);
    free(application);
    puts("Native C SHA-256 and ECDSA-P256 self-test passed.");
    return 0;
}
