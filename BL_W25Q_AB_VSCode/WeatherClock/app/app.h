#ifndef __APP_H__
#define __APP_H__

#ifndef APP_VERSION
#define APP_VERSION "v1.5.0-ota"
#endif

void app_init(void);

/** @brief Wi-Fi 建立连接后启动校时、在线天气及其周期任务。 */
void app_network_connected(void);

/** @brief Wi-Fi 断开后暂停尚未到期的网络周期任务。 */
void app_network_disconnected(void);

#endif /* __APP_H__ */
