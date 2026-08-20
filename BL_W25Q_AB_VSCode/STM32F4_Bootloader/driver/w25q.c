/**
 * @file w25q.c
 * @brief 板载双W25Q128的SPI1驱动。
 *
 * 两颗芯片共享PA5/SCK、PA6/MISO、PA7/MOSI，并分别使用PE13和PE14片选。
 * 固件批量读取由DMA2自动收发；短命令、擦除和页编程仍使用直接轮询，避免
 * 为很小的数据量增加额外状态。驱动只实现A/B固件存储所需的操作。
 */

#include "stm32f4xx.h"
#include "watchdog.h"
#include "tim_delay.h"
#include "w25q.h"

#define W25Q_CS_PORT       GPIOE
#define W25Q_A_CS_PIN      GPIO_Pin_13
#define W25Q_B_CS_PIN      GPIO_Pin_14
#define TF_CS_PORT         GPIOA
#define TF_CS_PIN          GPIO_Pin_15

#define W25Q_RX_DMA_STREAM DMA2_Stream0
#define W25Q_TX_DMA_STREAM DMA2_Stream3
#define W25Q_DMA_CHANNEL   DMA_Channel_3
#define W25Q_DMA_MAX_COUNT 0xFFFFu

#define W25Q_RX_DMA_FLAGS  (DMA_FLAG_FEIF0 | DMA_FLAG_DMEIF0 | \
                            DMA_FLAG_TEIF0 | DMA_FLAG_HTIF0 | DMA_FLAG_TCIF0)
#define W25Q_TX_DMA_FLAGS  (DMA_FLAG_FEIF3 | DMA_FLAG_DMEIF3 | \
                            DMA_FLAG_TEIF3 | DMA_FLAG_HTIF3 | DMA_FLAG_TCIF3)

#define CMD_READ_DATA      0x03u
#define CMD_PAGE_PROGRAM   0x02u
#define CMD_WRITE_ENABLE   0x06u
#define CMD_SECTOR_ERASE   0x20u
#define CMD_BLOCK_ERASE_32K 0x52u
#define CMD_BLOCK_ERASE_64K 0xD8u
#define CMD_READ_SR1       0x05u
#define CMD_READ_SR2       0x35u
#define CMD_JEDEC_ID       0x9Fu
#define CMD_UNIQUE_ID      0x4Bu
#define CMD_RELEASE_POWER  0xABu
#define CMD_RESET_ENABLE   0x66u
#define CMD_RESET          0x99u

/* TX DMA不递增此地址，持续写出0xFF为W25Q读取操作产生时钟。 */
static uint8_t spi_dummy_byte = 0xFFu;

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

/**
 * @brief 使用SPI1双向DMA接收一段连续数据。
 *
 * SPI主机必须发送字节才能产生时钟，因此RX DMA把SPI1->DR搬到目标缓冲区的
 * 同时，TX DMA从同一个0xFF字节反复取数并写入SPI1->DR。调用期间保持片选
 * 为低，超过DMA计数器上限的数据会被拆成多个连续DMA传输，不重新发送地址。
 *
 * @param[out] data 接收缓冲区。
 * @param[in] size 需要接收的字节数。
 */
static void spi_receive_dma(uint8_t *data, uint32_t size)
{
    DMA_InitTypeDef dma;
    uint32_t chunk;

    while (size)
    {
        chunk = size > W25Q_DMA_MAX_COUNT ? W25Q_DMA_MAX_COUNT : size;

        DMA_Cmd(W25Q_RX_DMA_STREAM, DISABLE);
        DMA_Cmd(W25Q_TX_DMA_STREAM, DISABLE);
        while (W25Q_RX_DMA_STREAM->CR & DMA_SxCR_EN);
        while (W25Q_TX_DMA_STREAM->CR & DMA_SxCR_EN);
        DMA_ClearFlag(W25Q_RX_DMA_STREAM, W25Q_RX_DMA_FLAGS);
        DMA_ClearFlag(W25Q_TX_DMA_STREAM, W25Q_TX_DMA_FLAGS);

        DMA_StructInit(&dma);
        dma.DMA_Channel = W25Q_DMA_CHANNEL;
        dma.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DR;
        dma.DMA_Memory0BaseAddr = (uint32_t)data;
        dma.DMA_DIR = DMA_DIR_PeripheralToMemory;
        dma.DMA_BufferSize = chunk;
        dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        dma.DMA_Mode = DMA_Mode_Normal;
        dma.DMA_Priority = DMA_Priority_High;
        dma.DMA_FIFOMode = DMA_FIFOMode_Disable;
        DMA_Init(W25Q_RX_DMA_STREAM, &dma);

        dma.DMA_Memory0BaseAddr = (uint32_t)&spi_dummy_byte;
        dma.DMA_DIR = DMA_DIR_MemoryToPeripheral;
        dma.DMA_MemoryInc = DMA_MemoryInc_Disable;
        dma.DMA_Priority = DMA_Priority_Medium;
        DMA_Init(W25Q_TX_DMA_STREAM, &dma);

        /* 两条流先就绪再开放SPI请求，避免第一个返回字节无人接收。 */
        DMA_Cmd(W25Q_RX_DMA_STREAM, ENABLE);
        DMA_Cmd(W25Q_TX_DMA_STREAM, ENABLE);
        SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, ENABLE);

        while (DMA_GetFlagStatus(W25Q_RX_DMA_STREAM, DMA_FLAG_TCIF0) == RESET);
        while (DMA_GetFlagStatus(W25Q_TX_DMA_STREAM, DMA_FLAG_TCIF3) == RESET);

        SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, DISABLE);
        DMA_Cmd(W25Q_RX_DMA_STREAM, DISABLE);
        DMA_Cmd(W25Q_TX_DMA_STREAM, DISABLE);

        data += chunk;
        size -= chunk;
    }
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
    uint64_t start = tim_now();
    uint64_t timeout_us = (uint64_t)timeout_ms * 1000u;

    while (w25q_read_status1(chip) & 0x01u)
    {
        /* 64 KiB块擦除可能持续数秒，不能让正常的Flash忙等待触发IWDG。 */
        watchdog_refresh();
        if (tim_now() - start >= timeout_us)
            return false;
    }
    return true;
}

