/**
 * @file ota_contract.h
 * @brief WeatherClock与Bootloader共享的OTA持久化记录格式。
 *
 * EEPROM的0xFC00..0xFCFF原本就是维护区。前一页保存可丢弃的下载进度，
 * 后一页保存一次性的安装请求；0xFD00和0xFD80仍只保存A/B启动状态。
 * 下载进度损坏时最多重新下载，不影响当前固件。安装请求则必须通过CRC，且
 * Bootloader仍会独立完成签名、摘要和防降级检查。
 */

#ifndef __OTA_CONTRACT_H__
#define __OTA_CONTRACT_H__

#include <stdint.h>

#define OTA_PROGRESS_MAGIC          0x3152504Fu /**< 小端内存中为"OPR1"。 */
#define OTA_REQUEST_MAGIC           0x3151524Fu /**< 小端内存中为"ORQ1"。 */
#define OTA_PROGRESS_ADDR           0xFC00u
#define OTA_REQUEST_ADDR            0xFC80u
#define OTA_RECORD_SIZE             128u
#define OTA_DOWNLOAD_CHECKPOINT     (32u * 1024u)

/**
 * @brief OTA断点记录。
 *
 * `next_offset`只在一个完整检查点写入W25Q后更新。记录无效时应用直接擦除
 * 非活动槽并重下，不需要为了提高下载命中率再引入双副本事务。
 */
typedef struct
{
    uint32_t magic;
    uint8_t target_slot;
    uint8_t reserved0[3];
    uint32_t xbin_size;
    uint32_t next_offset;
    uint32_t security_version;
    uint8_t xbin_sha256[32];
    uint8_t reserved[72];
    uint32_t record_crc32;
} ota_progress_t;

/**
 * @brief 应用写给Bootloader的一次性安装请求。
 *
 * 这些字段只用来确认Bootloader检查的是应用刚刚下载的候选镜像，不能替代
 * XBIN签名验证。Bootloader接受请求后仍按原有InstallPending流程安装。
 */
typedef struct
{
    uint32_t magic;
    uint8_t target_slot;
    uint8_t reserved0[3];
    uint32_t xbin_size;
    uint32_t header_crc32;
    uint32_t payload_crc32;
    uint32_t security_version;
    uint8_t image_sha256[32];
    uint8_t reserved[68];
    uint32_t record_crc32;
} ota_request_t;

typedef char ota_progress_record_must_be_128_bytes[
    (sizeof(ota_progress_t) == OTA_RECORD_SIZE) ? 1 : -1];
typedef char ota_request_record_must_be_128_bytes[
    (sizeof(ota_request_t) == OTA_RECORD_SIZE) ? 1 : -1];

#endif /* __OTA_CONTRACT_H__ */
