/**
 * @file esp_at.c
 * @brief ESP32-C3 AT驱动及面向OTA的流式HTTPS接收。
 *
 * USART2 RX中断只把字节放入单生产者/单消费者环形缓冲区，AT响应解析全部
 * 在任务上下文完成。普通天气命令和OTA共用一个互斥锁；流式HTTP从open到
 * close独占ESP，防止SNTP或天气查询插入同一条AT连接。
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "stm32f4xx.h"
#include "esp_at.h"

#define ESP_AT_DEBUG             1
#define ESP_AT_DEFAULT_BAUD      115200u
#define ESP_AT_RECOVERY_BAUD     921600u
#define ESP_AT_COMMAND_ATTEMPTS  3u
#define ESP_AT_BUSY_DELAY_MS     50u
#define ESP_RX_RING_SIZE         2048u
#define ESP_RESPONSE_SIZE        1024u
#define ESP_COMMAND_SIZE         512u
#define ESP_HTTP_HEADER_SIZE     1024u
#define ESP_HTTP_HEADER_PULL     256u
#define ESP_HTTP_PULL_SIZE       1024u
#define ESP_HTTP_HOST_SIZE       96u
#define ESP_HTTP_PATH_SIZE       320u

typedef enum
{
    AT_ACK_NONE,
    AT_ACK_OK,
    AT_ACK_ERROR,
    AT_ACK_BUSY,
    AT_ACK_READY,
} at_ack_t;

/**
 * @brief 被动接收命令的非数据返回值。
 *
 * 正数仍直接表示收到的正文长度。NO_DATA 和 BUSY 都表示本次命令已经明确
 * 结束，可以在同一个总超时内再次发送；PROTOCOL_ERROR 表示事务边界不再
 * 明确，调用者必须终止当前 HTTP 会话，不能盲目重发命令。
 */
typedef enum
{
    ESP_PULL_PROTOCOL_ERROR = -1,
    ESP_PULL_NO_DATA = -2,
    ESP_PULL_BUSY = -3,
} esp_pull_result_t;

typedef struct
{
    bool opened;
    uint32_t remaining;
    uint32_t restore_baud;
    uint16_t buffered_offset;
    uint16_t buffered_length;
    uint8_t buffered[ESP_HTTP_PULL_SIZE];
} http_session_t;

static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;
static volatile bool rx_overflow;
static uint8_t rx_ring[ESP_RX_RING_SIZE];

static char response_buffer[ESP_RESPONSE_SIZE];
static char command_buffer[ESP_COMMAND_SIZE];
static char http_header[ESP_HTTP_HEADER_SIZE];
static http_session_t http_session;
static SemaphoreHandle_t at_mutex;
static bool socket_cleanup_needed;
static uint32_t current_baud = ESP_AT_DEFAULT_BAUD;

static void esp_at_io_init(void)
{
    GPIO_InitTypeDef gpio;

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_USART2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_USART2);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_High_Speed;
    gpio.GPIO_Pin = GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_Init(GPIOA, &gpio);
}

static void esp_at_usart_init(uint32_t baud)
{
    USART_InitTypeDef usart;

    USART_Cmd(USART2, DISABLE);
    USART_StructInit(&usart);
    usart.USART_BaudRate = baud;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_WordLength = USART_WordLength_8b;
    USART_Init(USART2, &usart);
    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
    USART_ITConfig(USART2, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART2, ENABLE);
}

static void esp_at_dma_init(void)
{
    DMA_InitTypeDef dma;

    DMA_StructInit(&dma);
    dma.DMA_Channel = DMA_Channel_4;
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DR;
    dma.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_Medium;
    dma.DMA_FIFOMode = DMA_FIFOMode_Enable;
    dma.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    dma.DMA_MemoryBurst = DMA_MemoryBurst_INC8;
    dma.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(DMA1_Stream6, &dma);
}

static void esp_at_int_init(void)
{
    NVIC_InitTypeDef nvic;

    nvic.NVIC_IRQChannel = USART2_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 5;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
}

static void esp_at_lowlevel_init(void)
{
    esp_at_io_init();
    current_baud = ESP_AT_DEFAULT_BAUD;
    esp_at_usart_init(current_baud);
    esp_at_dma_init();
    esp_at_int_init();
}

/** @brief 丢弃UART环形缓冲区中的旧提示信息。 */
static void rx_flush(void)
{
    NVIC_DisableIRQ(USART2_IRQn);
    rx_tail = rx_head;
    rx_overflow = false;
    NVIC_EnableIRQ(USART2_IRQn);
}

