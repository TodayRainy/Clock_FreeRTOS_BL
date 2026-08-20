#ifndef CLOCK_F407_APP_H
#define CLOCK_F407_APP_H

#ifndef APP_VERSION
#define APP_VERSION "v1.5.0-ota"
#endif

/** @brief 创建本地数据刷新任务和联网周期定时器。 */
void app_init(void);

/** @brief Wi-Fi 建立连接后启动校时、在线天气及其周期任务。 */
void app_network_connected(void);

/** @brief Wi-Fi 断开后暂停尚未到期的网络周期任务。 */
void app_network_disconnected(void);

#endif /* CLOCK_F407_APP_H */
