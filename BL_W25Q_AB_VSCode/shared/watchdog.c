/**
 * @file watchdog.c
 * @brief STM32F407 IWDG的直接寄存器实现。
 *
 * 这里只使用IWDG规定的四个键值和三个配置寄存器，直接写寄存器比为两个固件
 * 各复制一层标准外设库封装更清楚，也避免额外链接未使用的IWDG接口。
 */

#include "stm32f4xx.h"
#include "watchdog.h"

#define IWDG_KEY_ENABLE_WRITE  0x5555u
#define IWDG_KEY_RELOAD        0xAAAAu
#define IWDG_KEY_START         0xCCCCu
#define IWDG_PRESCALER_256     6u
#define IWDG_RELOAD_MAX        0x0FFFu

void watchdog_init(void)
{
#ifdef FIRMWARE_BUILD_PROFILE_DEV
    /* 断点调试时暂停IWDG，不改变设备正常运行时的故障恢复行为。 */
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;
#endif

    /*
     * 先启动IWDG也会启动其LSI时钟。若这是由不带IWDG的旧Bootloader首次跳入
     * 新应用，LSI此前可能完全关闭；此时先写PR/RLR再等待SR会永久等不到同步。
     * 芯片复位后的默认超时约百毫秒，足够完成下面几次寄存器写入和同步。
     */
    IWDG->KR = IWDG_KEY_START;
    IWDG->KR = IWDG_KEY_ENABLE_WRITE;
    IWDG->PR = IWDG_PRESCALER_256;
    IWDG->RLR = IWDG_RELOAD_MAX;
    while (IWDG->SR != 0u);
    IWDG->KR = IWDG_KEY_RELOAD;
}

void watchdog_refresh(void)
{
    IWDG->KR = IWDG_KEY_RELOAD;
}
