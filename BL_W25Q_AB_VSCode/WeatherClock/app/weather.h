/**
 * @file weather.h
 * @brief WeatherClock在线天气字段及心知天气响应解析接口。
 */

#ifndef CLOCK_F407_WEATHER_H
#define CLOCK_F407_WEATHER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    char city[32];
    char location[128];
    char weather[16];
    int weather_code;
    int16_t temperature_x10; /**< 摄氏温度乘10，例如-1.5℃保存为-15。 */
} weather_info_t;

/**
 * @brief 从心知天气响应中提取当前城市、天气代码和温度。
 * @return 找到 results、location 和 now 主体时返回 true。
 */
bool parse_seniverse_response(const char *response, weather_info_t *info);

#endif /* CLOCK_F407_WEATHER_H */
