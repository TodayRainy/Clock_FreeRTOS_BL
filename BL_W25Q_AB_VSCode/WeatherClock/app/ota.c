/**
 * @file ota.c
 * @brief 从腾讯云 COS 检查并下载签名 XBIN 到非活动 W25Q 槽。
 *
 * 本模块运行在普通 WeatherClock 应用中，负责联网和外部 Flash 写入；它不
 * 决定固件是否可信。下载完成后只向 EEPROM 写一条一次性请求，Bootloader
 * 会在复位后重新执行 ECDSA、SHA-256、CRC32 和防降级检查，再沿既有 A/B
 * 状态机安装。下载期间每 32 KiB 保存一次进度，掉电后会从最近检查点恢复。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f4xx.h"
#include "ab_confirm.h"
#include "bl24c512.h"
#include "board.h"
#include "console.h"
#include "crc32.h"
#include "esp_at.h"
#include "firmware_release_policy.h"
#include "firmware_security_policy.h"
#include "magic_header.h"
#include "ota.h"
#include "ota_contract.h"
#include "sha256.h"
#include "w25q.h"
#include "led.h"

#define OTA_MANIFEST_URL \
    "https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/" \
    "clock-f407/manifest.json"
#define OTA_PRODUCT                 "clock-f407"
#define OTA_FIRST_CHECK_TICKS       ((TickType_t)(60u * configTICK_RATE_HZ))
/* 避免把24小时毫秒数送入旧版pdMS_TO_TICKS时发生32位乘法溢出。 */
#define OTA_PERIODIC_CHECK_TICKS    \
    ((TickType_t)(24u * 60u * 60u * configTICK_RATE_HZ))
#define OTA_HTTP_TIMEOUT_MS         10000u
#define OTA_DOWNLOAD_BAUD           921600u
#define OTA_MANIFEST_BUFFER_SIZE    1024u
#define OTA_STREAM_BUFFER_SIZE      1024u
#define OTA_URL_SIZE                256u
#define OTA_EVENT_UPDATE            (1u << 0)
#define OTA_EVENT_APP_READY         (1u << 1)

typedef struct
{
    char download_url[OTA_URL_SIZE];
    uint32_t xbin_size;
    uint8_t xbin_sha256[SHA256_DIGEST_SIZE];
    magic_header_t header;
} ota_candidate_t;

static TaskHandle_t ota_task_handle;
static char manifest_buffer[OTA_MANIFEST_BUFFER_SIZE];
static uint8_t stream_buffer[OTA_STREAM_BUFFER_SIZE];

/** @brief 用三颗板载 LED 表示固件已经下载完成，等待用户按 RESET。 */
static void update_ready_set(bool ready)
{
    led_set(led1, ready);
    led_set(led2, ready);
    led_set(led3, ready);
}

/** @brief 校验 EEPROM 中安装请求的魔术字、目标槽和记录 CRC。 */
static bool install_request_valid(const ota_request_t *request)
{
    return request->magic == OTA_REQUEST_MAGIC &&
           request->target_slot < BOOT_SLOT_COUNT &&
           request->record_crc32 ==
               crc32((const uint8_t *)request,
                     offsetof(ota_request_t, record_crc32));
}

/** @brief 判断远端候选是否已经完整下载并提交为待安装请求。 */
static bool install_request_matches(const ota_candidate_t *candidate,
                                    uint8_t target_slot)
{
    ota_request_t request;

    if (!bl24c512_read(OTA_REQUEST_ADDR, (uint8_t *)&request,
                       sizeof(request)) ||
        !install_request_valid(&request))
        return false;

    return request.target_slot == target_slot &&
           request.xbin_size == candidate->xbin_size &&
           request.header_crc32 == candidate->header.header_crc32 &&
           request.payload_crc32 == candidate->header.data_crc32 &&
           request.security_version == candidate->header.security_version &&
           memcmp(request.image_sha256, candidate->header.image_sha256,
                  sizeof(request.image_sha256)) == 0;
}

/** @brief 返回 JSON 中指定键的值起点。清单由本项目生成，不实现通用 JSON。 */
static const char *json_value(const char *json, const char *key)
{
    char pattern[48];
    const char *value;

    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) <= 0)
        return NULL;
    value = strstr(json, pattern);
    if (value == NULL)
        return NULL;
    value = strchr(value + strlen(pattern), ':');
    if (value == NULL)
        return NULL;
    do { ++value; } while (*value == ' ' || *value == '\t');
    return value;
}

