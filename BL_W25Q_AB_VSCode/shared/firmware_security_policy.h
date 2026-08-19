/**
 * @file firmware_security_policy.h
 * @brief Bootloader 与应用共用的固件防降级规则。
 *
 * 安全版本是独立于显示版本的单调计数。正常升级只能安装不低于当前下限的
 * 固件；KEY2 物理授权可以安装较低版本，但不会降低已经确认的安全下限。
 */

#ifndef __FIRMWARE_SECURITY_POLICY_H__
#define __FIRMWARE_SECURITY_POLICY_H__

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 判断一个已通过签名验证的候选固件是否允许提交。
 * @param[in] minimum_version 当前设备已经确认的安全版本下限。
 * @param[in] candidate_version 候选 XBIN 中经过签名保护的安全版本。
 * @param[in] physical_override 是否通过 KEY2 明确授权人工降级。
 */
static inline bool firmware_security_version_allowed(uint32_t minimum_version,
                                                     uint32_t candidate_version,
                                                     bool physical_override)
{
    return physical_override || candidate_version >= minimum_version;
}

/**
 * @brief Trial 成功时计算新的安全版本下限。
 *
 * 即使当前 Trial 是 KEY2 授权的低版本，已确认下限也只保持或提高，不会下降。
 */
static inline uint32_t firmware_security_floor_after_confirm(
    uint32_t minimum_version, uint32_t confirmed_version)
{
    return confirmed_version > minimum_version ? confirmed_version : minimum_version;
}

#endif /* __FIRMWARE_SECURITY_POLICY_H__ */
