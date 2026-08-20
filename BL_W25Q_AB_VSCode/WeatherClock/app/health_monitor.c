/**
 * @file health_monitor.c
 * @brief 应用侧IWDG持续喂养和Trial确认所需的核心健康信号。
 */

#include "health_monitor.h"

#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "watchdog.h"

#define WATCHDOG_REFRESH_INTERVAL_MS 1000u
#define STARTUP_GRACE_MS             20000u

static volatile bool core_ready;

/**
 * @brief 只要调度器正常并且核心初始化按时完成，就持续刷新IWDG。
 */
static void watchdog_task(void *parameter)
{
    uint32_t elapsed_ms = 0u;

    (void)parameter;
    while (core_ready || elapsed_ms < STARTUP_GRACE_MS)
    {
        watchdog_refresh();
        vTaskDelay(pdMS_TO_TICKS(WATCHDOG_REFRESH_INTERVAL_MS));
        elapsed_ms += WATCHDOG_REFRESH_INTERVAL_MS;
    }

    /* 不在失败路径主动复位；停止喂狗可以同时覆盖任务调度和中断失效场景。 */
    printf("[HEALTH] core startup timeout, watchdog reset pending\n");
    vTaskDelete(NULL);
}

void health_monitor_start(void)
{
    watchdog_init();
    configASSERT(xTaskCreate(watchdog_task, "watchdog", 192, NULL, 6,
                             NULL) == pdPASS);
}

void health_monitor_mark_ready(void)
{
    core_ready = true;
    printf("[HEALTH] core services ready\n");
}

bool health_monitor_is_ready(void)
{
    return core_ready;
}
