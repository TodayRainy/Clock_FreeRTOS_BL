/**
 * @file w25q.c
 * @brief 板载双W25Q128的SPI1轮询驱动。
 *
 * 两颗芯片共享PA5/SCK、PA6/MISO、PA7/MOSI，并分别使用PE13和PE14片选。
 * 驱动只实现A/B固件存储所需的读、4 KiB擦除和页编程操作。
 */

#include "stm32f4xx.h"
#include "tim_delay.h"
#include "w25q.h"

#define W25Q_CS_PORT       GPIOE
#define W25Q_A_CS_PIN      GPIO_Pin_13
#define W25Q_B_CS_PIN      GPIO_Pin_14

#define CMD_READ_DATA      0x03u
#define CMD_PAGE_PROGRAM   0x02u
#define CMD_WRITE_ENABLE   0x06u
#define CMD_SECTOR_ERASE   0x20u
#define CMD_READ_SR1       0x05u
#define CMD_READ_SR2       0x35u
#define CMD_JEDEC_ID       0x9Fu
#define CMD_UNIQUE_ID      0x4Bu
#define CMD_RELEASE_POWER  0xABu
#define CMD_RESET_ENABLE   0x66u
#define CMD_RESET          0x99u

static uint16_t chip_select_pin(w25q_chip_t chip)
{
    return chip == W25Q_CHIP_A ? W25Q_A_CS_PIN : W25Q_B_CS_PIN;
}

static void select_chip(w25q_chip_t chip)
{
    GPIO_ResetBits(W25Q_CS_PORT, chip_select_pin(chip));
}

static void release_chip(w25q_chip_t chip)
{
    /* BSY 清零后再抬高片选，保证最后一位已经真正离开 SPI。 */
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
    GPIO_SetBits(W25Q_CS_PORT, chip_select_pin(chip));
}

static uint8_t spi_transfer(uint8_t value)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);
    SPI_I2S_SendData(SPI1, value);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

static void send_command(w25q_chip_t chip, uint8_t command)
{
    select_chip(chip);
    spi_transfer(command);
    release_chip(chip);
}

static uint8_t read_register(w25q_chip_t chip, uint8_t command)
{
    uint8_t value;

    select_chip(chip);
    spi_transfer(command);
    value = spi_transfer(0xFFu);
    release_chip(chip);
    return value;
}

/**
 * @brief 等待状态寄存器BUSY位清零。
 */
static bool wait_ready(w25q_chip_t chip, uint32_t timeout_ms)
{
    uint64_t start = tim_get_ms();

    while (w25q_read_status1(chip) & 0x01u)
    {
        if (tim_get_ms() - start >= timeout_ms)
            return false;
    }
    return true;
}

static void write_enable(w25q_chip_t chip)
{
    send_command(chip, CMD_WRITE_ENABLE);
}

void w25q_init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;
    uint32_t chip;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOE, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    /* 先把输出锁存器置高，再把 PE13/PE14 切成输出，避免产生误选脉冲。 */
    GPIO_SetBits(W25Q_CS_PORT, W25Q_A_CS_PIN | W25Q_B_CS_PIN);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = W25Q_A_CS_PIN | W25Q_B_CS_PIN;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(W25Q_CS_PORT, &gpio);

    GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_SPI1);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_SPI1);
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &gpio);

    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, DISABLE);
    SPI_StructInit(&spi);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_Low;
    spi.SPI_CPHA = SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_Init(SPI1, &spi);
    SPI_Cmd(SPI1, ENABLE);

    /* 复位串行协议状态不会改动阵列内容，两颗芯片逐个处理。 */
    for (chip = 0; chip < W25Q_CHIP_COUNT; ++chip)
    {
        send_command((w25q_chip_t)chip, CMD_RESET_ENABLE);
        send_command((w25q_chip_t)chip, CMD_RESET);
        tim_delay_us(30);
        send_command((w25q_chip_t)chip, CMD_RELEASE_POWER);
        tim_delay_us(5);
    }
}

uint32_t w25q_read_jedec_id(w25q_chip_t chip)
{
    uint32_t id;

    select_chip(chip);
    spi_transfer(CMD_JEDEC_ID);
    id = (uint32_t)spi_transfer(0xFFu) << 16;
    id |= (uint32_t)spi_transfer(0xFFu) << 8;
    id |= spi_transfer(0xFFu);
    release_chip(chip);
    return id;
}

uint8_t w25q_read_status1(w25q_chip_t chip)
{
    return read_register(chip, CMD_READ_SR1);
}

uint8_t w25q_read_status2(w25q_chip_t chip)
{
    return read_register(chip, CMD_READ_SR2);
}

void w25q_read_unique_id(w25q_chip_t chip, uint8_t uid[8])
{
    uint32_t i;

    select_chip(chip);
    spi_transfer(CMD_UNIQUE_ID);
    for (i = 0; i < 4; ++i)
        spi_transfer(0xFFu);
    for (i = 0; i < 8; ++i)
        uid[i] = spi_transfer(0xFFu);
    release_chip(chip);
}

void w25q_read(w25q_chip_t chip, uint32_t address, uint8_t *data, uint32_t size)
{
    select_chip(chip);
    spi_transfer(CMD_READ_DATA);
    spi_transfer((uint8_t)(address >> 16));
    spi_transfer((uint8_t)(address >> 8));
    spi_transfer((uint8_t)address);

    while (size--)
        *data++ = spi_transfer(0xFFu);

    release_chip(chip);
}

bool w25q_erase(w25q_chip_t chip, uint32_t address, uint32_t size)
{
    if ((address % W25Q_SECTOR_SIZE) != 0 || (size % W25Q_SECTOR_SIZE) != 0)
        return false;

    while (size)
    {
        write_enable(chip);
        select_chip(chip);
        spi_transfer(CMD_SECTOR_ERASE);
        spi_transfer((uint8_t)(address >> 16));
        spi_transfer((uint8_t)(address >> 8));
        spi_transfer((uint8_t)address);
        release_chip(chip);

        if (!wait_ready(chip, 1000))
            return false;

        address += W25Q_SECTOR_SIZE;
        size -= W25Q_SECTOR_SIZE;
    }
    return true;
}

bool w25q_program(w25q_chip_t chip, uint32_t address, const uint8_t *data, uint32_t size)
{
    uint32_t chunk;

    while (size)
    {
        /* 一次 Page Program 不能跨越256字节页边界。 */
        chunk = W25Q_PAGE_SIZE - (address % W25Q_PAGE_SIZE);
        if (chunk > size)
            chunk = size;

        write_enable(chip);
        select_chip(chip);
        spi_transfer(CMD_PAGE_PROGRAM);
        spi_transfer((uint8_t)(address >> 16));
        spi_transfer((uint8_t)(address >> 8));
        spi_transfer((uint8_t)address);
        for (uint32_t i = 0; i < chunk; ++i)
            spi_transfer(data[i]);
        release_chip(chip);

        if (!wait_ready(chip, 100))
            return false;

        address += chunk;
        data += chunk;
        size -= chunk;
    }
    return true;
}