static bool json_string(const char *json, const char *key,
                        char *output, uint32_t capacity)
{
    const char *value = json_value(json, key);
    const char *end;
    size_t length;

    if (value == NULL || *value != '"')
        return false;
    end = strchr(value + 1, '"');
    if (end == NULL)
        return false;
    length = (size_t)(end - value - 1);
    if (length >= capacity)
        return false;
    memcpy(output, value + 1, length);
    output[length] = '\0';
    return true;
}

static bool json_u32(const char *json, const char *key, uint32_t *output)
{
    const char *value = json_value(json, key);
    uint32_t number = 0;
    bool has_digit = false;

    if (value == NULL)
        return false;
    while (*value >= '0' && *value <= '9')
    {
        uint32_t digit = (uint32_t)(*value++ - '0');

        if (number > (UINT32_MAX - digit) / 10u)
            return false;
        number = number * 10u + digit;
        has_digit = true;
    }
    if (!has_digit)
        return false;
    *output = number;
    return true;
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool decode_hex(const char *text, uint8_t *output, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size; ++i)
    {
        int high = hex_nibble(text[i * 2u]);
        int low = hex_nibble(text[i * 2u + 1u]);

        if (high < 0 || low < 0)
            return false;
        output[i] = (uint8_t)((high << 4) | low);
    }
    return text[size * 2u] == '\0';
}

static bool json_hex(const char *json, const char *key,
                     uint8_t *output, uint32_t size)
{
    char text[SHA256_DIGEST_SIZE * 2u + 1u];

    return size <= SHA256_DIGEST_SIZE &&
           json_string(json, key, text, size * 2u + 1u) &&
           decode_hex(text, output, size);
}

/**
 * @brief 解析定位 XBIN 所需的最小清单字段。
 *
 * 清单保留版本、发布序号和摘要等可读元数据，但运行时只信任随后下载的
 * Formal Header，避免同一信息存在两份安全决策来源。
 */
static bool manifest_parse(const char *json, ota_candidate_t *candidate)
{
    char product[32];
    char format[16];
    uint32_t manifest_version;
    uint32_t format_revision;

    memset(candidate, 0, sizeof(*candidate));
    if (!json_u32(json, "manifest_version", &manifest_version) ||
        manifest_version != 2u ||
        !json_string(json, "product", product, sizeof(product)) ||
        strcmp(product, OTA_PRODUCT) != 0 ||
        !json_string(json, "xbin_format", format, sizeof(format)) ||
        strcmp(format, "formal") != 0 ||
        !json_u32(json, "format_revision", &format_revision) ||
        format_revision != MAGIC_HEADER_FORMAT_FORMAL ||
        !json_u32(json, "xbin_size", &candidate->xbin_size) ||
        !json_hex(json, "xbin_sha256", candidate->xbin_sha256,
                  sizeof(candidate->xbin_sha256)) ||
        !json_string(json, "download_url", candidate->download_url,
                     sizeof(candidate->download_url)))
        return false;

    return candidate->xbin_size > MAGIC_HEADER_AREA_SIZE &&
           candidate->xbin_size <= MAGIC_HEADER_AREA_SIZE + MAGIC_DATA_MAX_SIZE &&
           strncmp(candidate->download_url, "https://", 8u) == 0;
}

/** @brief 从已打开的 HTTP 正文读取精确字节数。 */
static bool http_read_exact(uint8_t *data, uint32_t size)
{
    uint32_t received_total = 0;

    while (received_total < size)
    {
        int32_t received = esp_at_http_read(data + received_total,
                                            size - received_total,
                                            OTA_HTTP_TIMEOUT_MS);
        if (received <= 0)
            return false;
        received_total += (uint32_t)received;
    }
    return true;
}

/** @brief 下载不超过 1 KiB 的完整文本资源。 */
static bool download_manifest(void)
{
    esp_http_info_t info;
    bool ok = false;

    if (!esp_at_http_open(OTA_MANIFEST_URL, ESP_HTTP_RANGE_NONE,
                          ESP_HTTP_RANGE_NONE, &info))
        return false;
    if (info.status_code == 200u && info.content_length > 0u &&
        info.content_length < sizeof(manifest_buffer) &&
        http_read_exact((uint8_t *)manifest_buffer, info.content_length))
    {
        manifest_buffer[info.content_length] = '\0';
        ok = true;
    }
    esp_at_http_close();
    return ok;
}

