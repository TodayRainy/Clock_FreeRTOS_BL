/**
 * @file firmware_aes_gcm_tool.c
 * @brief 使用与 Bootloader 相同的 Mbed TLS 源码完成主机端 AES-256-GCM。
 *
 * 该小工具只处理二进制文件，不理解 XBIN。Python 打包器负责组织 Header、AAD
 * 和 Nonce，工具负责加密或认证解密，避免主机端再引入一套密码算法实现。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbedtls/gcm.h"

#define AES_KEY_SIZE 32u
#define GCM_NONCE_SIZE 12u
#define GCM_TAG_SIZE 16u

static int read_file(const char *path, unsigned char **data, size_t *size)
{
    FILE *file = fopen(path, "rb");
    long length;

    if (file == NULL || fseek(file, 0, SEEK_END) != 0)
        return 0;
    length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
    {
        fclose(file);
        return 0;
    }

    *data = malloc((size_t)length == 0u ? 1u : (size_t)length);
    if (*data == NULL ||
        ((size_t)length != 0u &&
         fread(*data, 1, (size_t)length, file) != (size_t)length))
    {
        free(*data);
        fclose(file);
        return 0;
    }
    fclose(file);
    *size = (size_t)length;
    return 1;
}

static int write_file(const char *path, const unsigned char *data, size_t size)
{
    FILE *file = fopen(path, "wb");
    int ok = file != NULL && fwrite(data, 1, size, file) == size;

    if (file != NULL)
        fclose(file);
    return ok;
}

int main(int argc, char **argv)
{
    unsigned char *key = NULL;
    unsigned char *nonce = NULL;
    unsigned char *aad = NULL;
    unsigned char *input = NULL;
    unsigned char *output = NULL;
    unsigned char *tag = NULL;
    size_t key_size, nonce_size, aad_size, input_size, tag_size = 0;
    mbedtls_gcm_context context;
    int encrypt;
    int result = 1;

    if (argc != 8 ||
        (strcmp(argv[1], "encrypt") != 0 && strcmp(argv[1], "decrypt") != 0))
    {
        fprintf(stderr,
                "usage: firmware_aes_gcm_tool <encrypt|decrypt> <key> <nonce> "
                "<aad> <input> <output> <tag>\n");
        return 2;
    }
    encrypt = strcmp(argv[1], "encrypt") == 0;

    if (!read_file(argv[2], &key, &key_size) || key_size != AES_KEY_SIZE ||
        !read_file(argv[3], &nonce, &nonce_size) || nonce_size != GCM_NONCE_SIZE ||
        !read_file(argv[4], &aad, &aad_size) ||
        !read_file(argv[5], &input, &input_size) ||
        (!encrypt && (!read_file(argv[7], &tag, &tag_size) ||
                      tag_size != GCM_TAG_SIZE)))
    {
        fprintf(stderr, "cannot read AES-GCM input files or sizes are invalid\n");
        goto out;
    }

    output = malloc(input_size == 0u ? 1u : input_size);
    if (output == NULL)
        goto out;
    if (encrypt)
    {
        tag = malloc(GCM_TAG_SIZE);
        if (tag == NULL)
            goto out;
    }

    mbedtls_gcm_init(&context);
    if (mbedtls_gcm_setkey(&context, MBEDTLS_CIPHER_ID_AES,
                           key, AES_KEY_SIZE * 8u) != 0)
        goto free_context;

    if (encrypt)
    {
        if (mbedtls_gcm_crypt_and_tag(&context, MBEDTLS_GCM_ENCRYPT,
                                      input_size, nonce, nonce_size,
                                      aad, aad_size, input, output,
                                      GCM_TAG_SIZE, tag) != 0)
            goto free_context;
    }
    else if (mbedtls_gcm_auth_decrypt(&context, input_size, nonce, nonce_size,
                                      aad, aad_size, tag, tag_size,
                                      input, output) != 0)
    {
        fprintf(stderr, "AES-GCM authentication failed\n");
        goto free_context;
    }

    if (!write_file(argv[6], output, input_size) ||
        (encrypt && !write_file(argv[7], tag, GCM_TAG_SIZE)))
    {
        fprintf(stderr, "cannot write AES-GCM output files\n");
        goto free_context;
    }
    result = 0;

free_context:
    mbedtls_gcm_free(&context);
out:
    free(tag);
    free(output);
    free(input);
    free(aad);
    free(nonce);
    free(key);
    return result;
}
