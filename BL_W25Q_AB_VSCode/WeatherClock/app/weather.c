/**
 * @file weather.c
 * @brief 心知天气当前天气响应的轻量字段提取。
 *
 * 接口响应结构固定且应用只需要少数字段，因此本模块不引入通用JSON库。解析器
 * 只在当前响应的 results/location/now 范围内查找并限制所有字符串写入长度。
 */

#include "weather.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/**
 * @brief 把天气接口的十进制温度文本转换为放大10倍的定点值。
 *
 * 心知天气通常返回整数，这里同时接受一位或多位小数并保留第一位，避免为了
 * 一个显示值引入atof及其浮点转换代码。
 */
static bool parse_temperature_x10(const char *text, int16_t *value)
{
    char *end;
    long whole;
    long result;
    bool negative;

    whole = strtol(text, &end, 10);
    negative = text[0] == '-';
    if (end == text || whole < -3276 || whole > 3276)
        return false;

    result = whole * 10;
    if (*end == '.')
    {
        ++end;
        if (*end < '0' || *end > '9')
            return false;

        result += negative ? -(*end - '0') : (*end - '0');
        while (*end >= '0' && *end <= '9')
            ++end;
    }

    if (*end != '\0' || result < INT16_MIN || result > INT16_MAX)
        return false;

    *value = (int16_t)result;
    return true;
}

bool parse_seniverse_response(const char *response, weather_info_t *info)
{
    const char *location_response;
    const char *location_name_response;
    const char *location_path_response;
    const char *now_response;
    const char *now_text_response;
    const char *now_code_response;
    const char *temperature_response;
    char temperature_text[16] = {0};

    response = strstr(response, "\"results\":");
    if (response == NULL)
        return false;

    location_response = strstr(response, "\"location\":");
    if (location_response == NULL)
        return false;

    location_name_response = strstr(location_response, "\"name\":");
    if (location_name_response != NULL)
    {
        sscanf(location_name_response, "\"name\": \"%31[^\"]\"",
               info->city);
    }

    location_path_response = strstr(location_response, "\"path\":");
    if (location_path_response != NULL)
    {
        sscanf(location_path_response, "\"path\": \"%127[^\"]\"",
               info->location);
    }

    now_response = strstr(response, "\"now\":");
    if (now_response == NULL)
        return false;

    now_text_response = strstr(now_response, "\"text\":");
    if (now_text_response != NULL)
    {
        sscanf(now_text_response, "\"text\": \"%15[^\"]\"",
               info->weather);
    }

    now_code_response = strstr(now_response, "\"code\":");
    if (now_code_response != NULL)
        sscanf(now_code_response, "\"code\": \"%d\"", &info->weather_code);

    temperature_response = strstr(now_response, "\"temperature\":");
    if (temperature_response != NULL &&
        sscanf(temperature_response, "\"temperature\": \"%15[^\"]\"",
               temperature_text) == 1 &&
        !parse_temperature_x10(temperature_text, &info->temperature_x10))
    {
        return false;
    }

    return true;
}
