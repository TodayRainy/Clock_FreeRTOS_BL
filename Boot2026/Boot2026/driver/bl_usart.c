#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_dma.h"
#include "stm32f4xx_ll_usart.h"
#include "bl_usart.h"

// USE USART3
// RX: PB11
// TX: PB10
// MODE: 8-N-1
// BAUD: 115200
// DMA: TX/RX

static uint32_t baudrate = 2 * 1000 * 1000;
static bl_usart_rx_callback_t rx_callback;
static bl_usart_async_rx_callback_t async_rx_callback;
static SemaphoreHandle_t dma_tx_semphr;


static void usart_io_init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStructure;
    LL_GPIO_StructInit(&GPIO_InitStructure);

    GPIO_InitStructure.Pin = LL_GPIO_PIN_10 | LL_GPIO_PIN_11;
    GPIO_InitStructure.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStructure.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStructure.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStructure.Pull = LL_GPIO_PULL_UP;
    GPIO_InitStructure.Alternate  = LL_GPIO_AF_7;
    LL_GPIO_Init(GPIOB, &GPIO_InitStructure);
}

static void usart_dma_config(void)
{
    LL_DMA_InitTypeDef DMA_InitStruct;
    LL_DMA_StructInit(&DMA_InitStruct);

    // RX: DMA1 Stream1 Channel4
    DMA_InitStruct.Channel = LL_DMA_CHANNEL_4;
    DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)&USART3->DR;
    DMA_InitStruct.MemoryOrM2MDstAddress = 0; // set later
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_PERIPH_TO_MEMORY;
    DMA_InitStruct.NbData = 0; // set later
    DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    DMA_InitStruct.Mode = LL_DMA_MODE_NORMAL;
    DMA_InitStruct.Priority = LL_DMA_PRIORITY_MEDIUM;
    DMA_InitStruct.FIFOMode = LL_DMA_FIFOMODE_DISABLE;
    LL_DMA_Init(DMA1, LL_DMA_STREAM_1, &DMA_InitStruct);
    LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_1);
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_1);

    // TX: DMA1 Stream3 Channel4
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    LL_DMA_Init(DMA1, LL_DMA_STREAM_3, &DMA_InitStruct);
    LL_DMA_EnableIT_TC(DMA1, LL_DMA_STREAM_3);
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_3);
}

static void usart_it_config(void)
{
    NVIC_SetPriority(USART3_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(USART3_IRQn);

    NVIC_SetPriority(DMA1_Stream3_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(DMA1_Stream3_IRQn);

    NVIC_SetPriority(DMA1_Stream1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

static void usart_lowlevel_init(void)
{
    LL_USART_InitTypeDef USART_InitStructure = {0};

    USART_InitStructure.BaudRate = 2 * 1000 * 1000;
    USART_InitStructure.DataWidth = LL_USART_DATAWIDTH_8B;
    USART_InitStructure.StopBits = LL_USART_STOPBITS_1;
    USART_InitStructure.Parity = LL_USART_PARITY_NONE;
    USART_InitStructure.TransferDirection = LL_USART_DIRECTION_TX_RX;
    USART_InitStructure.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    USART_InitStructure.OverSampling = LL_USART_OVERSAMPLING_16;
    LL_USART_Init(USART3, &USART_InitStructure);
    LL_USART_EnableIT_RXNE(USART3);
    LL_USART_EnableDMAReq_RX(USART3);
    LL_USART_EnableDMAReq_TX(USART3);
    LL_USART_Enable(USART3);
}

void bl_usart_init(void)
{
    dma_tx_semphr = xSemaphoreCreateBinary();
    configASSERT(dma_tx_semphr);

    usart_lowlevel_init();
    usart_dma_config();
    usart_it_config();
    usart_io_init();
}

void bl_usart_write(const uint8_t *data, uint32_t size)
{
    while (size > 0)
    {
        uint32_t chunk = size > 65535 ? 65535 : size;
        LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_3, (uint32_t)data);
        LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_3, chunk);
        LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_3);
        xSemaphoreTake(dma_tx_semphr, pdMS_TO_TICKS(chunk * 1000 * 3 / (baudrate / 10)));
        data += chunk;
        size -= chunk;
    }
    while (!LL_USART_IsActiveFlag_TC(USART3));
    LL_USART_ClearFlag_TC(USART3);
}

void bl_usart_receive_async(uint8_t *buffer, uint32_t size)
{
    LL_USART_DisableIT_RXNE(USART3);
    LL_DMA_SetMemoryAddress(DMA1, LL_DMA_STREAM_1, (uint32_t)buffer);
    LL_DMA_SetDataLength(DMA1, LL_DMA_STREAM_1, size);
    LL_DMA_EnableStream(DMA1, LL_DMA_STREAM_1);
}

void bl_usart_stop_receive(void)
{
    LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_1);
    LL_USART_EnableIT_RXNE(USART3);
}

void bl_usart_register_rx_callback(bl_usart_rx_callback_t callback)
{
    rx_callback = callback;
}

void bl_usart_register_async_rx_callback(bl_usart_async_rx_callback_t callback)
{
    async_rx_callback = callback;
}

void USART3_IRQHandler(void)
{
    if (LL_USART_IsActiveFlag_RXNE(USART3))
    {
        uint8_t data = LL_USART_ReceiveData8(USART3);
        if (rx_callback)
            rx_callback(&data, 1);
    }
}

// DMA1 Stream3 for USART3 TX
void DMA1_Stream3_IRQHandler(void)
{
    if (LL_DMA_IsActiveFlag_TC3(DMA1))
    {
        LL_DMA_ClearFlag_TC3(DMA1);
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(dma_tx_semphr, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
    }
}

// DMA1 Stream1 for USART3 RX
void DMA1_Stream1_IRQHandler(void)
{
    if (LL_DMA_IsActiveFlag_TC1(DMA1))
    {
        LL_DMA_ClearFlag_TC1(DMA1);
        LL_DMA_DisableStream(DMA1, LL_DMA_STREAM_1);
        LL_USART_EnableIT_RXNE(USART3);
        if (async_rx_callback)
            async_rx_callback();
    }
}
