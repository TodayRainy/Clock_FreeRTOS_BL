/**
 * @file ota.h
 * @brief WeatherClock 后台 OTA 检查与启动门槛接口。
 */

#ifndef CLOCK_F407_OTA_H
#define CLOCK_F407_OTA_H

/**
 * @brief 创建低优先级 OTA 任务并注册串口命令。
 *
 * 任务创建后先等待应用通知。首次室内温湿度和在线天气均刷新成功后，再等待
 * 60 秒执行首次自动检查；此后每 24 小时检查一次。USART1 输入 `u` 仍可用于
 * 联调时立即触发一次检查和下载。
 */
void ota_start(void);

/**
 * @brief 通知 OTA 任务：首次室内温湿度和在线天气均已刷新成功。
 *
 * 该通知只开放自动首检，不会立刻访问网络；OTA 任务仍保留 60 秒启动延迟。
 */
void ota_app_ready(void);

#endif /* CLOCK_F407_OTA_H */
