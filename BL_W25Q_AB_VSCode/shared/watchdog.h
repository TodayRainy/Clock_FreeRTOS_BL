/**
 * @file watchdog.h
 * @brief Bootloader和应用共用的STM32独立看门狗最小接口。
 */

#ifndef CLOCK_F407_WATCHDOG_H
#define CLOCK_F407_WATCHDOG_H

/**
 * @brief 启动或重新配置独立看门狗。
 *
 * 采用最大分频和最大重装值，按典型32 kHz LSI计算超时约32秒。Bootloader
 * 启动后IWDG无法关闭，应用必须继续定期刷新。DEV构建在CPU被调试器暂停时
 * 冻结计数器，PROD构建不修改调试冻结位。
 */
void watchdog_init(void);

/** @brief 刷新独立看门狗计数器。 */
void watchdog_refresh(void);

#endif /* CLOCK_F407_WATCHDOG_H */
