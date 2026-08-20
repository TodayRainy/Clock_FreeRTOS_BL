/**
 * @file health_monitor.h
 * @brief 应用启动健康状态和独立看门狗喂养策略。
 */

#ifndef CLOCK_F407_HEALTH_MONITOR_H
#define CLOCK_F407_HEALTH_MONITOR_H

#include <stdbool.h>

/**
 * @brief 启动IWDG和FreeRTOS监控任务。
 *
 * 应在调度器启动前调用。监控任务给予核心初始化20秒宽限；如果期间没有调用
 * health_monitor_mark_ready()，任务将停止喂狗，让IWDG复位系统。
 */
void health_monitor_start(void);

/**
 * @brief 标记调度器、板级设备和基本显示路径已经正常工作。
 *
 * Wi-Fi和互联网状态不属于固件可启动性的必要条件，调用点应位于联网之前。
 */
void health_monitor_mark_ready(void);

/** @brief 查询核心健康条件是否已经满足。 */
bool health_monitor_is_ready(void);

#endif /* CLOCK_F407_HEALTH_MONITOR_H */
