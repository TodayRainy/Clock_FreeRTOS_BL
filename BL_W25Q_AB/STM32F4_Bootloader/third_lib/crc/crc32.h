#ifndef _CRC_CRC32_H
#define _CRC_CRC32_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus  */

#include <stddef.h>
#include <stdint.h>

uint32_t crc32(const unsigned char *s, size_t len);
uint32_t crc32_begin(void);
uint32_t crc32_update(uint32_t state, const unsigned char *s, size_t len);
uint32_t crc32_end(uint32_t state);

#ifdef __cplusplus
}
#endif /* __cplusplus  */
#endif
