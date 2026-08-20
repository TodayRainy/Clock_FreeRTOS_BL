/**
 * @file magic_header.h
 * @brief XBIN Formal Header固定格式与CRC32校验接口。
 */

#ifndef CLOCK_F407_MAGIC_HEADER_H
#define CLOCK_F407_MAGIC_HEADER_H

#include <stdbool.h>
#include "../../shared/firmware_image_format.h"

/**
 * @brief 校验一份已经读入内存的固件 Header。
 *
 * 除 Header 自身 CRC32 外，还会检查所有决定内部 Flash 擦写范围的字段。
 * 只有通过本函数的 Header 才能用于遍历固件数据或执行安装。
 *
 * 本函数不执行签名或正文验证；调用方应在格式通过后继续调用相应认证接口。
 *
 * @param[in] header 待校验的 320 字节 Formal Header。
 * @return 格式、地址、长度及 Header CRC32 全部正确时返回 true。
 */
bool magic_header_validate_data(const magic_header_t *header);

#endif /* CLOCK_F407_MAGIC_HEADER_H */
