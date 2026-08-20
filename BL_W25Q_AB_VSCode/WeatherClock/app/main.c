/**
 * @file main.c
 * @brief WeatherClock任务创建、核心初始化和健康就绪信号入口。
 */

#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "workqueue.h"
#include "app.h"
#include "ui.h"
#include "wifi.h"
#include "page.h"
#include "ab_confirm.h"
#include "board.h"
#include "health_monitor.h"
#include "ota.h"

static void main_init(void *param)
{
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
    
    xTaskCreate(main_init, "init", 1024, NULL, 9, NULL);
    
    vTaskStartScheduler();
    
    while (1)
    {
        ; // code should not run here
    }
}
