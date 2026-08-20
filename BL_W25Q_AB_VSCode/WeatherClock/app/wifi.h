#ifndef CLOCK_F407_WIFI_H
#define CLOCK_F407_WIFI_H

#define WIFI_SSID   "RAIN"
#define WIFI_PASSWD "11111111"

/**
 * @brief 启动 Wi-Fi 后台管理任务。
 *
 * 任务负责初始化 ESP、连接热点并在断线后持续重试。该函数立即返回，网络不可用
 * 不会阻止本地时钟和温湿度功能启动。
 */
void wifi_start(void);

#endif /* CLOCK_F407_WIFI_H */