/** @brief 用Range请求读取完整的320字节Formal Header。 */
static bool download_candidate_header(ota_candidate_t *candidate)
{
    uint8_t header_area[MAGIC_HEADER_SIZE];
    esp_http_info_t info;
    bool ok = false;

    if (!esp_at_http_open(candidate->download_url, 0u,
                          sizeof(header_area) - 1u, &info))
        return false;
    if (info.status_code == 206u && info.content_length == sizeof(header_area) &&
        info.total_length == candidate->xbin_size &&
        http_read_exact(header_area, sizeof(header_area)))
        ok = true;
    esp_at_http_close();

    if (!ok)
        return false;

    memcpy(&candidate->header, header_area, sizeof(candidate->header));

    if (!magic_header_validate_data(&candidate->header) ||
        memchr(candidate->header.version, '\0', MAGIC_VERSION_SIZE) == NULL)
        return false;
    return candidate->header.data_offset + candidate->header.data_length ==
           candidate->xbin_size;
}

/**
 * @brief 联网检查清单并缓存一份通过格式一致性检查的候选固件。
 * @param[out] candidate 可自动更新时返回完整候选信息。
 * @return 候选发布序号更大且满足安全版本规则时返回 true。
 */
static bool ota_check(ota_candidate_t *candidate)
{
    boot_state_t state;
    const magic_header_t *current = (const magic_header_t *)MAGIC_HEADER_ADDR;
    firmware_release_decision_t release_decision;
    uint8_t target_slot;

    printf("[OTA] checking manifest\n");
    if (!wifi_is_connected() || !download_manifest() ||
        !manifest_parse(manifest_buffer, candidate))
    {
        printf("[OTA] manifest check failed\n");
        return false;
    }
    if (!ab_boot_state_load(&state) || state.phase != BOOT_PHASE_NORMAL ||
        state.active_slot >= BOOT_SLOT_COUNT)
    {
        printf("[OTA] A/B state is not ready for an update\n");
        return false;
    }
    if (!download_candidate_header(candidate))
    {
        printf("[OTA] candidate header does not match manifest\n");
        return false;
    }
    if (!firmware_security_version_allowed(
            state.minimum_security_version,
            candidate->header.security_version, false))
    {
        printf("[OTA] candidate security version is below device floor\n");
        return false;
    }
    if (!magic_header_validate_data(current))
    {
        printf("[OTA] current firmware header is invalid\n");
        return false;
    }

    release_decision = firmware_release_decide(
        current->release_sequence, current->image_sha256,
        candidate->header.release_sequence, candidate->header.image_sha256);
    if (release_decision == FIRMWARE_RELEASE_CURRENT)
    {
        printf("[OTA] firmware is already current (%s, release %lu)\n",
               current->version, (unsigned long)current->release_sequence);
        return false;
    }
    if (release_decision == FIRMWARE_RELEASE_OLDER)
    {
        printf("[OTA] older release ignored: current=%lu candidate=%lu\n",
               (unsigned long)current->release_sequence,
               (unsigned long)candidate->header.release_sequence);
        return false;
    }
    if (release_decision == FIRMWARE_RELEASE_CONFLICT)
    {
        printf("[OTA] release %lu has different content; package rejected\n",
               (unsigned long)candidate->header.release_sequence);
        return false;
    }

    target_slot = state.active_slot == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;
    if (install_request_matches(candidate, target_slot))
    {
        update_ready_set(true);
        printf("[OTA] firmware %s is ready; press RESET to install\n",
               candidate->header.version);
        return false;
    }

    printf("[OTA] update available: %s, release %lu, %lu bytes\n",
           candidate->header.version,
           (unsigned long)candidate->header.release_sequence,
           (unsigned long)candidate->xbin_size);
    return true;
}