static bool rx_get_byte(uint8_t *value, TickType_t deadline)
{
    while (rx_tail == rx_head)
    {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0)
            return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    *value = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1u) % ESP_RX_RING_SIZE);
    return true;
}

/** @brief 读取一行AT文本；超长行仍会被完整消费。 */
static bool rx_read_line(char *line, uint32_t capacity, TickType_t deadline)
{
    uint32_t length = 0;
    uint8_t value;

    if (capacity == 0)
        return false;
    while (rx_get_byte(&value, deadline))
    {
        if (length + 1u < capacity)
            line[length++] = (char)value;
        if (value == '\n')
        {
            line[length] = '\0';
            return true;
        }
    }
    line[length] = '\0';
    return false;
}

static at_ack_t line_ack(const char *line)
{
    if (strcmp(line, "OK\r\n") == 0 || strcmp(line, "SEND OK\r\n") == 0)
        return AT_ACK_OK;
    if (strcmp(line, "ERROR\r\n") == 0 || strcmp(line, "FAIL\r\n") == 0)
        return AT_ACK_ERROR;
    if (strncmp(line, "busy p", 6) == 0)
        return AT_ACK_BUSY;
    if (strcmp(line, "ready\r\n") == 0)
        return AT_ACK_READY;
    return AT_ACK_NONE;
}

static void append_response(const char *line)
{
    size_t used = strlen(response_buffer);
    size_t length = strlen(line);

    if (length > sizeof(response_buffer) - used - 1u)
        length = sizeof(response_buffer) - used - 1u;
    memcpy(response_buffer + used, line, length);
    response_buffer[used + length] = '\0';
}

/** @brief 在调用者给出的绝对截止时间前等待一条 AT 命令结束。 */
static at_ack_t wait_result_until(TickType_t deadline)
{
    char line[256];

    response_buffer[0] = '\0';
    while (rx_read_line(line, sizeof(line), deadline))
    {
        at_ack_t ack;

        append_response(line);
        ack = line_ack(line);
        if (ack != AT_ACK_NONE)
            return ack;
    }
    return AT_ACK_NONE;
}

static at_ack_t wait_result(uint32_t timeout_ms)
{
    return wait_result_until(xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms));
}

static void usart_write(const uint8_t *data, uint32_t length)
{
    DMA_Cmd(DMA1_Stream6, DISABLE);
    while (DMA1_Stream6->CR & DMA_SxCR_EN);
    DMA_ClearFlag(DMA1_Stream6, DMA_FLAG_FEIF6 | DMA_FLAG_DMEIF6 |
                  DMA_FLAG_TEIF6 | DMA_FLAG_HTIF6 | DMA_FLAG_TCIF6);
    DMA1_Stream6->M0AR = (uint32_t)data;
    DMA1_Stream6->NDTR = length;
    DMA_Cmd(DMA1_Stream6, ENABLE);
    while (DMA_GetFlagStatus(DMA1_Stream6, DMA_FLAG_TCIF6) == RESET);
    DMA_Cmd(DMA1_Stream6, DISABLE);
    while (USART_GetFlagStatus(USART2, USART_FLAG_TC) == RESET);
}

/** @brief 发送命令并保留ESP返回的具体结束状态。调用者必须持有AT互斥锁。 */
static at_ack_t execute_ack_locked(const char *command, uint32_t timeout_ms)
{
    at_ack_t ack;

#if ESP_AT_DEBUG
    printf("[AT] >> %s", command);
#endif
    rx_flush();
    usart_write((const uint8_t *)command, strlen(command));
    ack = wait_result(timeout_ms);
#if ESP_AT_DEBUG
    printf("[AT] << %s", response_buffer);
#endif
    return ack;
}

static bool execute_locked(const char *command, uint32_t timeout_ms)
{
    return execute_ack_locked(command, timeout_ms) == AT_ACK_OK;
}

static bool wait_ready(uint32_t timeout_ms)
{
    char line[128];
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (rx_read_line(line, sizeof(line), deadline))
    {
        if (line_ack(line) == AT_ACK_READY)
            return true;
    }
    return false;
}

static bool wait_boot(uint32_t timeout_ms)
{
    uint32_t elapsed;

    for (elapsed = 0; elapsed < timeout_ms; elapsed += 200u)
    {
        if (execute_locked("AT\r\n", 200u))
            return true;
    }
    return false;
}

