/**
 * @file boot_protocol.h
 * @brief Bootloader串口协议的接收、解析和命令执行接口。
 */

#ifndef __BOOT_PROTOCOL_H__
#define __BOOT_PROTOCOL_H__

#include <stdbool.h>

/**
 * @brief 协议处理后需要由Bootloader主流程执行的动作。
 */
typedef enum
{
    BOOT_PROTOCOL_ACTION_NONE = 0,
    BOOT_PROTOCOL_ACTION_BOOT_APPLICATION,
} boot_protocol_action_t;

/**
 * @brief 初始化USART3协议通道和循环接收DMA。
 */
void boot_protocol_init(void);

/**
 * @brief 判断协议串口DMA是否已经收到尚未处理的数据。
 *
 * 启动等待窗口使用该函数判断是否应停留在Bootloader。
 *
 * @return DMA缓冲区非空时返回 true。
 */
bool boot_protocol_data_pending(void);

/**
 * @brief 处理DMA缓冲区中已经到达的数据。
 *
 * 函数会一直解析到DMA暂时为空或得到一个完整帧。完整帧在本函数内完成
 * CRC校验和命令分发；BOOT命令返回动作给主流程，确保应用启动统一经过
 * A/B状态决策。
 *
 * @return 当前完整命令要求主流程执行的动作。
 */
boot_protocol_action_t boot_protocol_process(void);

#endif /* __BOOT_PROTOCOL_H__ */
