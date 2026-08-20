/**
 * @file bl_usart.c
 * @brief Bootloader协议使用的USART3轮询发送和循环DMA接收驱动。
 *
 * DMA只负责持续接收，协议层通过独立读游标消费字节。驱动不解释帧内容，也不
 * 决定高速波特率的超时恢复策略。
 */

#include "bl_usart.h"

#include "stm32f4xx.h"

/* Bootloader协议串口：USART3，PB10/TX，PB11/RX，8-N-1。 */
#define RX_DMA_STREAM       DMA1_Stream1
#define RX_DMA_CHANNEL      DMA_Channel_4
#define RX_DMA_BUFFER_SIZE  (16u * 1024u)

static uint8_t rx_dma_buffer[RX_DMA_BUFFER_SIZE];
static uint16_t rx_read_position;

static void usart_io_init(void)
{
    GPIO_InitTypeDef gpio;

    GPIO_StructInit(&gpio);
    gpio.GPIO_Pin = GPIO_Pin_10 | GPIO_Pin_11;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &gpio);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_USART3);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource11, GPIO_AF_USART3);
}

/**
 * @brief 配置USART3_RX使用的循环DMA。
 *
 * 16 KiB缓冲区可以容纳一个完整8 KiB请求和协议余量。上位机只允许两个
 * W25_PROGRAM请求在途，因此Bootloader写上一包时，下一包不会覆盖未读数据。
 */
static void usart_dma_init(void)
{
    DMA_InitTypeDef dma;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    DMA_Cmd(RX_DMA_STREAM, DISABLE);
    while (DMA_GetCmdStatus(RX_DMA_STREAM) != DISABLE);
    DMA_DeInit(RX_DMA_STREAM);

    DMA_StructInit(&dma);
    dma.DMA_Channel = RX_DMA_CHANNEL;
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DR;
    dma.DMA_Memory0BaseAddr = (uint32_t)rx_dma_buffer;
    dma.DMA_DIR = DMA_DIR_PeripheralToMemory;
    dma.DMA_BufferSize = RX_DMA_BUFFER_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_High;
    dma.DMA_FIFOMode = DMA_FIFOMode_Disable;
    DMA_Init(RX_DMA_STREAM, &dma);

    rx_read_position = 0;
    USART_DMACmd(USART3, USART_DMAReq_Rx, ENABLE);
    DMA_Cmd(RX_DMA_STREAM, ENABLE);
}

static void usart_lowlevel_init(uint32_t baud_rate)
{
    USART_InitTypeDef usart;

    USART_StructInit(&usart);
    usart.USART_BaudRate = baud_rate;
    usart.USART_WordLength = USART_WordLength_8b;
    usart.USART_StopBits = USART_StopBits_1;
    usart.USART_Parity = USART_Parity_No;
    usart.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    usart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART3, &usart);
    USART_DMACmd(USART3, USART_DMAReq_Rx, ENABLE);
    USART_Cmd(USART3, ENABLE);
}

void bl_usart_init(void)
{
    usart_io_init();
    usart_lowlevel_init(BL_USART_DEFAULT_BAUD_RATE);
    usart_dma_init();
}

void bl_usart_deinit(void)
{
    USART_DMACmd(USART3, USART_DMAReq_Rx, DISABLE);
    DMA_Cmd(RX_DMA_STREAM, DISABLE);
    while (DMA_GetCmdStatus(RX_DMA_STREAM) != DISABLE);
    DMA_DeInit(RX_DMA_STREAM);
    USART_Cmd(USART3, DISABLE);
    USART_DeInit(USART3);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, DISABLE);
}

void bl_usart_set_baud(uint32_t baud_rate)
{
    /* protocol_response()通常已经等待过TC；这里再确认一次，避免截断ACK。 */
    while (USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET);
    USART_Cmd(USART3, DISABLE);
    usart_lowlevel_init(baud_rate);
}

void bl_usart_write(const uint8_t *data, uint32_t size)
{
    while (size--)
    {
        while (USART_GetFlagStatus(USART3, USART_FLAG_TXE) == RESET);
        USART_SendData(USART3, *data++);
    }
    while (USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET);
}

static uint16_t rx_write_position(void)
{
    uint16_t position = (uint16_t)(RX_DMA_BUFFER_SIZE -
                                   DMA_GetCurrDataCounter(RX_DMA_STREAM));
    return position == RX_DMA_BUFFER_SIZE ? 0u : position;
}

bool bl_usart_read_byte(uint8_t *data)
{
    if (rx_read_position == rx_write_position())
        return false;

    *data = rx_dma_buffer[rx_read_position++];
    if (rx_read_position == RX_DMA_BUFFER_SIZE)
        rx_read_position = 0;
    return true;
}

bool bl_usart_data_pending(void)
{
    return rx_read_position != rx_write_position();
}
