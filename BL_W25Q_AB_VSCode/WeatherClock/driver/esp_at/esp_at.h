/**
 * @file esp_at.h
 * @brief ESP32-C3 AT 命令、Wi-Fi 及流式 HTTPS 访问接口。
 */

#ifndef WEATHER_CLOCK_ESP_AT_H
#define WEATHER_CLOCK_ESP_AT_H

#include <stdbool.h>
#include <stdint.h>

/** 不发送 Range Header，下载完整 HTTP 资源。 */
#define ESP_HTTP_RANGE_NONE UINT32_MAX

/** @brief 当前 Wi-Fi 连接信息。 */
typedef struct
{
    char ssid[64];       /**< 接入点名称。 */
    char bssid[18];      /**< 接入点 MAC 地址字符串。 */
    int channel;         /**< Wi-Fi 信道。 */
    int rssi;            /**< 接收信号强度，单位 dBm。 */
    bool connected;      /**< ESP 当前是否已关联接入点。 */
} esp_wifi_info_t;

/** @brief 从 ESP SNTP 响应解析出的本地日期时间。 */
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

/** @brief 流式 HTTP 响应的基本信息。 */
typedef struct
{
    uint16_t status_code;      /**< 完整下载为 200，Range 下载为 206。 */
    uint32_t content_length;   /**< 本次响应正文长度。 */
    uint32_t range_start;      /**< Content-Range起始位置；非Range响应为0。 */
    uint32_t total_length;     /**< 资源总长度；非Range响应等于content_length。 */
} esp_http_info_t;

/**
 * @brief 初始化 ESP AT 通信并恢复默认配置。
 *
 * 创建互斥锁、初始化 USART2/DMA，必要时从 921600 波特率恢复，再执行
 * AT+RESTORE 和关闭回显。该函数应在其他 ESP 任务启动前调用。
 *
 * @return ESP 响应正常并完成初始化时返回 true。
 */
bool esp_at_init(void);

/**
 * @brief 临时修改 ESP32-C3 与 STM32 之间的 USART2 波特率。
 *
 * 使用 AT+UART_CUR，不写入 ESP Flash。调用返回 true 时双方已经切换到新
 * 速率；普通天气通信保持 115200，OTA 大文件传输可临时使用更高速率。
 *
 * @param baud 新波特率。
 * @return 切换成功时返回 true。
 */
bool esp_at_set_baud(uint32_t baud);

/** @brief 将 ESP 设置为 Station 模式。 */
bool esp_at_wifi_init(void);

/**
 * @brief 连接指定 Wi-Fi 接入点。
 * @param ssid 接入点名称。
 * @param pwd 密码。
 * @param mac 可选 BSSID；传入 NULL 时由 ESP 自行选择。
 * @return AT 命令成功时返回 true。
 */
bool esp_at_connect_wifi(const char *ssid, const char *pwd, const char *mac);

/**
 * @brief 查询当前 Wi-Fi 状态和接入点信息。
 * @param info 接收查询结果。
 * @return 查询和解析均成功时返回 true。
 */
bool esp_at_get_wifi_info(esp_wifi_info_t *info);

/** @brief 查询 ESP 当前是否已连接 Wi-Fi。 */
bool esp_at_wifi_is_connected(void);

/** @brief 启用 ESP SNTP，并使用 UTC+8 时区。 */
bool esp_at_sntp_init(void);

/**
 * @brief 查询 ESP 的 SNTP 日期时间。
 * @param date 接收解析后的日期时间。
 * @return 查询和解析均成功时返回 true。
 */
bool esp_at_sntp_get_time(esp_date_time_t *date);

/**
 * @brief 使用 ESP HTTPCLIENT 命令获取一个小型 HTTPS 文本资源。
 *
 * 返回值指向模块内部响应缓冲区，只供当前调用任务立即读取；下一条 AT 命令会覆盖它。
 *
 * @param url HTTPS URL。
 * @return 成功时返回响应文本，失败时返回 NULL。
 */
const char *esp_at_http_get(const char *url);

/**
 * @brief 打开一个 HTTPS 流并独占 ESP AT 接口，直到调用 esp_at_http_close()。
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
bool esp_at_http_open(const char *url,
                      uint32_t range_start,
                      uint32_t range_end,
                      esp_http_info_t *info);

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
bool esp_at_http_open_at_baud(const char *url,
                              uint32_t range_start,
                              uint32_t range_end,
                              esp_http_info_t *info,
                              uint32_t baud);

/**
 * @brief 从已打开的HTTP正文读取最多capacity字节。
 * @return 正数表示实际读取长度，0表示正文结束，-1表示连接或协议错误。
 */
int32_t esp_at_http_read(uint8_t *data, uint32_t capacity, uint32_t timeout_ms);

/**
 * @brief 关闭当前HTTP连接，恢复打开前的波特率并释放ESP互斥锁。
 * @return 波特率已经恢复时返回true；没有活动连接也返回true。
 */
bool esp_at_http_close(void);

#endif /* WEATHER_CLOCK_ESP_AT_H */