/**
 * @brief 在已经独占ESP时切换双方波特率。
 *
 * ESP偶尔会在SSL收尾阶段返回busy p。这里只针对已经实测到的忙状态进行
 * 三次短重试；其它错误立即返回，避免把真实协议错误隐藏成长时间等待。
 */
static bool set_baud_locked(uint32_t baud)
{
    uint32_t attempt;
    int length;

    if (baud == 0u)
        return false;
    if (baud == current_baud)
        return true;

    length = snprintf(command_buffer, sizeof(command_buffer),
                      "AT+UART_CUR=%lu,8,1,0,0\r\n",
                      (unsigned long)baud);
    if (length <= 0 || (uint32_t)length >= sizeof(command_buffer))
        return false;

    for (attempt = 0; attempt < ESP_AT_COMMAND_ATTEMPTS; ++attempt)
    {
        at_ack_t ack = execute_ack_locked(command_buffer, 1000u);

        if (ack == AT_ACK_OK)
        {
            /* ESP在旧速率返回OK后切换，STM32随即更新BRR与接收状态。 */
            rx_flush();
            esp_at_usart_init(baud);
            current_baud = baud;
            return true;
        }
        if (ack != AT_ACK_BUSY)
            return false;
#if ESP_AT_DEBUG
        printf("[AT] UART busy, retry %lu/%u\n",
               (unsigned long)(attempt + 1u), ESP_AT_COMMAND_ATTEMPTS);
#endif
        vTaskDelay(pdMS_TO_TICKS(ESP_AT_BUSY_DELAY_MS));
    }
    return false;
}

/** @brief 关闭socket；CLOSED后附带ERROR表示连接已经由远端关闭，也算成功。 */
static bool close_socket_locked(void)
{
    uint32_t attempt;

    for (attempt = 0; attempt < ESP_AT_COMMAND_ATTEMPTS; ++attempt)
    {
        at_ack_t ack = execute_ack_locked("AT+CIPCLOSE\r\n", 1000u);

        if (ack == AT_ACK_OK || strstr(response_buffer, "CLOSED") != NULL)
            return true;
        if (ack != AT_ACK_BUSY)
            return false;
#if ESP_AT_DEBUG
        printf("[AT] close busy, retry %lu/%u\n",
               (unsigned long)(attempt + 1u), ESP_AT_COMMAND_ATTEMPTS);
#endif
        vTaskDelay(pdMS_TO_TICKS(ESP_AT_BUSY_DELAY_MS));
    }
    return false;
}

bool esp_at_init(void)
{
    if (at_mutex == NULL)
    {
        at_mutex = xSemaphoreCreateMutex();
        configASSERT(at_mutex);
    }

    esp_at_lowlevel_init();
    if (!wait_boot(3000u))
    {
        /* MCU单独复位时ESP可能仍停在OTA的临时高速率。只探测本项目实际
         * 使用的921600，并立即恢复115200，不扫描无关波特率。 */
        rx_flush();
        esp_at_usart_init(ESP_AT_RECOVERY_BAUD);
        current_baud = ESP_AT_RECOVERY_BAUD;
#if ESP_AT_DEBUG
        printf("[AT] probing recovery baud %u\n", ESP_AT_RECOVERY_BAUD);
#endif
        if (!wait_boot(1000u) || !set_baud_locked(ESP_AT_DEFAULT_BAUD))
        {
            esp_at_usart_init(ESP_AT_DEFAULT_BAUD);
            current_baud = ESP_AT_DEFAULT_BAUD;
            return false;
        }
#if ESP_AT_DEBUG
        printf("[AT] USART2 recovered to %u\n", ESP_AT_DEFAULT_BAUD);
#endif
    }
    if (!execute_locked("AT+RESTORE\r\n", 2000u) ||
        !wait_ready(5000u))
        return false;

    /* 关闭命令回显，流式二进制响应中只保留ESP返回内容。 */
    return execute_locked("ATE0\r\n", 1000u);
}

static bool lock_at(void)
{
    return at_mutex != NULL && xSemaphoreTake(at_mutex, portMAX_DELAY) == pdTRUE;
}

bool esp_at_set_baud(uint32_t baud)
{
    bool ok;

    if (baud == 0u || !lock_at())
        return false;
    ok = set_baud_locked(baud);
    xSemaphoreGive(at_mutex);
    return ok;
}

bool esp_at_wifi_init(void)
{
    bool ok;

    if (!lock_at())
        return false;
    ok = execute_locked("AT+CWMODE=1\r\n", 2000u);
    xSemaphoreGive(at_mutex);
    return ok;
}

