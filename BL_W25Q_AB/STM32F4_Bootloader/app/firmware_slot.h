#ifndef __FIRMWARE_SLOT_H__
#define __FIRMWARE_SLOT_H__

#include <stdbool.h>
#include <stdint.h>
#include "magic_header.h"

#define FIRMWARE_SLOT_SIZE (1024u * 1024u)

/**
 * @brief 校验 STM32 内部 Flash 中的完整应用固件。
 *
 * @param[out] header 返回已通过格式校验的内部 Header。
 * @return Header和应用数据CRC32均正确时返回 true。
 */
bool firmware_internal_validate(magic_header_t *header);

/**
 * @brief 从指定外部槽读取并校验固件 Header。
 *
 * 本函数不扫描应用数据，只确认Header自身及槽内边界合法。
 *
 * @param[in] slot 槽号，0为A，1为B。
 * @param[out] header 返回有效的Header。
 * @return Header有效且整个镜像位于1 MiB槽内时返回 true。
 */
bool firmware_slot_load_header(uint8_t slot, magic_header_t *header);

/**
 * @brief 使用已经校验的Header计算外部槽中应用数据的CRC32。
 *
 * @param[in] slot 槽号，0为A，1为B。
 * @param[in] header 由 @ref firmware_slot_load_header 得到的Header。
 * @return 实际CRC32与Header记录值相等时返回 true。
 */
bool firmware_slot_verify_data(uint8_t slot, const magic_header_t *header);

/**
 * @brief 完整校验指定外部固件槽。
 *
 * @param[in] slot 槽号，0为A，1为B。
 * @param[out] header 返回通过完整校验的Header。
 * @return Header和应用数据均有效时返回 true。
 */
bool firmware_slot_validate(uint8_t slot, magic_header_t *header);

/**
 * @brief 将外部槽中的完整固件安装到STM32内部应用区。
 *
 * 安装顺序固定为：外部全量校验、擦除内部Header、写应用、内部CRC校验、
 * 最后写Header。任何中断都会让内部半成品保持不可启动状态。
 *
 * @param[in] slot 要安装的外部槽。
 * @return 安装并校验成功时返回 true。
 */
bool firmware_slot_install(uint8_t slot);

/**
 * @brief 把当前有效的内部固件快照到指定外部槽。
 *
 * 先写应用数据，最后写有效Header，因此中断不会留下可被误识别的半成品槽。
 *
 * @param[in] slot 快照目标槽。
 * @return 外部快照通过完整校验时返回 true。
 */
bool firmware_slot_snapshot_internal(uint8_t slot);

#endif /* __FIRMWARE_SLOT_H__ */
