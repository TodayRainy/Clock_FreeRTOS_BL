/**
 * @file rtc.c
 * @brief STM32F407 片上 RTC 的二进制日期时间读写。
 *
 * 按 STM32 RTC 的要求先读时间寄存器、再读日期寄存器：读取时间会锁住
 * Calendar Shadow Registers，读取日期后解锁，因此一次调用即可得到一致快照。
 */

#include "rtc.h"

#include "stm32f4xx.h"

void rtc_init(void)
{
    RTC_InitTypeDef rtc;

    RTC_StructInit(&rtc);
    RTC_Init(&rtc);

    RCC_RTCCLKCmd(ENABLE);
    RTC_WaitForSynchro();
}

static void rtc_set_once(const rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);

    date.RTC_Year = date_time->year - 2000;
    date.RTC_Month = date_time->month;
    date.RTC_Date = date_time->day;
    date.RTC_WeekDay = date_time->weekday;
    time.RTC_Hours = date_time->hour;
    time.RTC_Minutes = date_time->minute;
    time.RTC_Seconds = date_time->second;
    
    RTC_SetDate(RTC_Format_BIN, &date);
    RTC_SetTime(RTC_Format_BIN, &time);
}

static void rtc_get_once(rtc_date_time_t *date_time)
{
    RTC_DateTypeDef date;
    RTC_TimeTypeDef time;

    RTC_DateStructInit(&date);
    RTC_TimeStructInit(&time);

    /* TR 读取后 Shadow Registers 保持不变，直到随后读取 DR。 */
    RTC_GetTime(RTC_Format_BIN, &time);
    RTC_GetDate(RTC_Format_BIN, &date);

    date_time->year = 2000 + date.RTC_Year;
    date_time->month = date.RTC_Month;
    date_time->day = date.RTC_Date;
    date_time->weekday = date.RTC_WeekDay;
    date_time->hour = time.RTC_Hours;
    date_time->minute = time.RTC_Minutes;
    date_time->second = time.RTC_Seconds;
}

void rtc_set_time(const rtc_date_time_t *date_time)
{
    rtc_date_time_t rtime;

    do
    {
        rtc_set_once(date_time);
        rtc_get_once(&rtime);
    } while (date_time->second != rtime.second);
}

void rtc_get_time(rtc_date_time_t *date_time)
{
    rtc_get_once(date_time);
}
