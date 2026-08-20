/**
 * @file wifi.c
 * @brief ESP-AT 初始化、热点连接和断线恢复任务。
 *
 * Wi-Fi 只为校时、在线天气和 OTA 提供网络能力，不再决定天气时钟能否进入
 * 主页面。热点未开启时任务会定期重试，本地 RTC 与 AHT20 不受影响。
 */

#include "wifi.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "app.h"
#include "esp_at.h"
#include "page.h"

#define WIFI_RETRY_INTERVAL_MS  5000u
#define WIFI_POLL_INTERVAL_MS   5000u

/** @brief 完成一次 ESP、Station 模式和 SNTP 配置。 */
static bool wifi_stack_init(void)
{
    if (!esp_at_init())
    {
        printf("[AT] init failed\n");
        return false;
    }
    printf("[AT] inited\n");

    if (!esp_at_wifi_init())
    {
        printf("[WIFI] init failed\n");
        return false;
    }
    printf("[WIFI] inited\n");

    if (!esp_at_sntp_init())
    {
        printf("[SNTP] init failed\n");
        return false;
    }
    printf("[SNTP] inited\n");
    return true;
}

/** @brief 发布一次从离线到在线的状态变化。 */
static void wifi_publish_connected(const esp_wifi_info_t *info)
{
    printf("[WIFI] connected\n");
    printf("[WIFI] SSID: %s, BSSID: %s, Channel: %d, RSSI: %d\n",
           info->ssid, info->bssid, info->channel, info->rssi);
    main_page_redraw_wifi_ssid(info->ssid);
    app_network_connected();
}

/**
 * @brief Wi-Fi 后台任务。
 *
 * ESP 初始化失败时重新初始化；初始化成功但热点不可用时只重试连接，避免把
 * 普通断网升级成终止错误。在线状态下每 5 秒查询一次，检测到掉线后暂停应用
 * 的联网周期任务，重新连接成功再恢复。
 */
static void wifi_task(void *parameter)
{
    bool initialized = false;
    bool connected = false;
    bool offline_displayed = false;

    (void)parameter;
    while (1)
    {
        esp_wifi_info_t info = {0};

        if (!initialized)
        {
            if (!wifi_stack_init())
            {
                if (!offline_displayed)
                {
                    main_page_redraw_wifi_ssid("wifi init failed");
                    offline_displayed = true;
                }
                vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS));
                continue;
            }
            initialized = true;
        }

        if (esp_at_get_wifi_info(&info) && info.connected)
        {
            if (!connected)
            {
                connected = true;
                offline_displayed = false;
                wifi_publish_connected(&info);
            }
            vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_INTERVAL_MS));
            continue;
        }

        if (connected)
        {
            connected = false;
            app_network_disconnected();
            printf("[WIFI] disconnected\n");
        }
        if (!offline_displayed)
        {
            main_page_redraw_wifi_ssid("reconnecting");
            offline_displayed = true;
        }

        printf("[WIFI] connecting to %s\n", WIFI_SSID);
        esp_at_connect_wifi(WIFI_SSID, WIFI_PASSWD, NULL);

        info = (esp_wifi_info_t){0};
        if (esp_at_get_wifi_info(&info) && info.connected)
        {
            connected = true;
            offline_displayed = false;
            wifi_publish_connected(&info);
            vTaskDelay(pdMS_TO_TICKS(WIFI_POLL_INTERVAL_MS));
        }
        else
        {
            printf("[WIFI] connect failed; retry later\n");
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_INTERVAL_MS));
        }
    }
}

void wifi_start(void)
{
    configASSERT(xTaskCreate(wifi_task, "wifi", 512, NULL, 4, NULL) == pdPASS);
}
