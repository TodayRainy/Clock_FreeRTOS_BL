/**
 * @file native_encryption_selftest.c
 * @brief 在主机上运行 Bootloader 同一份流式 AES-GCM 解密代码。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware_decryption.h"

#define STREAM_CHUNK_SIZE 4096u

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

static int decrypt_image(const uint8_t *xbin, size_t xbin_size,
                         const magic_header_t *header,
                         uint8_t *plaintext, size_t *plaintext_size)
{
    uint8_t buffer[STREAM_CHUNK_SIZE + 16u];
    firmware_decryption_t context;
    size_t input_offset = header->data_offset;
    size_t output_offset = 0;

    if (input_offset + header->data_length != xbin_size ||
        !firmware_decryption_begin(&context, header))
        return 0;

    while (input_offset < xbin_size)
    {
        size_t chunk = xbin_size - input_offset;
        size_t output_length;

        if (chunk > STREAM_CHUNK_SIZE)
            chunk = STREAM_CHUNK_SIZE;
        memcpy(buffer, xbin + input_offset, chunk);
        if (!firmware_decryption_update(&context, buffer, chunk, sizeof(buffer),
                                        &output_length))
        {
            firmware_decryption_abort(&context);
            return 0;
        }
        memcpy(plaintext + output_offset, buffer, output_length);
        output_offset += output_length;
        input_offset += chunk;
    }

    {
        size_t output_length;
        if (!firmware_decryption_finish(&context, header, buffer,
                                        sizeof(buffer), &output_length))
            return 0;
        memcpy(plaintext + output_offset, buffer, output_length);
        output_offset += output_length;
    }
    *plaintext_size = output_offset;
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t *xbin = NULL;
    uint8_t *application = NULL;
    uint8_t *plaintext = NULL;
    size_t xbin_size;
    size_t application_size;
    size_t plaintext_size;
    magic_header_t header;
    int result = 1;

    if (argc != 3 || !load_file(argv[1], &xbin, &xbin_size) ||
        !load_file(argv[2], &application, &application_size) ||
        xbin_size < MAGIC_HEADER_AREA_SIZE + 1u)
    {
        fprintf(stderr,
                "usage: native_encryption_selftest <firmware.xbin> <application.bin>\n");
        return 2;
    }
    memcpy(&header, xbin, sizeof(header));
    plaintext = malloc(application_size + 16u);
    if (plaintext == NULL)
        goto out;

    if (!decrypt_image(xbin, xbin_size, &header,
                       plaintext, &plaintext_size) ||
        plaintext_size != application_size ||
        memcmp(plaintext, application, application_size) != 0)
    {
        fprintf(stderr, "Bootloader streaming AES-GCM rejected the valid XBIN\n");
        goto out;
    }

    header.encryption_tag[0] ^= 1u;
    if (decrypt_image(xbin, xbin_size, &header,
                      plaintext, &plaintext_size))
    {
        fprintf(stderr, "Bootloader streaming AES-GCM accepted a modified tag\n");
        goto out;
    }

    puts("Native Bootloader streaming AES-256-GCM self-test passed.");
    result = 0;

out:
    free(plaintext);
    free(application);
    free(xbin);
    return result;
}
