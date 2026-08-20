/**
 * @file bootloader.h
 * @brief Bootloader顶层入口。
 */

#ifndef CLOCK_F407_BOOTLOADER_H
#define CLOCK_F407_BOOTLOADER_H

/**
 * @brief 初始化升级相关硬件，执行A/B状态机并提供串口升级服务。
 *
 * 有可启动应用且没有进入请求时，本函数最终跳转到应用且不返回；否则持续运行
 * Bootloader协议循环，等待上位机或按键操作。
 */
void bootloader_main(void);

#endif /* CLOCK_F407_BOOTLOADER_H */
