#ifndef CLOCK_F407_KEY_H
#define CLOCK_F407_KEY_H

#include <stdbool.h>
#include <stdint.h>

struct key_desc;
typedef struct key_desc *key_desc_t;

/** @brief 按描述对象配置一个按键GPIO输入。 */
void key_init(key_desc_t key);

/** @brief 返回按键当前是否处于按下电平。 */
bool key_read(key_desc_t key);

#endif /* CLOCK_F407_KEY_H */
