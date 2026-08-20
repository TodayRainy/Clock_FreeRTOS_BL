/**
 * @file test_weather_parser.c
 * @brief 在宿主机上验证天气 JSON 的定点温度和最大长度字段解析。
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "weather.h"

static void check_temperature(const char *temperature, int16_t expected_x10)
{
    char response[384];
    weather_info_t info = {0};

    snprintf(response, sizeof(response),
             "{\"results\":[{\"location\":{\"name\": \"Hefei\","
             "\"path\": \"Hefei,Anhui,China\"},\"now\":{\"text\": \"Cloudy\","
             "\"code\": \"4\",\"temperature\": \"%s\"}}]}",
             temperature);
    assert(parse_seniverse_response(response, &info));
    assert(info.temperature_x10 == expected_x10);
    assert(info.weather_code == 4);
    assert(strcmp(info.city, "Hefei") == 0);
}

static void check_maximum_location_length(void)
{
    char response[512];
    char location[sizeof(((weather_info_t *)0)->location)];
    weather_info_t info = {0};

    memset(location, 'A', sizeof(location) - 1u);
    location[sizeof(location) - 1u] = '\0';
    snprintf(response, sizeof(response),
             "{\"results\":[{\"location\":{\"name\": \"Hefei\","
             "\"path\": \"%s\"},\"now\":{\"text\": \"Cloudy\","
             "\"code\": \"4\",\"temperature\": \"23\"}}]}",
             location);

    assert(parse_seniverse_response(response, &info));
    assert(strcmp(info.location, location) == 0);
}

int main(void)
{
    check_temperature("23", 230);
    check_temperature("-0.5", -5);
    check_temperature("12.9", 129);
    check_maximum_location_length();
    puts("Weather fixed-point parser tests passed.");
    return 0;
}
