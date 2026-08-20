#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "weather.h"

/**
 * @brief 把天气接口的十进制温度文本转换为放大10倍的定点值。
 *
 * 心知天气通常返回整数，这里同时接受一位或多位小数并保留第一位，避免为了
 * 一个显示值引入atof及其浮点转换代码。
 */
static bool parse_temperature_x10(const char *text, int16_t *value)
{
	char *end;
	long whole = strtol(text, &end, 10);
	long result;
	bool negative = text[0] == '-';

	if (end == text)
		return false;
	if (whole < -3276 || whole > 3276)
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
	response = strstr(response, "\"results\":");
	if (response == NULL)
		return false;
	
	const char *location_response = strstr(response, "\"location\":");
	if (location_response == NULL)
		return false;
	
	const char *loaction_name_response = strstr(location_response, "\"name\":");
	if (loaction_name_response)
	{
		sscanf(loaction_name_response, "\"name\": \"%31[^\"]\"", info->city);
	}
	
	const char *loaction_path_response = strstr(location_response, "\"path\":");
	if (loaction_path_response)
	{
		sscanf(loaction_path_response, "\"path\": \"%128[^\"]\"", info->loaction);
	}
	
	const char *now_response = strstr(response, "\"now\":");
	if (now_response == NULL)
		return false;
	
	const char *now_text_response = strstr(now_response, "\"text\":");
	if (now_text_response)
	{
		sscanf(now_text_response, "\"text\": \"%15[^\"]\"", info->weather);
	}
	
	const char *now_code_response = strstr(now_response, "\"code\":");
	if (now_code_response)
	{
		sscanf(now_code_response, "\"code\": \"%d\"", &info->weather_code);
	}
	
	char temperature_str[16] = { 0 };
	const char *now_temperature_response = strstr(now_response, "\"temperature\":");
	if (now_temperature_response)
	{
		if (sscanf(now_temperature_response, "\"temperature\": \"%15[^\"]\"", temperature_str) == 1)
		{
			if (!parse_temperature_x10(temperature_str, &info->temperature_x10))
				return false;
		}
	}
	
	return true;
}
