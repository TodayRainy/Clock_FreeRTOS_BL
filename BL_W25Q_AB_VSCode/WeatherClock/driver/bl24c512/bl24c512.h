#ifndef __BL24C512_H
#define __BL24C512_H

#include <stdbool.h>
#include <stdint.h>

#define BL24C512_PAGE_SIZE 128u

void bl24c512_init(void);
bool bl24c512_read(uint16_t address, uint8_t *data, uint32_t size);
bool bl24c512_write(uint16_t address, const uint8_t *data, uint32_t size);



#endif /* __BL24C512_H */
