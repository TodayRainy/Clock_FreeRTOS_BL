/**
 * @file aht20.c
 * @brief AHT20温湿度传感器的I2C2轮询驱动。
 *
 * 本模块只在应用任务上下文中调用。I2C事件等待使用微秒级超时，失败路径统一
 * 产生STOP释放总线；测量转换等待则用FreeRTOS延时，不占用CPU空转。
 */

#include "aht20.h"

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f4xx.h"
#include "tim_delay.h"

#define AHT20_I2C_ADDRESS       0x70u
#define AHT20_I2C_TIMEOUT_US    1000u
#define AHT20_BUSY_MASK         0x80u
#define AHT20_CALIBRATED_MASK   0x08u

static bool aht20_wait_event(uint32_t event)
{
    uint32_t timeout_us = AHT20_I2C_TIMEOUT_US;

    while (!I2C_CheckEvent(I2C2, event) && timeout_us > 0u)
    {
        tim_delay_us(10u);
        timeout_us -= 10u;
    }
    return timeout_us > 0u;
}

static bool aht20_write(const uint8_t *data, uint32_t length)
{
    uint32_t index;
    bool success = false;

    I2C_AcknowledgeConfig(I2C2, ENABLE);
    I2C_GenerateSTART(I2C2, ENABLE);
    if (!aht20_wait_event(I2C_EVENT_MASTER_MODE_SELECT))
        goto out;

    I2C_Send7bitAddress(I2C2, AHT20_I2C_ADDRESS,
                        I2C_Direction_Transmitter);
    if (!aht20_wait_event(I2C_EVENT_MASTER_TRANSMITTER_MODE_SELECTED))
        goto out;

    for (index = 0u; index < length; ++index)
    {
        I2C_SendData(I2C2, data[index]);
        if (!aht20_wait_event(I2C_EVENT_MASTER_BYTE_TRANSMITTING))
            goto out;
    }
    success = true;

out:
    I2C_GenerateSTOP(I2C2, ENABLE);
    return success;
}

static bool aht20_read(uint8_t *data, uint32_t length)
{
    uint32_t index;
    bool success = false;

    I2C_AcknowledgeConfig(I2C2, ENABLE);
    I2C_GenerateSTART(I2C2, ENABLE);
    if (!aht20_wait_event(I2C_EVENT_MASTER_MODE_SELECT))
        goto out;

    I2C_Send7bitAddress(I2C2, AHT20_I2C_ADDRESS,
                        I2C_Direction_Receiver);
    if (!aht20_wait_event(I2C_EVENT_MASTER_RECEIVER_MODE_SELECTED))
        goto out;

    for (index = 0u; index < length; ++index)
    {
        if (index + 1u == length)
            I2C_AcknowledgeConfig(I2C2, DISABLE);
        if (!aht20_wait_event(I2C_EVENT_MASTER_BYTE_RECEIVED))
            goto out;
        data[index] = I2C_ReceiveData(I2C2);
    }
    success = true;

out:
    I2C_GenerateSTOP(I2C2, ENABLE);
    return success;
}

static bool aht20_read_status(uint8_t *status)
{
    const uint8_t command = 0x71u;

    return aht20_write(&command, sizeof(command)) &&
           aht20_read(status, sizeof(*status));
}

static bool aht20_is_ready(void)
{
    uint8_t status;

    return aht20_read_status(&status) &&
           (status & AHT20_CALIBRATED_MASK) != 0u;
}

bool aht20_init(void)
{
    static const uint8_t calibrate_command[] = {0xBEu, 0x08u, 0x00u};
    I2C_InitTypeDef i2c;
    GPIO_InitTypeDef gpio;
    uint32_t attempt;

    I2C_StructInit(&i2c);
    i2c.I2C_Ack = I2C_Ack_Enable;
    i2c.I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit;
    i2c.I2C_ClockSpeed = 100u * 1000u;
    i2c.I2C_DutyCycle = I2C_DutyCycle_2;
    i2c.I2C_Mode = I2C_Mode_I2C;
    i2c.I2C_OwnAddress1 = 0x00u;
    I2C_Init(I2C2, &i2c);

    GPIO_StructInit(&gpio);
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_OD;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio.GPIO_Speed = GPIO_High_Speed;
    gpio.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11;
    GPIO_Init(GPIOB, &gpio);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_I2C2);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource11, GPIO_AF_I2C2);

    vTaskDelay(pdMS_TO_TICKS(40u));
    if (aht20_is_ready())
        return true;

    if (!aht20_write(calibrate_command, sizeof(calibrate_command)))
        return false;

    for (attempt = 0u; attempt < 20u; ++attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(5u));
        if (aht20_is_ready())
            return true;
    }
    return false;
}

bool aht20_start_measurement(void)
{
    static const uint8_t measurement_command[] = {0xACu, 0x33u, 0x00u};

    return aht20_write(measurement_command, sizeof(measurement_command));
}

bool aht20_wait_for_measurement(void)
{
    uint8_t status;
    uint32_t attempt;

    for (attempt = 0u; attempt < 20u; ++attempt)
    {
        vTaskDelay(pdMS_TO_TICKS(10u));
        if (!aht20_read_status(&status))
            return false;
        if ((status & AHT20_BUSY_MASK) == 0u)
            return true;
    }
    return false;
}

bool aht20_read_measurement(int16_t *temperature_x10,
                            uint16_t *humidity_x10)
{
    uint8_t data[6];
    uint32_t raw_humidity;
    uint32_t raw_temperature;

    if (!aht20_read(data, sizeof(data)))
        return false;

    raw_humidity = ((uint32_t)data[1] << 12) |
                   ((uint32_t)data[2] << 4) |
                   ((uint32_t)(data[3] & 0xF0u) >> 4);
    raw_temperature = ((uint32_t)(data[3] & 0x0Fu) << 16) |
                      ((uint32_t)data[4] << 8) |
                      (uint32_t)data[5];

    /*
     * AHT20原始值均为20位：
     *   RH = raw * 100 / 2^20
     *   T  = raw * 200 / 2^20 - 50
     * 这里直接计算放大10倍的结果并四舍五入。最大中间值仍小于
     * UINT32_MAX，不需要浮点运算或64位除法。
     */
    *humidity_x10 = (uint16_t)((raw_humidity * 1000u + 0x80000u) >> 20);
    *temperature_x10 = (int16_t)(
        (int32_t)((raw_temperature * 2000u + 0x80000u) >> 20) - 500);
    return true;
}