static void write_enable(w25q_chip_t chip)
{
    send_command(chip, CMD_WRITE_ENABLE);
}

/**
 * @brief 发送一次带24位地址的擦除命令并等待芯片完成。
 */
static bool erase_unit(w25q_chip_t chip, uint32_t address, uint8_t command,
                       uint32_t timeout_ms)
{
    write_enable(chip);
    select_chip(chip);
    spi_transfer(command);
    spi_transfer((uint8_t)(address >> 16));
    spi_transfer((uint8_t)(address >> 8));
    spi_transfer((uint8_t)address);
    release_chip(chip);
    return wait_ready(chip, timeout_ms);
}

void w25q_init(void)
{
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;
    uint32_t chip;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOE |
                           RCC_AHB1Periph_DMA2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);

    /*
     * 三个SPI1从设备先全部取消片选。TF卡与两颗W25Q共享PA5/PA6/PA7，
     * OTA访问外部Flash时必须让PA15保持高电平，避免卡座同时驱动MISO。
     */
    GPIO_SetBits(TF_CS_PORT, TF_CS_PIN);
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = TF_CS_PIN;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(TF_CS_PORT, &gpio);

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
    /* APB2为84 MHz，分频4得到21 MHz，仍远低于W25Q128普通读写上限。 */
    spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
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

void w25q_deinit(void)
{
    GPIO_InitTypeDef gpio;

    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Rx | SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(W25Q_RX_DMA_STREAM, DISABLE);
    DMA_Cmd(W25Q_TX_DMA_STREAM, DISABLE);
    while (W25Q_RX_DMA_STREAM->CR & DMA_SxCR_EN);
    while (W25Q_TX_DMA_STREAM->CR & DMA_SxCR_EN);
    DMA_ClearFlag(W25Q_RX_DMA_STREAM, W25Q_RX_DMA_FLAGS);
    DMA_ClearFlag(W25Q_TX_DMA_STREAM, W25Q_TX_DMA_FLAGS);
    DMA_DeInit(W25Q_RX_DMA_STREAM);
    DMA_DeInit(W25Q_TX_DMA_STREAM);

    /* 确认最后一位已经发送完成，再同时释放两颗Flash。 */
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
    GPIO_SetBits(W25Q_CS_PORT, W25Q_A_CS_PIN | W25Q_B_CS_PIN);

    SPI_Cmd(SPI1, DISABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, ENABLE);
    RCC_APB2PeriphResetCmd(RCC_APB2Periph_SPI1, DISABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, DISABLE);

    /*
     * DMA2还由USART1控制台等外设共享。这里只释放W25Q使用的Stream0/3，
     * 不能复位或关闭整个DMA2，否则OTA结束后会破坏其他DMA通道。
     */

    /* 模拟模式关闭数字输入缓冲，应用以后仍可按自己的用途重新初始化这些脚。 */
    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AN;
    gpio.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(GPIOA, &gpio);
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

    spi_receive_dma(data, size);

    release_chip(chip);
}

bool w25q_erase(w25q_chip_t chip, uint32_t address, uint32_t size)
{
    uint32_t erase_size;
    uint32_t timeout_ms;
    uint8_t command;

    if ((address % W25Q_SECTOR_SIZE) != 0 || (size % W25Q_SECTOR_SIZE) != 0)
        return false;

    while (size)
    {
        /*
         * 优先使用与当前位置对齐的最大块，不能整块覆盖的尾部退回到4 KiB。
         * 这样不会擦到调用方请求区域之外，也兼容任意扇区对齐的范围。
         */
        if ((address % W25Q_BLOCK64_SIZE) == 0 && size >= W25Q_BLOCK64_SIZE)
        {
            command = CMD_BLOCK_ERASE_64K;
            erase_size = W25Q_BLOCK64_SIZE;
            timeout_ms = 3000u;
        }
        else if ((address % W25Q_BLOCK32_SIZE) == 0 && size >= W25Q_BLOCK32_SIZE)
        {
            command = CMD_BLOCK_ERASE_32K;
            erase_size = W25Q_BLOCK32_SIZE;
            timeout_ms = 2000u;
        }
        else
        {
            command = CMD_SECTOR_ERASE;
            erase_size = W25Q_SECTOR_SIZE;
            timeout_ms = 1000u;
        }

        if (!erase_unit(chip, address, command, timeout_ms))
            return false;

        address += erase_size;
        size -= erase_size;
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
