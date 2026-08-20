/**
 * @file key.h
 * @brief GPIO 按键与回调接口。
 */

#ifndef CLOCK_F407_APP_KEY_H
#define CLOCK_F407_APP_KEY_H

#include <stdbool.h>
#include <stdint.h>

struct key_desc;
typedef struct key_desc *key_desc_t;

typedef void (*key_func_t)(key_desc_t key);

/** @brief 按描述对象配置 GPIO 输入、EXTI 和 NVIC。 */
void key_init(key_desc_t key);

/** @brief 返回按键 GPIO 当前是否为高电平。 */
bool key_read(key_desc_t key);

/** @brief 保存由板级 EXTI 入口调用的按键回调。 */
void key_press_callback_register(key_desc_t key, key_func_t func);

#endif /* CLOCK_F407_APP_KEY_H */
