/**
 * @file main.c
 * @brief WeatherClock任务创建、核心初始化和健康就绪信号入口。
 */

#include "FreeRTOS.h"
#include "task.h"

#include <stdio.h>

#include "ab_confirm.h"
#include "app.h"
#include "board.h"
#include "health_monitor.h"
#include "ota.h"
#include "page.h"
#include "ui.h"
#include "wifi.h"
#include "workqueue.h"

/**
 * @brief 完成需要调度器支持的设备与应用服务初始化。
 *
 * 本地显示先于网络任务启动，因此热点暂时不可用不会阻塞RTC和室内传感器。
 */
static void main_init_task(void *parameter)
{
    (void)parameter;

    board_init();
    ui_init();

    welcome_page_display();
    health_monitor_mark_ready();

    printf("current app version: %s\r\n", APP_VERSION);

    /*
     * Wi-Fi 是可恢复的联网服务，不再作为本地时钟启动的前置条件。
     * 这样热点未开启时，RTC 和 AHT20 仍能正常显示；后台任务会继续重连。
     */
    main_page_display();
    app_init();
    ota_start();
    wifi_start();

    vTaskDelete(NULL);
}

int main(void)
{
    board_lowlevel_init();
    health_monitor_start();
    workqueue_init();
    ab_confirm_start();

    configASSERT(xTaskCreate(main_init_task, "init", 1024, NULL, 9,
                             NULL) == pdPASS);

    vTaskStartScheduler();

    /* 调度器只有在启动失败时才会返回，停在此处等待IWDG恢复系统。 */
    for (;;);
}