bool esp_at_connect_wifi(const char *ssid, const char *pwd, const char *mac)
{
    bool ok;
    int length;

    if (ssid == NULL || pwd == NULL || !lock_at())
        return false;
    if (mac != NULL)
        length = snprintf(command_buffer, sizeof(command_buffer),
                          "AT+CWJAP=\"%s\",\"%s\",\"%s\"\r\n",
                          ssid, pwd, mac);
    else
        length = snprintf(command_buffer, sizeof(command_buffer),
                          "AT+CWJAP=\"%s\",\"%s\"\r\n", ssid, pwd);
    ok = length > 0 && (uint32_t)length < sizeof(command_buffer) &&
         execute_locked(command_buffer, 15000u);
    xSemaphoreGive(at_mutex);
    return ok;
}

static bool parse_cwstate_response(const char *response, esp_wifi_info_t *info)
{
    int wifi_state;

    response = strstr(response, "+CWSTATE:");
    if (response == NULL ||
        sscanf(response, "+CWSTATE:%d,\"%63[^\"]", &wifi_state, info->ssid) != 2)
        return false;
    info->connected = wifi_state == 2;
    return true;
}

static bool parse_cwjap_response(const char *response, esp_wifi_info_t *info)
{
    response = strstr(response, "+CWJAP:");
    return response != NULL &&
           sscanf(response, "+CWJAP:\"%63[^\"]\",\"%17[^\"]\",%d,%d",
                  info->ssid, info->bssid, &info->channel, &info->rssi) == 4;
}

bool esp_at_get_wifi_info(esp_wifi_info_t *info)
{
    bool ok = false;

    if (info == NULL || !lock_at())
        return false;
    memset(info, 0, sizeof(*info));
    if (!execute_locked("AT+CWSTATE?\r\n", 2000u) ||
        !parse_cwstate_response(response_buffer, info))
        goto out;
    if (info->connected &&
        (!execute_locked("AT+CWJAP?\r\n", 2000u) ||
         !parse_cwjap_response(response_buffer, info)))
        goto out;
    ok = true;
out:
    xSemaphoreGive(at_mutex);
    return ok;
}

bool wifi_is_connected(void)
{
    esp_wifi_info_t info;

    return esp_at_get_wifi_info(&info) && info.connected;
}

bool esp_at_sntp_init(void)
{
    bool ok;

    if (!lock_at())
        return false;
    ok = execute_locked("AT+CIPSNTPCFG=1,8\r\n", 2000u);
    xSemaphoreGive(at_mutex);
    return ok;
}

