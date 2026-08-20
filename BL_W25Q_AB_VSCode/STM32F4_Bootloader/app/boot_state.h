#ifndef __BOOT_STATE_H__
#define __BOOT_STATE_H__

#include <stdbool.h>
#include "../../shared/ab_boot_contract.h"

/**
 * @brief 生成未初始化的默认状态。
 *
 * 默认状态为Normal，但没有活动槽。生产初始化时由Bootloader采用一个与内部
 * Formal固件匹配且完整有效的外部槽，不再从内部Flash生成外部快照。
 *
 * @param[out] state 接收默认状态的对象。
 */
void boot_state_default(boot_state_t *state);

/**
 * @brief 从 EEPROM 双副本中读取最新的有效状态。
 *
 * @param[out] state 成功时接收最新记录；没有有效记录时接收默认状态。
 * 旧版记录保留区中的0xFFFFFFFF安全版本会在RAM中迁移为0。
 * @return 找到至少一份通过格式和 CRC32 校验的记录时返回 true。
 */
bool boot_state_load(boot_state_t *state);

/**
 * @brief 将状态写入较旧副本的另一页，并进行读回比较。
 *
 * 函数先递增 sequence，再根据其奇偶选择0xFD00或0xFD80。掉电最多破坏
 * 正在写的新页，另一页中的上一条完整记录仍然可用。
 *
 * @param[in,out] state 要保存的状态；成功或失败后 sequence 均已递增。
 * @return 写入和读回完全一致时返回 true。
 */
bool boot_state_save(boot_state_t *state);

#endif /* __BOOT_STATE_H__ */