static bool progress_valid(const ota_progress_t *progress,
                           const ota_candidate_t *candidate,
                           uint8_t target_slot)
{
    uint32_t crc = crc32((const uint8_t *)progress,
                         offsetof(ota_progress_t, record_crc32));

    return progress->magic == OTA_PROGRESS_MAGIC &&
           progress->record_crc32 == crc &&
           progress->target_slot == target_slot &&
           progress->xbin_size == candidate->xbin_size &&
           progress->security_version == candidate->header.security_version &&
           progress->next_offset >= MAGIC_HEADER_AREA_SIZE &&
           progress->next_offset <= candidate->xbin_size &&
           (progress->next_offset - MAGIC_HEADER_AREA_SIZE) %
               OTA_DOWNLOAD_CHECKPOINT == 0u &&
           memcmp(progress->xbin_sha256, candidate->xbin_sha256,
                  SHA256_DIGEST_SIZE) == 0;
}

static bool progress_save(ota_progress_t *progress)
{
    ota_progress_t verify;

    progress->record_crc32 = crc32((const uint8_t *)progress,
                                   offsetof(ota_progress_t, record_crc32));
    return bl24c512_write(OTA_PROGRESS_ADDR, (const uint8_t *)progress,
                          sizeof(*progress)) &&
           bl24c512_read(OTA_PROGRESS_ADDR, (uint8_t *)&verify,
                         sizeof(verify)) &&
           memcmp(progress, &verify, sizeof(*progress)) == 0;
}

static void progress_clear(void)
{
    const uint32_t empty_magic = 0u;

    bl24c512_write(OTA_PROGRESS_ADDR, (const uint8_t *)&empty_magic,
                   sizeof(empty_magic));
}

/** @brief 从服务器把 XBIN 数据区写到非活动槽，Header 仍保持无效。 */
static bool download_payload(const ota_candidate_t *candidate,
                             uint8_t target_slot)
{
    ota_progress_t progress;
    esp_http_info_t info;
    uint32_t rounded_size = (candidate->xbin_size + W25Q_SECTOR_SIZE - 1u) &
                            ~(W25Q_SECTOR_SIZE - 1u);
    uint32_t offset;
    uint32_t next_checkpoint;
    bool resumed;
    bool ok = false;
    bool baud_restored;

    resumed = bl24c512_read(OTA_PROGRESS_ADDR, (uint8_t *)&progress,
                            sizeof(progress)) &&
              progress_valid(&progress, candidate, target_slot);
    if (!resumed)
    {
        memset(&progress, 0, sizeof(progress));
        progress.magic = OTA_PROGRESS_MAGIC;
        progress.target_slot = target_slot;
        progress.xbin_size = candidate->xbin_size;
        progress.next_offset = MAGIC_HEADER_AREA_SIZE;
        progress.security_version = candidate->header.security_version;
        memcpy(progress.xbin_sha256, candidate->xbin_sha256,
               sizeof(progress.xbin_sha256));
    }

    offset = progress.next_offset;
    printf("[OTA] %s at offset %lu\n", resumed ? "resuming" : "starting",
           (unsigned long)offset);

    /* 重下区域先擦除；最后一个已保存检查点之前的内容保持不变。 */
    if (!w25q_erase((w25q_chip_t)target_slot,
                    resumed ? offset : 0u,
                    rounded_size - (resumed ? offset : 0u)) ||
        !progress_save(&progress))
        return false;

    if (offset == candidate->xbin_size)
        return true;
    /* 槽擦除完成后才进入高速会话。驱动从切换波特率到关闭并恢复115200
     * 始终持有同一个ESP互斥锁，天气和Wi-Fi任务不能插入。 */
    if (!esp_at_http_open_at_baud(candidate->download_url, offset,
                                  ESP_HTTP_RANGE_NONE, &info,
                                  OTA_DOWNLOAD_BAUD))
        return false;
    printf("[OTA] USART2 download baud: %lu\n",
           (unsigned long)OTA_DOWNLOAD_BAUD);
    if (info.status_code != 206u || info.range_start != offset ||
        info.total_length != candidate->xbin_size ||
        info.content_length != candidate->xbin_size - offset)
        goto out;

    next_checkpoint = offset + OTA_DOWNLOAD_CHECKPOINT;
    while (offset < candidate->xbin_size)
    {
        uint32_t chunk = candidate->xbin_size - offset;
        int32_t received;

        if (chunk > sizeof(stream_buffer))
            chunk = sizeof(stream_buffer);
        if (offset + chunk > next_checkpoint)
            chunk = next_checkpoint - offset;
        received = esp_at_http_read(stream_buffer, chunk, OTA_HTTP_TIMEOUT_MS);
        if (received <= 0 ||
            !w25q_program((w25q_chip_t)target_slot, offset, stream_buffer,
                          (uint32_t)received))
            goto out;
        offset += (uint32_t)received;

        if (offset == next_checkpoint)
        {
            progress.next_offset = offset;
            if (!progress_save(&progress))
                goto out;
            printf("[OTA] downloaded %lu / %lu\n", (unsigned long)offset,
                   (unsigned long)candidate->xbin_size);
            next_checkpoint += OTA_DOWNLOAD_CHECKPOINT;
        }
    }
    ok = true;
out:
    baud_restored = esp_at_http_close();
    return ok && baud_restored;
}

