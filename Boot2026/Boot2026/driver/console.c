#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "stm32f4xx_ll_gpio.h"
#include "stm32f4xx_ll_usart.h"
#include "stm32f4xx_ll_dma.h"
#include "console.h"

static SemaphoreHandle_t write_async_semaphore;
static console_received_func_t received_func;

static void console_io_init(void)
{
    LL_GPIO_InitTypeDef GPIO_InitStruct;
    LL_GPIO_StructInit(&GPIO_InitStruct);
    GPIO_InitStruct.Pin = LL_GPIO_PIN_9 | LL_GPIO_PIN_10;
    GPIO_InitStruct.Mode = LL_GPIO_MODE_ALTERNATE;
    GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
    GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = LL_GPIO_AF_7;
    LL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

static void console_usart_init(void)
{
    LL_USART_InitTypeDef USART_InitStruct;
    LL_USART_StructInit(&USART_InitStruct);
    USART_InitStruct.BaudRate = 115200;
    USART_InitStruct.DataWidth = LL_USART_DATAWIDTH_8B;
    USART_InitStruct.StopBits = LL_USART_STOPBITS_1;
    USART_InitStruct.Parity = LL_USART_PARITY_NONE;
    USART_InitStruct.TransferDirection = LL_USART_DIRECTION_TX_RX;
    USART_InitStruct.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    USART_InitStruct.OverSampling = LL_USART_OVERSAMPLING_16;
    LL_USART_Init(USART1, &USART_InitStruct);

    LL_USART_EnableIT_RXNE(USART1);
    LL_USART_EnableDMAReq_TX(USART1);
    LL_USART_Enable(USART1);
}

static void console_dma_init(void)
{
    LL_DMA_InitTypeDef DMA_InitStruct;
    LL_DMA_StructInit(&DMA_InitStruct);
    DMA_InitStruct.PeriphOrM2MSrcAddress = (uint32_t)&USART1->DR;
    DMA_InitStruct.Channel = LL_DMA_CHANNEL_4;
    DMA_InitStruct.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    DMA_InitStruct.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    DMA_InitStruct.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    DMA_InitStruct.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    DMA_InitStruct.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    DMA_InitStruct.Mode = LL_DMA_MODE_NORMAL;
    DMA_InitStruct.Priority = LL_DMA_PRIORITY_LOW;
    DMA_InitStruct.FIFOMode = LL_DMA_FIFOMODE_ENABLE;
    DMA_InitStruct.FIFOThreshold = LL_DMA_FIFOTHRESHOLD_FULL;
    DMA_InitStruct.MemBurst = LL_DMA_MBURST_INC8;
    DMA_InitStruct.PeriphBurst = LL_DMA_PBURST_SINGLE;
    DMA_InitStruct.MemoryOrM2MDstAddress = 0;
    DMA_InitStruct.NbData = 0;
    LL_DMA_Init(DMA2, LL_DMA_STREAM_7, &DMA_InitStruct);

    LL_DMA_EnableIT_TC(DMA2, LL_DMA_STREAM_7);
}

static void console_int_init(void)
{
    NVIC_SetPriority(USART1_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(USART1_IRQn);

    NVIC_SetPriority(DMA2_Stream7_IRQn, NVIC_EncodePriority(NVIC_GetPriorityGrouping(), 5, 0));
    NVIC_EnableIRQ(DMA2_Stream7_IRQn);
}

void console_init(void)
{
    write_async_semaphore = xSemaphoreCreateBinary();
    configASSERT(write_async_semaphore);

    console_io_init();
    console_usart_init();
    console_dma_init();
    console_int_init();
}

void console_deinit(void)
{
    LL_USART_Disable(USART1);
    LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_7);
    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(DMA2_Stream7_IRQn);
}

void console_write(const char str[], uint32_t length)
{
    if (xPortIsInsideInterrupt())
    {
        for (uint32_t i = 0; i < length; i++)
        {
            while (!LL_USART_IsActiveFlag_TXE(USART1));
            LL_USART_TransmitData8(USART1, (uint8_t)str[i]);
        }
    }
    else
    {
        uint32_t len = length;
        do
        {
            uint32_t chunk_size = len < 65535 ? len : 65535;

            LL_DMA_SetMemoryAddress(DMA2, LL_DMA_STREAM_7, (uint32_t)str);
            LL_DMA_SetDataLength(DMA2, LL_DMA_STREAM_7, chunk_size);
            LL_DMA_EnableStream(DMA2, LL_DMA_STREAM_7);
            xSemaphoreTake(write_async_semaphore, portMAX_DELAY);

            str += chunk_size;
            len -= chunk_size;
        } while (len > 0);
    }
    while (!LL_USART_IsActiveFlag_TC(USART1));
    LL_USART_ClearFlag_TC(USART1);
}

void console_received_register(console_received_func_t func)
{
    received_func = func;
}

void USART1_IRQHandler(void)
{
    if (LL_USART_IsEnabledIT_RXNE(USART1) && LL_USART_IsActiveFlag_RXNE(USART1))
    {
        if (received_func != NULL)
        {
            uint8_t data = (uint8_t)(USART1->DR & 0xFFU);
            received_func(data);
        }
    }
}

void DMA2_Stream7_IRQHandler(void)
{
    if (LL_DMA_IsActiveFlag_TC7(DMA2))
    {
        BaseType_t pxHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(write_async_semaphore, &pxHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(pxHigherPriorityTaskWoken);

        LL_DMA_ClearFlag_TC7(DMA2);
        LL_DMA_DisableStream(DMA2, LL_DMA_STREAM_7);
    }
}
