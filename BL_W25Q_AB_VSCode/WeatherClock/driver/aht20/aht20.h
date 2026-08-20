#ifndef __AHT20_H__
#define __AHT20_H__

#include <stdbool.h>
#include <stdint.h>

bool aht20_init(void);
bool aht20_start_measurement(void);
bool aht20_wait_for_measurement(void);

/**
 * @brief 读取AHT20测量结果。
 * @param[out] temperature_x10 摄氏温度乘10，例如23.4℃返回234。
 * @param[out] humidity_x10 相对湿度百分比乘10，例如56.7%返回567。
 * @return I2C读取成功时返回true。
 */
bool aht20_read_measurement(int16_t *temperature_x10,
                            uint16_t *humidity_x10);

#endif /* __AHT20_H__ */
