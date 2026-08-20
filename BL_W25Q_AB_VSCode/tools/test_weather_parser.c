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

int main(void)
{
    check_temperature("23", 230);
    check_temperature("-0.5", -5);
    check_temperature("12.9", 129);
    puts("Weather fixed-point parser tests passed.");
    return 0;
}
