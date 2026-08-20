/**
 * @file magic_header.h
 * @brief XBIN Header固定格式与CRC32校验接口。
 */

#ifndef __MAGIC_HEADER_H__
#define __MAGIC_HEADER_H__

#include <stdbool.h>
#include "../../shared/firmware_image_format.h"
#include "../../shared/firmware_encryption_format.h"

/**
 * @brief 校验一份已经读入内存的固件 Header。
 *
 * 除 Header 自身 CRC32 外，还会检查所有决定内部 Flash 擦写范围的字段。
 * 只有通过本函数的 Header 才能用于遍历固件数据或执行安装。
 *
 * @param[in] header 待校验的256字节 Header。
 * @return 格式、地址、长度及 Header CRC32 全部正确时返回 true。
 */
bool magic_header_validate_data(const magic_header_t *header);

/**
 * @brief 校验 XBIN v3 的 64 字节 AES-GCM 描述。
 *
 * 本函数只检查固定字段、保留字和结构 CRC；真正的密钥 ID 与 GCM Tag 由
 * Bootloader 在扫描密文时验证。
 *
 * @param[in] header 位于 XBIN Header 区偏移 256 的描述。
 * @return 二进制格式完整且算法为 AES-256-GCM 时返回 true。
 */
bool magic_encryption_header_validate(const xbin_encryption_header_t *header);

#endif /* __MAGIC_HEADER_H__ */
