#ifndef __AB_CONFIRM_H__
#define __AB_CONFIRM_H__

#include <stdbool.h>
#include "ab_boot_contract.h"

/**
 * @brief 读取应用侧可见的最新 A/B 启动状态。
 *
 * OTA只用它确定当前活动槽和防降级下限；状态更新仍由确认任务或Bootloader
 * 完成，避免应用侧出现两套不同的状态转换逻辑。
 */
bool ab_boot_state_load(boot_state_t *state);

/**
 * @brief 创建一次性的A/B试运行确认任务。
 *
 * 非Trial启动时任务会在延时后直接退出；Trial启动时，任务在运行5秒后校验
 * 当前固件身份并更新EEPROM中的BCR。
 */
void ab_confirm_start(void);

#endif /* __AB_CONFIRM_H__ */
