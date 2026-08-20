/**
 * @file bl24c512.c
 * @brief BL24C512软件I2C驱动，用于掉电安全的A/B状态记录。
 */

#include "bl24c512.h"

#include "stm32f4xx.h"
#include "tim_delay.h"

/*
 * 原理图中 A0/A1/A2 和 WP 均接地，因此7位地址为0x50且允许写入。
 * 这里使用简单的软件 I2C。Bootloader只偶尔读写几十字节状态，不需要
 * 硬件I2C中断或DMA，软件时序反而更容易在跳转应用前恢复为普通GPIO。
 */
#define EEPROM_PORT       GPIOB
#define EEPROM_SCL_PIN    GPIO_Pin_6
#define EEPROM_SDA_PIN    GPIO_Pin_7
#define EEPROM_ADDR_WRITE 0xA0u
#define EEPROM_ADDR_READ  0xA1u

static void bus_delay(void)
{
    tim_delay_us(3);
}

static void scl_high(void)
{
    GPIO_SetBits(EEPROM_PORT, EEPROM_SCL_PIN);
    bus_delay();
}

static void scl_low(void)
{
    GPIO_ResetBits(EEPROM_PORT, EEPROM_SCL_PIN);
    bus_delay();
}

static void sda_high(void)
{
    GPIO_SetBits(EEPROM_PORT, EEPROM_SDA_PIN);
    bus_delay();
}

static void sda_low(void)
{
    GPIO_ResetBits(EEPROM_PORT, EEPROM_SDA_PIN);
    bus_delay();
}

static void i2c_start(void)
{
    sda_high();
    scl_high();
    sda_low();
    scl_low();
}

static void i2c_stop(void)
{
    sda_low();
    scl_high();
    sda_high();
}

static bool i2c_write_byte(uint8_t value)
{
    uint32_t i;
    bool ack;

    for (i = 0; i < 8; ++i)
    {
        if (value & 0x80u)
            sda_high();
        else
            sda_low();
        scl_high();
        scl_low();
        value <<= 1;
    }

    sda_high();                   /* 释放 SDA，让从机输出 ACK。 */
    scl_high();
    ack = GPIO_ReadInputDataBit(EEPROM_PORT, EEPROM_SDA_PIN) == Bit_RESET;
    scl_low();
    return ack;
}

static uint8_t i2c_read_byte(bool ack)
{
    uint8_t value = 0;
    uint32_t i;

    sda_high();
    for (i = 0; i < 8; ++i)
    {
        value <<= 1;
        scl_high();
        if (GPIO_ReadInputDataBit(EEPROM_PORT, EEPROM_SDA_PIN) == Bit_SET)
            value |= 1u;
        scl_low();
    }

    if (ack)
        sda_low();
    else
        sda_high();
    scl_high();
    scl_low();
    sda_high();
    return value;
}

static bool begin_write(uint16_t address)
{
    i2c_start();
    if (!i2c_write_byte(EEPROM_ADDR_WRITE) ||
        !i2c_write_byte((uint8_t)(address >> 8)) ||
        !i2c_write_byte((uint8_t)address))
    {
        i2c_stop();
        return false;
    }
    return true;
}

static bool wait_write_complete(void)
{
    uint64_t start = tim_now();

    do
    {
        bool ready;
        i2c_start();
        ready = i2c_write_byte(EEPROM_ADDR_WRITE);
        i2c_stop();
        if (ready)
            return true;
    } while (tim_now() - start < 10000u);

    return false;
}

void bl24c512_init(void)
{
    GPIO_InitTypeDef gpio;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    GPIO_SetBits(EEPROM_PORT, EEPROM_SCL_PIN | EEPROM_SDA_PIN);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = EEPROM_SCL_PIN | EEPROM_SDA_PIN;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_OD;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_25MHz;
    GPIO_Init(EEPROM_PORT, &gpio);
}

bool bl24c512_is_present(void)
{
    bool ack;

    i2c_start();
    ack = i2c_write_byte(EEPROM_ADDR_WRITE);
    i2c_stop();
    return ack;
}

bool bl24c512_read(uint16_t address, uint8_t *data, uint32_t size)
{
    uint32_t i;

    if (size == 0)
        return true;
    if (!begin_write(address))
        return false;

    i2c_start();
    if (!i2c_write_byte(EEPROM_ADDR_READ))
    {
        i2c_stop();
        return false;
    }

    for (i = 0; i < size; ++i)
        data[i] = i2c_read_byte(i + 1 < size);
    i2c_stop();
    return true;
}

bool bl24c512_write(uint16_t address, const uint8_t *data, uint32_t size)
{
    uint32_t chunk;
    uint32_t i;

    while (size)
    {
        chunk = BL24C512_PAGE_SIZE - (address % BL24C512_PAGE_SIZE);
        if (chunk > size)
            chunk = size;

        if (!begin_write(address))
            return false;
        for (i = 0; i < chunk; ++i)
        {
            if (!i2c_write_byte(data[i]))
            {
                i2c_stop();
                return false;
            }
        }
        i2c_stop();
        if (!wait_write_complete())
            return false;

        address += chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}