static uint8_t month_str_to_num(const char *month_str)
{
    static const char *months[] =
    {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    uint8_t i;

    for (i = 0; i < 12; ++i)
    {
        if (strcmp(month_str, months[i]) == 0)
            return i + 1u;
    }
    return 0;
}

static uint8_t weekday_str_to_num(const char *weekday_str)
{
    static const char *weekdays[] =
        {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    uint8_t i;

    for (i = 0; i < 7; ++i)
    {
        if (strcmp(weekday_str, weekdays[i]) == 0)
            return i + 1u;
    }
    return 0;
}

static bool parse_cipsntptime_response(const char *response,
                                       esp_date_time_t *date)
{
    char weekday_str[8];
    char month_str[4];
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int year;

    response = strstr(response, "+CIPSNTPTIME:");
    if (response == NULL ||
        sscanf(response, "+CIPSNTPTIME:%3s %3s %u %u:%u:%u %u",
               weekday_str, month_str, &day, &hour, &minute,
               &second, &year) != 7 || day > 31u || hour > 23u ||
        minute > 59u || second > 59u || year > UINT16_MAX)
        return false;
    date->weekday = weekday_str_to_num(weekday_str);
    date->month = month_str_to_num(month_str);
    date->day = (uint8_t)day;
    date->hour = (uint8_t)hour;
    date->minute = (uint8_t)minute;
    date->second = (uint8_t)second;
    date->year = (uint16_t)year;
    return date->weekday != 0 && date->month != 0;
}

bool esp_at_sntp_get_time(esp_date_time_t *date)
{
    bool ok;

    if (date == NULL || !lock_at())
        return false;
    ok = execute_locked("AT+CIPSNTPTIME?\r\n", 2000u) &&
         parse_cipsntptime_response(response_buffer, date);
    xSemaphoreGive(at_mutex);
    return ok;
}

const char *esp_at_http_get(const char *url)
{
    bool ok;
    int length;

    if (url == NULL || !lock_at())
        return NULL;
    length = snprintf(command_buffer, sizeof(command_buffer),
                      "AT+HTTPCLIENT=2,1,\"%s\",,,2\r\n", url);
    ok = length > 0 && (uint32_t)length < sizeof(command_buffer) &&
         execute_locked(command_buffer, 5000u);
    xSemaphoreGive(at_mutex);
    return ok ? response_buffer : NULL;
}

static bool parse_https_url(const char *url, char *host, uint32_t host_size,
                            char *path, uint32_t path_size)
{
    const char *host_start;
    const char *path_start;
    size_t host_length;

    if (strncmp(url, "https://", 8) != 0)
        return false;
    host_start = url + 8;
    path_start = strchr(host_start, '/');
    host_length = path_start != NULL ? (size_t)(path_start - host_start) :
                                      strlen(host_start);
    if (host_length == 0 || host_length >= host_size)
        return false;
    memcpy(host, host_start, host_length);
    host[host_length] = '\0';
    if (path_start == NULL)
        path_start = "/";
    if (strlen(path_start) >= path_size)
        return false;
    strcpy(path, path_start);
    return true;
}

/** @brief 等待AT+CIPSEND的输入提示符。 */
static at_ack_t wait_send_prompt(uint32_t timeout_ms)
{
    static const char busy_text[] = "busy p";
    static const char error_text[] = "ERROR";
    uint32_t busy_match = 0;
    uint32_t error_match = 0;
    uint8_t value;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (rx_get_byte(&value, deadline))
    {
        if (value == '>')
            return AT_ACK_OK;
        busy_match = value == (uint8_t)busy_text[busy_match] ?
                     busy_match + 1u : (value == 'b' ? 1u : 0u);
        error_match = value == (uint8_t)error_text[error_match] ?
                      error_match + 1u : (value == 'E' ? 1u : 0u);
        if (busy_match == sizeof(busy_text) - 1u)
            return AT_ACK_BUSY;
        if (error_match == sizeof(error_text) - 1u)
            return AT_ACK_ERROR;
    }
    return AT_ACK_NONE;
}

static bool send_http_request(const char *request, uint32_t length)
{
    int command_length;
    uint32_t attempt;

    command_length = snprintf(command_buffer, sizeof(command_buffer),
                              "AT+CIPSEND=%lu\r\n", (unsigned long)length);
    if (command_length <= 0 || (uint32_t)command_length >= sizeof(command_buffer))
        return false;

    for (attempt = 0; attempt < ESP_AT_COMMAND_ATTEMPTS; ++attempt)
    {
        at_ack_t prompt;
        at_ack_t ack;

        rx_flush();
        usart_write((const uint8_t *)command_buffer, (uint32_t)command_length);
        prompt = wait_send_prompt(2000u);
        if (prompt == AT_ACK_BUSY)
        {
#if ESP_AT_DEBUG
            printf("[HTTP] CIPSEND busy, retry %lu/%u\n",
                   (unsigned long)(attempt + 1u), ESP_AT_COMMAND_ATTEMPTS);
#endif
            vTaskDelay(pdMS_TO_TICKS(ESP_AT_BUSY_DELAY_MS));
            continue;
        }
        if (prompt != AT_ACK_OK)
        {
#if ESP_AT_DEBUG
            printf("[HTTP] CIPSEND prompt timeout/error\n");
#endif
            return false;
        }

        usart_write((const uint8_t *)request, length);
        ack = wait_result(5000u);
        if (ack == AT_ACK_OK || strstr(response_buffer, "SEND OK") != NULL)
            return true;
        if (ack != AT_ACK_BUSY)
        {
#if ESP_AT_DEBUG
            printf("[HTTP] request send failed: %s\n", response_buffer);
#endif
            return false;
        }
#if ESP_AT_DEBUG
        printf("[HTTP] request busy, retry %lu/%u\n",
               (unsigned long)(attempt + 1u), ESP_AT_COMMAND_ATTEMPTS);
#endif
        vTaskDelay(pdMS_TO_TICKS(ESP_AT_BUSY_DELAY_MS));
    }
    return false;
}

/**
 * @brief 从AT+CIPRECVDATA响应中提取精确长度的二进制数据。
 * @param[in] deadline 本次拉取事务与外层重试共用的绝对截止时间。
 * @return 正数为数据长度；0表示ESP返回了空数据；负数见
 *         @ref esp_pull_result_t。
 */
static int32_t receive_passive_data(uint8_t *data, uint32_t capacity,
                                    TickType_t deadline)
{
    static const char prefix[] = "+CIPRECVDATA:";
    static const char error_text[] = "ERROR\r\n";
    static const char busy_text[] = "busy p";
    uint32_t prefix_match = 0;
    uint32_t error_match = 0;
    uint32_t busy_match = 0;
    uint32_t actual = 0;
    uint32_t i;
    uint8_t value = 0;
    int length;

    length = snprintf(command_buffer, sizeof(command_buffer),
                      "AT+CIPRECVDATA=%lu\r\n", (unsigned long)capacity);
    if (length <= 0 || (uint32_t)length >= sizeof(command_buffer))
        return ESP_PULL_PROTOCOL_ERROR;

    rx_flush();
    usart_write((const uint8_t *)command_buffer, (uint32_t)length);

    while (rx_get_byte(&value, deadline))
    {
        prefix_match = value == (uint8_t)prefix[prefix_match] ?
                       prefix_match + 1u : (value == '+' ? 1u : 0u);
        error_match = value == (uint8_t)error_text[error_match] ?
                      error_match + 1u : (value == 'E' ? 1u : 0u);
        busy_match = value == (uint8_t)busy_text[busy_match] ?
                     busy_match + 1u : (value == 'b' ? 1u : 0u);
        if (prefix_match == sizeof(prefix) - 1u)
            break;
        if (error_match == sizeof(error_text) - 1u)
            return ESP_PULL_NO_DATA;
        if (busy_match == sizeof(busy_text) - 1u)
        {
            /* busy p... 表示命令没有开始执行。消费完整提示行后才允许重发，
             * 避免残留的省略号和换行污染下一条响应。 */
            while (value != '\n' && rx_get_byte(&value, deadline))
            {
                /* 只消费当前 BUSY 提示行，不解释其中的省略号。 */
            }
            return ESP_PULL_BUSY;
        }
    }
    if (prefix_match != sizeof(prefix) - 1u)
        return ESP_PULL_PROTOCOL_ERROR;

    while (rx_get_byte(&value, deadline) && value >= '0' && value <= '9')
        actual = actual * 10u + (uint32_t)(value - '0');
    if (value != ',' || actual > capacity)
        return ESP_PULL_PROTOCOL_ERROR;
    for (i = 0; i < actual; ++i)
    {
        if (!rx_get_byte(&data[i], deadline))
            return ESP_PULL_PROTOCOL_ERROR;
    }
    if (wait_result_until(deadline) != AT_ACK_OK)
        return ESP_PULL_PROTOCOL_ERROR;
    return (int32_t)actual;
}

static int32_t socket_pull(uint8_t *data, uint32_t capacity,
                           uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while ((int32_t)(xTaskGetTickCount() - deadline) < 0)
    {
        int32_t received = receive_passive_data(data, capacity, deadline);

        if (received > 0)
            return received;
        if ((received != 0 && received != ESP_PULL_NO_DATA &&
             received != ESP_PULL_BUSY) || rx_overflow)
            return -1;
#if ESP_AT_DEBUG
        if (received == ESP_PULL_BUSY)
            printf("[HTTP] CIPRECVDATA busy, retrying\n");
#endif
        vTaskDelay(pdMS_TO_TICKS(received == ESP_PULL_BUSY ?
                                 ESP_AT_BUSY_DELAY_MS : 20u));
    }
    return -1;
}

static bool parse_http_headers(esp_http_info_t *info, bool range_requested)
{
    const char *length_line;
    const char *range_line;
    unsigned int status;
    unsigned long content_length;
    unsigned long range_start;
    unsigned long range_end;
    unsigned long total_length;

    if (sscanf(http_header, "HTTP/%*u.%*u %u", &status) != 1)
        return false;
    if ((!range_requested && status != 200u) ||
        (range_requested && status != 206u))
        return false;

    length_line = strstr(http_header, "Content-Length:");
    if (length_line == NULL ||
        sscanf(length_line, "Content-Length: %lu", &content_length) != 1)
        return false;
    info->status_code = (uint16_t)status;
    info->content_length = (uint32_t)content_length;
    info->range_start = 0;
    info->total_length = info->content_length;
    if (!range_requested)
        return true;

    range_line = strstr(http_header, "Content-Range:");
    if (range_line == NULL ||
        sscanf(range_line, "Content-Range: bytes %lu-%lu/%lu",
               &range_start, &range_end, &total_length) != 3 ||
        range_end < range_start ||
        content_length != range_end - range_start + 1u)
        return false;
    info->range_start = (uint32_t)range_start;
    info->total_length = (uint32_t)total_length;
    return true;
}

static bool receive_http_headers(esp_http_info_t *info, bool range_requested)
{
    /* Header 始终按 ESP_HTTP_HEADER_PULL 分段读取，不需要在任务栈上放置
     * 与正文拉取上限同样大的缓冲区。正文仍由 http_session.buffered 以
     * 1024 字节接收，这里只保留一次 Header 分段所需的 256 字节。 */
    uint8_t pull_buffer[ESP_HTTP_HEADER_PULL];
    uint32_t used = 0;

    while (used + 1u < sizeof(http_header))
    {
        char *end;
        /* COS响应头可能超过512字节。小步拉取可避免第二次读取把1 KiB
         * Header缓冲区连同大段正文一次填满，同时不增加常驻RAM。 */
        int32_t received = socket_pull(pull_buffer, ESP_HTTP_HEADER_PULL, 5000u);

        if (received <= 0 || used + (uint32_t)received >= sizeof(http_header))
        {
#if ESP_AT_DEBUG
            printf("[HTTP] header pull failed, received=%ld used=%lu\n",
                   (long)received, (unsigned long)used);
#endif
            return false;
        }
        memcpy(http_header + used, pull_buffer, (uint32_t)received);
        used += (uint32_t)received;
        http_header[used] = '\0';
        end = strstr(http_header, "\r\n\r\n");
        if (end != NULL)
        {
            uint32_t header_length = (uint32_t)(end - http_header) + 4u;
            uint32_t body_length = used - header_length;

            if (body_length > sizeof(http_session.buffered) ||
                !parse_http_headers(info, range_requested) ||
                body_length > info->content_length)
            {
#if ESP_AT_DEBUG
                printf("[HTTP] invalid response header:\n%s\n", http_header);
#endif
                return false;
            }
            memcpy(http_session.buffered, http_header + header_length, body_length);
            http_session.buffered_offset = 0;
            http_session.buffered_length = (uint16_t)body_length;
            http_session.remaining = info->content_length;
            return true;
        }
    }
    return false;
}

static bool http_open_internal(const char *url, uint32_t range_start,
                               uint32_t range_end, esp_http_info_t *info,
                               uint32_t session_baud)
{
    char host[ESP_HTTP_HOST_SIZE];
    char path[ESP_HTTP_PATH_SIZE];
    char request[ESP_COMMAND_SIZE];
    bool range_requested = range_start != ESP_HTTP_RANGE_NONE;
    bool connection_opened = false;
    const char *failure_stage = "arguments";
    uint32_t restore_baud;
    int length;

    if (url == NULL || info == NULL ||
        !parse_https_url(url, host, sizeof(host), path, sizeof(path)) ||
        !lock_at())
        return false;
    memset(&http_session, 0, sizeof(http_session));
    restore_baud = current_baud;
    http_session.restore_baud = restore_baud;

    if (session_baud != 0u && !set_baud_locked(session_baud))
    {
        failure_stage = "UART baud";
        goto fail;
    }

    if (socket_cleanup_needed)
    {
        /* 上次连接若在ESP忙时关闭失败，先清一次遗留socket。远端已经主动
         * 关闭时本命令返回ERROR也无妨，后续CIPMUX成功即可确认链路空闲。 */
        close_socket_locked();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    failure_stage = "CIPMUX";
    if (!execute_locked("AT+CIPMUX=0\r\n", 1000u))
        goto fail;
    socket_cleanup_needed = false;
    /* 新版ESP-AT使用CIPRECVTYPE，旧版固件使用CIPRECVMODE。 */
    failure_stage = "CIPRECVTYPE";
    if (!execute_locked("AT+CIPRECVTYPE=1\r\n", 1000u) &&
        !execute_locked("AT+CIPRECVMODE=1\r\n", 1000u))
        goto fail;
    failure_stage = "CIPSSLCSNI";
    length = snprintf(command_buffer, sizeof(command_buffer),
                      "AT+CIPSSLCSNI=\"%s\"\r\n", host);
    if (length <= 0 || (uint32_t)length >= sizeof(command_buffer) ||
        !execute_locked(command_buffer, 1000u))
        goto fail;
    failure_stage = "CIPSTART";
    length = snprintf(command_buffer, sizeof(command_buffer),
                      "AT+CIPSTART=\"SSL\",\"%s\",443\r\n", host);
    if (length <= 0 || (uint32_t)length >= sizeof(command_buffer) ||
        !execute_locked(command_buffer, 10000u))
        goto fail;
    connection_opened = true;
    socket_cleanup_needed = true;

    if (!range_requested)
        length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.1\r\nHost: %s\r\n"
                          "User-Agent: Clock-F407\r\nConnection: close\r\n\r\n",
                          path, host);
    else if (range_end == ESP_HTTP_RANGE_NONE)
        length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.1\r\nHost: %s\r\n"
                          "Range: bytes=%lu-\r\nUser-Agent: Clock-F407\r\n"
                          "Connection: close\r\n\r\n", path, host,
                          (unsigned long)range_start);
    else
        length = snprintf(request, sizeof(request),
                          "GET %s HTTP/1.1\r\nHost: %s\r\n"
                          "Range: bytes=%lu-%lu\r\nUser-Agent: Clock-F407\r\n"
                          "Connection: close\r\n\r\n", path, host,
                          (unsigned long)range_start, (unsigned long)range_end);

    failure_stage = "HTTP request";
    if (length <= 0 || (uint32_t)length >= sizeof(request) ||
        !send_http_request(request, (uint32_t)length))
        goto fail;
    failure_stage = "HTTP response";
    if (!receive_http_headers(info, range_requested) ||
        (range_requested && info->range_start != range_start))
        goto fail;

    http_session.opened = true;
    return true;

fail:
#if ESP_AT_DEBUG
    printf("[HTTP] open failed at %s\n", failure_stage);
#endif
    if (connection_opened)
        socket_cleanup_needed = !close_socket_locked();
    if (current_baud != restore_baud && !set_baud_locked(restore_baud))
        printf("[AT] cannot restore USART2 to %lu after HTTP open failure\n",
               (unsigned long)restore_baud);
    memset(&http_session, 0, sizeof(http_session));
    xSemaphoreGive(at_mutex);
    return false;
}

bool esp_at_http_open(const char *url, uint32_t range_start,
                      uint32_t range_end, esp_http_info_t *info)
{
    return http_open_internal(url, range_start, range_end, info, 0u);
}

bool esp_at_http_open_at_baud(const char *url, uint32_t range_start,
                              uint32_t range_end, esp_http_info_t *info,
                              uint32_t baud)
{
    if (baud == 0u)
        return false;
    return http_open_internal(url, range_start, range_end, info, baud);
}

int32_t esp_at_http_read(uint8_t *data, uint32_t capacity,
                         uint32_t timeout_ms)
{
    uint32_t copied = 0;

    if (!http_session.opened || data == NULL || capacity == 0)
        return -1;
    if (http_session.remaining == 0)
        return 0;
    if (capacity > http_session.remaining)
        capacity = http_session.remaining;

    if (http_session.buffered_length != 0)
    {
        uint32_t available = http_session.buffered_length;

        if (available > capacity)
            available = capacity;
        memcpy(data, http_session.buffered + http_session.buffered_offset, available);
        http_session.buffered_offset += (uint16_t)available;
        http_session.buffered_length -= (uint16_t)available;
        copied = available;
    }

    while (copied < capacity)
    {
        uint32_t request = capacity - copied;
        int32_t received;

        if (request > ESP_HTTP_PULL_SIZE)
            request = ESP_HTTP_PULL_SIZE;
        received = socket_pull(data + copied, request, timeout_ms);
        if (received < 0)
            return copied != 0 ? (int32_t)copied : -1;
        if (received == 0)
            break;
        copied += (uint32_t)received;
    }
    http_session.remaining -= copied;
    return (int32_t)copied;
}

bool esp_at_http_close(void)
{
    uint32_t restore_baud;
    bool restore_ok;

    if (!http_session.opened)
        return true;
    restore_baud = http_session.restore_baud;
    socket_cleanup_needed = !close_socket_locked();
    memset(&http_session, 0, sizeof(http_session));
    restore_ok = current_baud == restore_baud || set_baud_locked(restore_baud);
    if (!restore_ok)
        printf("[AT] cannot restore USART2 to %lu after HTTP close\n",
               (unsigned long)restore_baud);
    xSemaphoreGive(at_mutex);
    return restore_ok;
}

void USART2_IRQHandler(void)
{
    if (USART_GetITStatus(USART2, USART_IT_RXNE) == SET)
    {
        uint8_t value = (uint8_t)USART_ReceiveData(USART2);
        uint16_t next = (uint16_t)((rx_head + 1u) % ESP_RX_RING_SIZE);

        if (next != rx_tail)
        {
            rx_ring[rx_head] = value;
            rx_head = next;
        }
        else
        {
            rx_overflow = true;
        }
        USART_ClearITPendingBit(USART2, USART_IT_RXNE);
    }
}
