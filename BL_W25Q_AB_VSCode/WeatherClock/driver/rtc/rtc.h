/**
 * @file rtc.h
 * @brief STM32 片上 RTC 日期时间接口。
 */

#ifndef CLOCK_F407_RTC_H
#define CLOCK_F407_RTC_H

#include <stdint.h>

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
} rtc_date_time_t;

/** @brief 初始化 STM32 RTC 并等待 Shadow Registers 同步。 */
void rtc_init(void);

/** @brief 设置 RTC 日期和时间，并读回确认秒字段已经生效。 */
void rtc_set_time(const rtc_date_time_t *date_time);

/** @brief 按 TR、DR 顺序读取一个一致的 RTC 日期时间快照。 */
void rtc_get_time(rtc_date_time_t *date_time);

#endif /* CLOCK_F407_RTC_H */
