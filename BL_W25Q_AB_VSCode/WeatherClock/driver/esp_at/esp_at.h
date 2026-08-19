/**
 * @file esp_at.h
 * @brief ESP32-C3 AT命令、Wi-Fi及流式HTTPS访问接口。
 */

#ifndef __ESP_AT_H__
#define __ESP_AT_H__

#include <stdbool.h>
#include <stdint.h>

/** 不发送Range头，下载完整HTTP资源。 */
#define ESP_HTTP_RANGE_NONE UINT32_MAX

typedef struct
{
    char ssid[64];
    char bssid[18];
    int channel;
    int rssi;
    bool connected;
} esp_wifi_info_t;

typedef struct
{
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t weekday;
} esp_date_time_t;

/** @brief 流式HTTP响应的基本信息。 */
typedef struct
{
    uint16_t status_code;      /**< 完整下载为200，Range下载为206。 */
    uint32_t content_length;   /**< 本次响应正文长度。 */
    uint32_t range_start;      /**< Content-Range起始位置；非Range响应为0。 */
    uint32_t total_length;     /**< 资源总长度；非Range响应等于content_length。 */
} esp_http_info_t;

bool esp_at_init(void);

/**
 * @brief 临时修改 ESP32-C3 与 STM32 之间的 USART2 波特率。
 *
 * 使用 AT+UART_CUR，不写入 ESP Flash。调用返回 true 时双方已经切换到新
 * 速率；普通天气通信保持 115200，OTA 大文件传输可临时使用更高速率。
 */
bool esp_at_set_baud(uint32_t baud);

bool esp_at_wifi_init(void);
bool esp_at_connect_wifi(const char *ssid, const char *pwd, const char *mac);
bool esp_at_get_wifi_info(esp_wifi_info_t *info);
bool wifi_is_connected(void);
bool esp_at_sntp_init(void);
bool esp_at_sntp_get_time(esp_date_time_t *date);
const char *esp_at_http_get(const char *url);

/**
 * @brief 打开一个HTTPS流并独占ESP AT接口，直到调用esp_at_http_close()。
 *
 * ESP32-C3使用被动接收模式保存网络数据，STM32随后通过
 * @ref esp_at_http_read 按自身速度拉取。该接口只支持当前OTA所需的HTTPS
 * GET，不实现重定向、分块编码或通用HTTP客户端。
 *
 * @param[in] url 必须以"https://"开头。
 * @param[in] range_start 起始偏移；传ESP_HTTP_RANGE_NONE表示完整下载。
 * @param[in] range_end 结束偏移（包含）；ESP_HTTP_RANGE_NONE表示直到文件末尾。
 * @param[out] info 返回HTTP状态和长度。
 * @return 连接成功且响应头有效时返回true。失败时不会占用ESP互斥锁。
 */
bool esp_at_http_open(const char *url, uint32_t range_start,
                      uint32_t range_end, esp_http_info_t *info);

/**
 * @brief 以指定USART2速率打开HTTPS流，并在关闭流时恢复原速率。
 *
 * 波特率切换、HTTP会话和速率恢复共用同一次ESP互斥锁持有期，因此天气查询
 * 和Wi-Fi状态任务不能插入高速OTA会话。打开失败时也会尝试恢复调用前速率。
 *
 * @param[in] url 必须以"https://"开头。
 * @param[in] range_start 起始偏移。
 * @param[in] range_end 结束偏移（包含）。
 * @param[out] info 返回HTTP状态和长度。
 * @param[in] baud HTTP会话使用的USART2波特率。
 * @return 切换速率、建立连接并解析响应头成功时返回true。
 */
bool esp_at_http_open_at_baud(const char *url, uint32_t range_start,
                              uint32_t range_end, esp_http_info_t *info,
                              uint32_t baud);

/**
 * @brief 从已打开的HTTP正文读取最多capacity字节。
 * @return 正数表示实际读取长度，0表示正文结束，-1表示连接或协议错误。
 */
int32_t esp_at_http_read(uint8_t *data, uint32_t capacity,
                         uint32_t timeout_ms);

/**
 * @brief 关闭当前HTTP连接，恢复打开前的波特率并释放ESP互斥锁。
 * @return 波特率已经恢复时返回true；没有活动连接也返回true。
 */
bool esp_at_http_close(void);

#endif /* __ESP_AT_H__ */