/**
 * @brief 重新扫描外部槽并验证完整XBIN的下载摘要。
 *
 * WeatherClock不持有AES密钥，只确认W25Q中的完整加密包与COS清单一致。
 * ECDSA、GCM Tag以及明文CRC/SHA仍由Bootloader在复位后独立验证。
 */
static bool verify_download(const ota_candidate_t *candidate,
                             uint8_t target_slot)
{
    static const uint8_t zeros[256] = {0};
    sha256_context_t xbin_sha;
    uint8_t xbin_digest[SHA256_DIGEST_SIZE];
    uint32_t offset = MAGIC_HEADER_AREA_SIZE;
    uint32_t i;
    bool xbin_ok;

    sha256_init(&xbin_sha);
    sha256_update(&xbin_sha, (const uint8_t *)&candidate->header,
                  sizeof(candidate->header));
    for (i = MAGIC_HEADER_SIZE; i < MAGIC_HEADER_AREA_SIZE; i += sizeof(zeros))
    {
        uint32_t chunk = MAGIC_HEADER_AREA_SIZE - i;
        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        sha256_update(&xbin_sha, zeros, chunk);
    }

    while (offset < candidate->xbin_size)
    {
        uint32_t chunk = candidate->xbin_size - offset;

        if (chunk > sizeof(stream_buffer))
            chunk = sizeof(stream_buffer);
        w25q_read((w25q_chip_t)target_slot, offset, stream_buffer, chunk);
        sha256_update(&xbin_sha, stream_buffer, chunk);
        offset += chunk;
    }
    sha256_final(&xbin_sha, xbin_digest);

    xbin_ok = memcmp(xbin_digest, candidate->xbin_sha256,
                     sizeof(xbin_digest)) == 0;
    printf("[OTA] verify encrypted XBIN=%s\n", xbin_ok ? "ok" : "bad");
    return xbin_ok;
}

/** @brief 最后写 Header，使外部槽从“半成品”一次变为有效候选。 */
static bool commit_header(const ota_candidate_t *candidate,
                           uint8_t target_slot)
{
    static const uint8_t zeros[256] = {0};
    magic_header_t verify_header;
    uint32_t offset;

    /* Header区的填充先写，真正有效的320字节Formal Header始终最后提交。 */
    for (offset = MAGIC_HEADER_SIZE; offset < MAGIC_HEADER_AREA_SIZE;
         offset += sizeof(zeros))
    {
        uint32_t chunk = MAGIC_HEADER_AREA_SIZE - offset;
        if (chunk > sizeof(zeros))
            chunk = sizeof(zeros);
        if (!w25q_program((w25q_chip_t)target_slot, offset, zeros,
                          chunk))
            return false;
    }
    if (!w25q_program((w25q_chip_t)target_slot, 0u,
                      (const uint8_t *)&candidate->header,
                      sizeof(candidate->header)))
        return false;
    w25q_read((w25q_chip_t)target_slot, 0u,
              (uint8_t *)&verify_header, sizeof(verify_header));
    return memcmp(&verify_header, &candidate->header,
                  sizeof(verify_header)) == 0;
}

