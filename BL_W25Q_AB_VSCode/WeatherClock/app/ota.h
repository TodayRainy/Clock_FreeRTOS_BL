/**
 * @file ota.h
 * @brief WeatherClock 后台 OTA 检查与用户触发入口。
 */

#ifndef __OTA_H__
#define __OTA_H__

/**
 * @brief 创建低优先级 OTA 任务并注册串口命令。
 *
 * 联网后调用一次即可。任务在 60 秒后自动检查，此后每 24 小时检查一次；
 * 自动检查不会下载或安装固件。USART1 输入 `c` 可立即检查，输入 `u` 则
 * 明确授权本次检查、下载和重启安装。
 */
void ota_start(void);

#endif /* __OTA_H__ */
