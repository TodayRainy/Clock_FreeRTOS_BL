/**
 * @file firmware_release_policy.h
 * @brief 自动 OTA 使用的普通固件发布顺序规则。
 *
 * `release_sequence`回答“哪个包更新”，SHA-256回答“是不是同一个包”。
 * 安全漏洞形成的永久防降级下限仍由`security_version`独立负责。
 */

#ifndef __FIRMWARE_RELEASE_POLICY_H__
#define __FIRMWARE_RELEASE_POLICY_H__

#include <stdint.h>
#include <string.h>

/** @brief 候选固件相对当前固件的自动 OTA 判定。 */
typedef enum
{
    FIRMWARE_RELEASE_CURRENT = 0, /**< 序号和内容均相同。 */
    FIRMWARE_RELEASE_UPDATE,      /**< 候选序号更大，可以自动更新。 */
    FIRMWARE_RELEASE_OLDER,       /**< 候选序号更小，只能走人工降级。 */
    FIRMWARE_RELEASE_CONFLICT     /**< 序号相同但内容不同，发布包有冲突。 */
} firmware_release_decision_t;

/**
 * @brief 按发布序号和镜像摘要判断候选固件。
 *
 * @param[in] current_sequence 当前正在运行固件的发布序号。
 * @param[in] current_sha256 当前固件数据区的 SHA-256。
 * @param[in] candidate_sequence 候选固件的发布序号。
 * @param[in] candidate_sha256 候选固件数据区的 SHA-256。
 * @return 自动 OTA 应采取的处理方式。
 */
static inline firmware_release_decision_t firmware_release_decide(
    uint32_t current_sequence,
    const uint8_t current_sha256[32],
    uint32_t candidate_sequence,
    const uint8_t candidate_sha256[32])
{
    if (candidate_sequence > current_sequence)
        return FIRMWARE_RELEASE_UPDATE;
    if (candidate_sequence < current_sequence)
        return FIRMWARE_RELEASE_OLDER;
    return memcmp(current_sha256, candidate_sha256, 32u) == 0
               ? FIRMWARE_RELEASE_CURRENT
               : FIRMWARE_RELEASE_CONFLICT;
}

#endif /* __FIRMWARE_RELEASE_POLICY_H__ */
