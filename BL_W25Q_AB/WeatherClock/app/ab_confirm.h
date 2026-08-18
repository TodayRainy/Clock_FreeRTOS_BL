#ifndef __AB_CONFIRM_H__
#define __AB_CONFIRM_H__

/**
 * @brief 创建一次性的A/B试运行确认任务。
 *
 * 非Trial启动时任务会在延时后直接退出；Trial启动时，任务在运行5秒后校验
 * 当前固件身份并更新EEPROM中的BCR。
 */
void ab_confirm_start(void);

#endif /* __AB_CONFIRM_H__ */
