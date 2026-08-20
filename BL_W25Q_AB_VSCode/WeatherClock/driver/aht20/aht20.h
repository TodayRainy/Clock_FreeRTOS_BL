/**
 * @file aht20.h
 * @brief AHT20 温湿度传感器接口。
 */

#ifndef CLOCK_F407_AHT20_H
#define CLOCK_F407_AHT20_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief 初始化 I2C2 并在需要时校准 AHT20。
 * @return 传感器就绪时返回 true。
 */
bool aht20_init(void);

/** @brief 向 AHT20 发送一次温湿度测量命令。 */
bool aht20_start_measurement(void);

/** @brief 等待 AHT20 完成当前测量，最长约 200 ms。 */
bool aht20_wait_for_measurement(void);

/**
 * @brief 读取 AHT20 测量结果。
 * @param[out] temperature_x10 摄氏温度乘 10，例如 23.4 ℃ 返回 234。
 * @param[out] humidity_x10 相对湿度百分比乘 10，例如 56.7% 返回 567。
 * @return I2C 读取成功时返回 true。
 */
bool aht20_read_measurement(int16_t *temperature_x10,
                            uint16_t *humidity_x10);

#endif /* CLOCK_F407_AHT20_H */
