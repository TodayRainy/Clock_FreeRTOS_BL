/**
 * @file firmware_slot.h
 * @brief 内部应用和两颗外部固件槽的校验与安装接口。
 */

#ifndef __FIRMWARE_SLOT_H__
#define __FIRMWARE_SLOT_H__

#include <stdbool.h>
#include <stdint.h>
#include "magic_header.h"

#define FIRMWARE_SLOT_SIZE (1024u * 1024u)
#define FIRMWARE_RESUME_BLOCK_SIZE 4096u

/**
 * @brief 校验 STM32 内部 Flash 中的完整应用固件。
 *
 * @param[out] header 返回已通过格式校验的内部 Header。
 * @return Header签名、应用CRC32、SHA-256和向量表均正确时返回 true。
 */
bool firmware_internal_validate(magic_header_t *header);

/**
 * @brief 从指定外部槽读取并校验固件 Header。
 *
 * 本函数不扫描应用数据，只确认Header自身、ECDSA签名及槽内边界合法。
 *
 * @param[in] slot 槽号，0为A，1为B。
 * @param[out] header 返回有效的Header。
 * @return Header有效且整个镜像位于1 MiB槽内时返回 true。
 */
bool firmware_slot_load_header(uint8_t slot, magic_header_t *header);

/**
 * @brief 使用已经验签的Header校验外部槽应用数据。
 *
 * @param[in] slot 槽号，0为A，1为B。
 * @param[in] header 由 @ref firmware_slot_load_header 得到的Header。
 * @return CRC32、SHA-256和应用向量表均符合Header时返回 true。
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
 * @brief 查找外部槽与候选固件之间第一个不一致的数据块。
 *
 * 断点续传只比较从 `MAGIC_HEADER_AREA_SIZE` 开始的应用数据，不比较Header。
 * Header由协议层先失效、最后重新提交，因此任何未完成的续传都不会形成有效槽。
 * 每个CRC覆盖一个4 KiB块；最后一块只覆盖实际固件长度，不包含扇区尾部填充。
 *
 * @param[in] slot 要扫描的非活动槽。
 * @param[in] data_length 候选固件应用数据长度。
 * @param[in] expected_crc32 上位机按顺序提供的分块CRC32数组。
 * @param[in] block_count CRC32数组元素数。
 * @param[out] resume_offset 返回槽内续传地址；全部匹配时返回数据末地址。
 * @return 参数及槽号有效并完成扫描时返回 true。
 */
bool firmware_slot_find_resume_offset(uint8_t slot, uint32_t data_length,
                                      const uint32_t *expected_crc32,
                                      uint16_t block_count,
                                      uint32_t *resume_offset);

/**
 * @brief 将外部槽中的完整固件安装到STM32内部应用区。
 *
 * 调用方必须紧邻本调用完成 @ref firmware_slot_validate，并传入当时得到的
 * Header。安装顺序固定为：擦除内部Header、写应用、内部CRC校验、最后写
 * Header。复制后同时复核CRC32、SHA-256和向量表；任何中断都会让内部半成品
 * 保持不可启动状态。
 *
 * @param[in] slot 要安装的外部槽。
 * @param[in] verified_header 已通过完整槽校验的Header。
 * @return 安装并校验成功时返回 true。
 */
bool firmware_slot_install(uint8_t slot, const magic_header_t *verified_header);

#endif /* __FIRMWARE_SLOT_H__ */