/** @brief 写入一次性请求；此后只有 Bootloader 能决定是否安装。 */
static bool request_install(const ota_candidate_t *candidate,
                            uint8_t target_slot)
{
    ota_request_t request;
    ota_request_t verify;

    memset(&request, 0, sizeof(request));
    request.magic = OTA_REQUEST_MAGIC;
    request.target_slot = target_slot;
    request.xbin_size = candidate->xbin_size;
    request.header_crc32 = candidate->header.header_crc32;
    request.payload_crc32 = candidate->header.data_crc32;
    request.security_version = candidate->header.security_version;
    memcpy(request.image_sha256, candidate->header.image_sha256,
           sizeof(request.image_sha256));
    request.record_crc32 = crc32((const uint8_t *)&request,
                                 offsetof(ota_request_t, record_crc32));

    return bl24c512_write(OTA_REQUEST_ADDR, (const uint8_t *)&request,
                          sizeof(request)) &&
           bl24c512_read(OTA_REQUEST_ADDR, (uint8_t *)&verify,
                         sizeof(verify)) &&
           memcmp(&request, &verify, sizeof(request)) == 0;
}

static bool ota_download_and_stage(const ota_candidate_t *candidate)
{
    boot_state_t state;
    uint8_t target_slot;
    bool downloaded;
    bool ok;

    if (!ab_boot_state_load(&state) ||
        state.phase != BOOT_PHASE_NORMAL ||
        state.active_slot >= BOOT_SLOT_COUNT)
        return false;
    target_slot = state.active_slot == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;

    w25q_init();
    if (w25q_read_jedec_id((w25q_chip_t)target_slot) != W25Q_JEDEC_W25Q128)
    {
        printf("[OTA] target W25Q128 is not available\n");
        w25q_deinit();
        return false;
    }
    printf("[OTA] downloading %s to slot %c\n", candidate->header.version,
           target_slot == BOOT_SLOT_A ? 'A' : 'B');

    downloaded = download_payload(candidate, target_slot);

    ok = downloaded && verify_download(candidate, target_slot) &&
         commit_header(candidate, target_slot) &&
         request_install(candidate, target_slot);
    w25q_deinit();
    if (!ok)
    {
        printf("[OTA] download or verification failed; current firmware kept\n");
        return false;
    }

    progress_clear();
    update_ready_set(true);
    printf("[OTA] package verified; press RESET to install\n");
    return true;
}

/** @brief USART1 的 U 命令立即执行一次检查与下载，便于联调。 */
static void ota_console_received(uint8_t data)
{
    BaseType_t higher_priority_task_woken = pdFALSE;

    if ((data == 'u' || data == 'U') && ota_task_handle != NULL)
    {
        xTaskNotifyFromISR(ota_task_handle, OTA_EVENT_UPDATE, eSetBits,
                           &higher_priority_task_woken);
        portYIELD_FROM_ISR(higher_priority_task_woken);
    }
}

static void ota_task(void *parameter)
{
    ota_candidate_t candidate;
    uint32_t events;
    TickType_t wait_time = portMAX_DELAY;
    bool automatic_ready = false;

    (void)parameter;
    printf("[OTA] waiting for first indoor and online weather update\n");
    printf("[OTA] 'u'=check/download now\n");
    while (1)
    {
        if (xTaskNotifyWait(0u, UINT32_MAX, &events, wait_time) != pdTRUE)
            events = OTA_EVENT_UPDATE;

        if (events & OTA_EVENT_APP_READY)
        {
            automatic_ready = true;
            wait_time = OTA_FIRST_CHECK_TICKS;
            printf("[OTA] app data ready; automatic check in 60 seconds\n");
        }
        if ((events & OTA_EVENT_UPDATE) && ota_check(&candidate))
            ota_download_and_stage(&candidate);
        if ((events & OTA_EVENT_UPDATE) && automatic_ready)
            wait_time = OTA_PERIODIC_CHECK_TICKS;
    }
}

void ota_start(void)
{
    ota_request_t request;

    led_init(led1);
    led_init(led2);
    led_init(led3);
    bl24c512_init();
    if (bl24c512_read(OTA_REQUEST_ADDR, (uint8_t *)&request,
                      sizeof(request)) && install_request_valid(&request))
        update_ready_set(true);

    configASSERT(xTaskCreate(ota_task, "ota", 1024, NULL, 3,
                             &ota_task_handle) == pdPASS);
    console_received_register(ota_console_received);
}

void ota_app_ready(void)
{
    if (ota_task_handle != NULL)
        xTaskNotify(ota_task_handle, OTA_EVENT_APP_READY, eSetBits);
}
