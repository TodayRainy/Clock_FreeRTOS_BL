#include <stdbool.h>
#include <stddef.h>
#include "stm32f4xx_ll_gpio.h"
#include "key.h"
#include "key_desc.h"

// KEY1: PA0
// KEY2: PC4
// KEY3: PC5

void key_init(key_desc_t key)
{
    LL_GPIO_InitTypeDef GPIO_InitStructure = {0};
    GPIO_InitStructure.Mode  = LL_GPIO_MODE_INPUT;
    GPIO_InitStructure.Pull  = key->pupd;
    GPIO_InitStructure.Speed = LL_GPIO_SPEED_FREQ_MEDIUM;
    GPIO_InitStructure.Pin   = key->pin;
    LL_GPIO_Init(key->port, &GPIO_InitStructure);
}

bool key_read(key_desc_t key)
{
    return LL_GPIO_IsInputPinSet(key->port, key->pin) == key->press_level;
}
