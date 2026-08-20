#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "workqueue.h"
#include "rtc.h"
#include "aht20.h"
#include "esp_at.h"
#include "weather.h"
#include "page.h"
#include "ota.h"
#include "app.h"

#define MILLISECONDS(x) (x)
#define SECONDS(x)      MILLISECONDS((x) * 1000)
#define MINUTES(x)      SECONDS((x) * 60)
#define HOURS(x)        MINUTES((x) * 60)
#define DAYS(x)          HOURS((x) * 24)

#define TIME_SYNC_INTERVAL          HOURS(1)
#define TIME_UPDATE_INTERVAL        SECONDS(1)
#define INNER_UPDATE_INTERVAL       SECONDS(3)
#define OUTDOOR_UPDATE_INTERVAL     MINUTES(1)

static TimerHandle_t time_sync_timer;
static TimerHandle_t time_update_timer;
static TimerHandle_t outdoor_update_timer;
static bool inner_ready;
static bool outdoor_ready;
static bool ota_ready_sent;

/** @brief 首次本地和在线数据都已显示后，开放 OTA 自动首检。 */
static void ota_ready_update(void)
{
    if (!ota_ready_sent && inner_ready && outdoor_ready)
    {
        ota_ready_sent = true;
        ota_app_ready();
    }
}

static void time_sync(void)
{
    uint32_t restart_sync_delay = TIME_SYNC_INTERVAL;
    rtc_date_time_t rtc_date = { 0 };

    esp_date_time_t esp_date = { 0 };
    if (!esp_at_sntp_get_time(&esp_date))
    {
        printf("[SNTP] get time failed\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }
    
    if (esp_date.year < 2000)
    {
        printf("[SNTP] invalid date formate\n");
        restart_sync_delay = SECONDS(1);
        goto err;
    }
    
    printf("[SNTP] sync time: %04u-%02u-%02u %02u:%02u:%02u (%d)\n",
        esp_date.year, esp_date.month, esp_date.day,
        esp_date.hour, esp_date.minute, esp_date.second, esp_date.weekday);
    
    rtc_date.year = esp_date.year;
    rtc_date.month = esp_date.month;
    rtc_date.day = esp_date.day;
    rtc_date.hour = esp_date.hour;
    rtc_date.minute = esp_date.minute;
    rtc_date.second = esp_date.second;
    rtc_date.weekday = esp_date.weekday;
    rtc_set_time(&rtc_date);
    
err:
    xTimerChangePeriod(time_sync_timer, pdMS_TO_TICKS(restart_sync_delay), 0);
}

static void time_update(void)
{
    static rtc_date_time_t last_date = { 0 };
    
    rtc_date_time_t date;
    rtc_get_time(&date);
    
    if (date.year < 2020)
    {
        return;
    }
    
    bool date_changed = date.year != last_date.year ||
                        date.month != last_date.month ||
                        date.day != last_date.day ||
                        date.weekday != last_date.weekday;

    /* 时间函数会自行只刷新变化的数字和冒号；日期只在跨天时重画。 */
    main_page_redraw_time(&date);
    if (date_changed)
        main_page_redraw_date(&date);

    last_date = date;
}

static void inner_update(void)
{
    static int16_t last_temperature_x10;
    static uint16_t last_humidity_x10;
    static bool has_last_measurement;
    
    if (!aht20_start_measurement())
    {
        printf("[AHT20] start measurement failed\n");
        return;
    }
    
    if (!aht20_wait_for_measurement())
    {
        printf("[AHT20] wait for measurement failed\n");
        return;
    }
    
    int16_t temperature_x10;
    uint16_t humidity_x10;
    
    if (!aht20_read_measurement(&temperature_x10, &humidity_x10))
    {
        printf("[AHT20] read measurement failed\n");
        return;
    }
    
    if (has_last_measurement && temperature_x10 == last_temperature_x10 &&
        humidity_x10 == last_humidity_x10)
    {
        return;
    }
    
    last_temperature_x10 = temperature_x10;
    last_humidity_x10 = humidity_x10;
    has_last_measurement = true;
    
    int32_t temperature_abs = temperature_x10 < 0 ?
                              -(int32_t)temperature_x10 : temperature_x10;
    printf("[AHT20] Temperature: %s%ld.%ld, Humidity: %u.%u\n",
           temperature_x10 < 0 ? "-" : "",
           (long)(temperature_abs / 10), (long)(temperature_abs % 10),
           (unsigned int)(humidity_x10 / 10u),
           (unsigned int)(humidity_x10 % 10u));
    main_page_redraw_inner_temperature(temperature_x10);
    main_page_redraw_inner_humidity(humidity_x10);

    if (!inner_ready)
    {
        inner_ready = true;
        printf("[APP] first indoor measurement ready\n");
        ota_ready_update();
    }
}

/**
 * @brief 独立的室内传感器任务。
 *
 * AHT20 不再经过可能被 HTTP/OTA 堵住的网络工作队列，因此固件下载期间仍按
 * 3 秒周期采集和刷新。
 */
static void inner_task(void *parameter)
{
    TickType_t last_wake = xTaskGetTickCount();

    (void)parameter;
    while (1)
    {
        inner_update();
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(INNER_UPDATE_INTERVAL));
    }
}

static void outdoor_update(void)
{
    static weather_info_t last_weather = { 0 };
    
    weather_info_t weather = { 0 };
    const char *weather_url = "https://api.seniverse.com/v3/weather/now.json?key=SfRic8Wmp-Qh3OeFk&location=WTEMH46Z5N09&language=en&unit=c";
    const char *weather_http_response = esp_at_http_get(weather_url);
    if (weather_http_response == NULL)
    {
        printf("[WEATHER] http error\n");
        return;
    }
    
    if (!parse_seniverse_response(weather_http_response, &weather))
    {
        printf("[WEATHER] parse failed\n");
        return;
    }
    
    if (memcmp(&last_weather, &weather, sizeof(weather_info_t)) != 0)
    {
        int32_t temperature_abs = weather.temperature_x10 < 0 ?
                                  -(int32_t)weather.temperature_x10 :
                                  weather.temperature_x10;

        memcpy(&last_weather, &weather, sizeof(weather_info_t));
        printf("[WEATHER] %s, %s, %s%ld.%ld\n",
               weather.city, weather.weather,
               weather.temperature_x10 < 0 ? "-" : "",
               (long)(temperature_abs / 10),
               (long)(temperature_abs % 10));
        main_page_redraw_outdoor_temperature(weather.temperature_x10);
        main_page_redraw_outdoor_weather_icon(weather.weather_code);
    }

    if (!outdoor_ready)
    {
        outdoor_ready = true;
        printf("[APP] first online weather ready\n");
        ota_ready_update();
    }
}

typedef void (*app_job_t)(void);

static void app_work(void *param)
{
    app_job_t job = (app_job_t)param;
    job();
}

static void work_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    workqueue_run(app_work, job);
}

