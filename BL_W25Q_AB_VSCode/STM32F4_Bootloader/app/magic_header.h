#ifndef __MAGIC_HEADER_H__
#define __MAGIC_HEADER_H__

#include <stdbool.h>
#include "../../shared/firmware_image_format.h"

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
 * @brief 校验位于 STM32 内部固定地址的固件 Header。
 * @return 内部 Header 有效时返回 true。
 */
bool magic_header_validate(void);

#endif /* __MAGIC_HEADER_H__ */