static void app_timer_cb(TimerHandle_t timer)
{
    app_job_t job = (app_job_t)pvTimerGetTimerID(timer);
    job();
}

void app_init(void)
{
    time_update_timer = xTimerCreate("time update", pdMS_TO_TICKS(TIME_UPDATE_INTERVAL), pdTRUE, time_update, app_timer_cb);
    time_sync_timer = xTimerCreate("time sync", pdMS_TO_TICKS(200), pdFALSE, time_sync, work_timer_cb);
    outdoor_update_timer = xTimerCreate("outdoor update", pdMS_TO_TICKS(OUTDOOR_UPDATE_INTERVAL), pdTRUE, outdoor_update, work_timer_cb);

    xTimerStart(time_update_timer, 0);
    configASSERT(xTaskCreate(inner_task, "indoor", 512, NULL, 4, NULL) ==
                 pdPASS);
}

void app_network_connected(void)
{
    /* 首次联网和每次重连都立即校时、刷新一次天气，随后恢复原有周期。 */
    workqueue_run(app_work, time_sync);
    workqueue_run(app_work, outdoor_update);
    xTimerStart(outdoor_update_timer, 0);
}

void app_network_disconnected(void)
{
    /* 已经排队的网络操作允许自然失败；只阻止新的周期任务继续进入队列。 */
    xTimerStop(time_sync_timer, 0);
    xTimerStop(outdoor_update_timer, 0);
}
