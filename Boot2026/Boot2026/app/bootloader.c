#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "stm32f4xx_ll_tim.h"
#include "stm32f4xx_ll_usart.h"
#include "board.h"
#include "bl_usart.h"
#include "tim_delay.h"
#include "stm32_flash.h"
#include "magic_header.h"
#include "crc16.h"
#include "crc32.h"
#include "ringbuffer.h"
#include "macros.h"
#include "bitops.h"

#define LOG_TAG    "boot"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

#define BL_VERSION      "v0.9.0"
#define BL_ADDRESS      0x08000000
#define BL_SIZE         (48 * 1024)
#define BOOT_DELAY      3000
#define APP_VTOR_ADDR   0x08010000
#define RX_BUFFER_SIZE  (5 * 1024)
#define RX_TIMEOUT_MS   20
#define PAYLOAD_SIZE_MAX (8192 + 8) // 8192为Program最大数据长度，8为Program指令的地址(4)和长度(4)
#define PACKET_QUEUE_MAX 4
#define PACKET_SIZE_MAX (4 + PAYLOAD_SIZE_MAX + 2) // header(1) + opcode(1) + length(2) + payload + crc16(2)

// 协议常数
#define PACKET_HEADER_REQUEST 0xAA
#define PACKET_HEADER_RESPONSE 0x55

// 数据包结构常数
#define PACKET_HEADER_SIZE 1
#define PACKET_OPCODE_SIZE 1
#define PACKET_LENGTH_SIZE 2
#define PACKET_CRC_SIZE 2
#define PACKET_HEADER_OFFSET 0
#define PACKET_OPCODE_OFFSET 1
#define PACKET_LENGTH_OFFSET 2
#define PACKET_PAYLOAD_OFFSET 4
#define PACKET_MIN_SIZE (PACKET_HEADER_SIZE + PACKET_OPCODE_SIZE + PACKET_LENGTH_SIZE + PACKET_CRC_SIZE)

#define ADDR_SIZE_PARAM_LENGTH 8  // uint addr + uint size
#define ADDR_SIZE_CRC_PARAM_LENGTH 12  // uint addr + uint size + uint crc


typedef enum
{
    PACKET_STATE_HEADER,
    PACKET_STATE_OPCODE,
    PACKET_STATE_LENGTH,
    PACKET_STATE_PAYLOAD,
    PACKET_STATE_CRC16,
    PACKET_STATE_FINISH,
} packet_state_machine_t;

typedef enum
{
    PACKET_OPCODE_INQUERY = 0x01,
    PACKET_OPCODE_REQUEST = 0x02,
    PACKET_OPCODE_ERASE = 0x81,
    PACKET_OPCODE_PROGRAM = 0x82,
    PACKET_OPCODE_VERIFY  = 0x83,
    PACKET_OPCODE_RESET = 0x21,
    PACKET_OPCODE_BOOT = 0x22,
} packet_opcode_t;

typedef enum
{
    INQUERY_SUBCODE_VERSION = 0x00,
    INQUERY_SUBCODE_MTU = 0x01,
} inquery_subcode_t;

typedef enum
{
    REQUEST_SUBCODE_FWINFO = 0x10,
} request_subcode_t;

typedef enum
{
    PACKET_ERRCODE_OK = 0,
    PACKET_ERRCODE_OPCODE,
    PACKET_ERRCODE_OVERFLOW,
    PACKET_ERRCODE_TIMEOUT,
    PACKET_ERRCODE_FORMAT,
    PACKET_ERRCODE_VERIFY,
    PACKET_ERRCODE_PARAM,
    PACKET_ERRCODE_UNKNOWN = 0xff,
} packet_errcode_t;

typedef struct
{
    union
    {
        struct
        {
            uint8_t header;
            packet_opcode_t opcode;
            uint16_t length;
            uint8_t payload[PAYLOAD_SIZE_MAX];
            uint16_t crc;
        };
        uint8_t raw[PACKET_SIZE_MAX];
    };
    uint8_t _temp[4];
    uint16_t _recv;
    packet_state_machine_t _state;
} packet_desc_t;

static QueueHandle_t empty_queue, parser_queue, program_queue;
static packet_desc_t *current_packet;
static uint32_t firmware_size=298308ul, firmware_received, firmware_programed;

static bool application_validate(void)
{
    if (!magic_header_validate())
    {
        log_e("magic header invalid");
        return false;
    }

    uint32_t addr = magic_header_get_address();
    uint32_t size = magic_header_get_length();
    uint32_t crc = magic_header_get_crc32();
    uint32_t ccrc = crc32((uint8_t *)addr, size);
    if (crc != ccrc)
    {
        log_e("application crc error: expected %08X, got %08X", crc, ccrc);
        return false;
    }

    return true;
}

static void boot_application(void)
{
    if (!application_validate())
    {
        log_e("application validate failed, cannot boot");
        return;
    }

    log_w("booting application...");
    tim_delay_ms(2);

    led_off(led1);
    LL_TIM_DisableIT_UPDATE(TIM6);
    LL_TIM_DisableCounter(TIM6);
    LL_USART_Disable(USART1);
    LL_USART_Disable(USART3);
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    NVIC_DisableIRQ(USART1_IRQn);
    NVIC_DisableIRQ(USART3_IRQn);

    SCB->VTOR = APP_VTOR_ADDR;
    extern void JumpApp(uint32_t base);
    JumpApp(0x08010000);
}

static void bl_response(packet_opcode_t opcode, packet_errcode_t errcode, const uint8_t *data, uint16_t length)
{
    uint8_t response[128], *prsp = response;
    put_u8_inc(&prsp, PACKET_HEADER_RESPONSE);
    put_u8_inc(&prsp, (uint8_t)opcode);
    put_u8_inc(&prsp, (uint8_t)errcode);
    put_u16_inc(&prsp, length);
    put_bytes_inc(&prsp, data, length);
    uint16_t crc = crc16(response, prsp - response);
    put_u16_inc(&prsp, crc);

    bl_usart_write(response, prsp - response);
}

//static inline void bl_response_ack(packet_opcode_t opcode, packet_errcode_t errcode)
//{
//    bl_response(opcode, errcode, NULL, 0);
//}

static void bl_opcode_inquery_handler(packet_desc_t *packet)
{
    log_i("inquery handler");

    if (packet->length != 1)
    {
        log_e("inquery packet length error");
        return;
    }

    uint8_t subcode = get_u8(packet->payload);

    switch (subcode)
    {
        case INQUERY_SUBCODE_VERSION:
        {
            bl_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_OK, (const uint8_t *)BL_VERSION, strlen(BL_VERSION));
            break;
        }
        case INQUERY_SUBCODE_MTU:
        {
            uint8_t bmtu[2];
            put_u16(bmtu, PAYLOAD_SIZE_MAX);
            bl_response(PACKET_OPCODE_INQUERY, PACKET_ERRCODE_OK, (const uint8_t *)&bmtu, sizeof(bmtu));
            break;
        }
        default:
        {
            log_w("unknown inquery subcode: %02X", subcode);
            break;
        }
    }
}

static void bl_opcode_request_handler(packet_desc_t *packet)
{
    log_i("request handler");

    if (packet->length < 1)
    {
        log_e("request packet length error");
        return;
    }

    uint8_t subcode = get_u8(packet->payload);

    switch (subcode)
    {
        case REQUEST_SUBCODE_FWINFO:
        {
            if (packet->length != 5)
            {
                log_e("set fwinfo param length error %u", packet->length);
                bl_response(PACKET_OPCODE_REQUEST, PACKET_ERRCODE_PARAM, NULL, 0);
                return;
            }

            uint8_t *payload = packet->payload + 1;
            uint32_t fwsize = get_u32_inc(&payload);
            log_i("set firmware size: %u", fwsize);
            firmware_size = fwsize;
            firmware_received = 0;
            firmware_programed = 0;
            bl_response(PACKET_OPCODE_REQUEST, PACKET_ERRCODE_OK, NULL, 0);
            break;
        }
        default:
        {
            log_w("unknown request subcode: %02X", subcode);
            break;
        }
    }
}

static void bl_opcode_erase_handler(packet_desc_t *packet)
{
    log_i("erase handler");

    if (packet->length != ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("erase packet length error: %d", packet->length);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    uint8_t *payload = packet->payload;
    uint32_t address = get_u32_inc(&payload);
    uint32_t size = get_u32_inc(&payload);

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("erase address=0x%08X, size=%u out of range", address, size);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (address >= BL_ADDRESS && address < BL_ADDRESS + BL_SIZE)
    {
        log_e("address 0x%08X is protected", address);
        bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    log_d("erase address=0x%08X, size=%u", address, size);

    stm32_flash_unlock();
    stm32_flash_erase(address, size);
    stm32_flash_lock();

    bl_response(PACKET_OPCODE_ERASE, PACKET_ERRCODE_OK, NULL, 0);
}

static void bl_opcode_program_handler(packet_desc_t *packet)
{
    log_i("program handler");

    if (packet->length <= ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("program packet length error: %d", packet->length);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    uint8_t *payload = packet->payload;
    uint32_t address = get_u32_inc(&payload);
    uint32_t size = get_u32_inc(&payload);
    log_i("program address=0x%08X, size=%u", address, size);

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("program address=0x%08X, size=%u out of range", address, size);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (address >= BL_ADDRESS && address < BL_ADDRESS + BL_SIZE)
    {
        log_e("address 0x%08X is protected", address);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    if (size != packet->length - ADDR_SIZE_PARAM_LENGTH)
    {
        log_e("program size %u does not match payload length %u", size, packet->length - 8);
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    xQueueSend(program_queue, &packet, portMAX_DELAY);

    firmware_received += size;
    if (firmware_received < firmware_size)
    {
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_OK, NULL, 0);
    }
}

static void bl_opcode_verify_handler(packet_desc_t *packet)
{
    log_i("verify handler");

    if (packet->length != ADDR_SIZE_CRC_PARAM_LENGTH)
    {
        log_e("verify packet length error: %d", packet->length);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    uint8_t *payload = packet->payload;
    uint32_t address = get_u32_inc(&payload);
    uint32_t size = get_u32_inc(&payload);
    uint32_t crc = get_u32_inc(&payload);

    if (address < STM32_FLASH_BASE || address + size > STM32_FLASH_BASE + STM32_FLASH_SIZE)
    {
        log_e("verify address=0x%08X, size=%u out of range", address, size);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_PARAM, NULL, 0);
        return;
    }

    log_d("verify address=0x%08X, size=%u, crc=0x%08X", address, size, crc);

    uint32_t ccrc = crc32((uint8_t *)address, size);

    if (ccrc != crc)
    {
        log_e("verify failed: expected 0x%08X, got 0x%08X", crc, ccrc);
        bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_VERIFY, NULL, 0);
        return;
    }

    bl_response(PACKET_OPCODE_VERIFY, PACKET_ERRCODE_OK, NULL, 0);
}

static void bl_opcode_reset_handler(packet_desc_t *packet)
{
    log_i("reset handler");
    bl_response(PACKET_OPCODE_RESET, PACKET_ERRCODE_OK, NULL, 0);
    log_w("system resetting...");
    tim_delay_ms(2);

    NVIC_SystemReset();
}

static void bl_opcode_boot_handler(packet_desc_t *packet)
{
    log_i("boot handler");
    bl_response(PACKET_OPCODE_BOOT, PACKET_ERRCODE_OK, NULL, 0);

    boot_application();
}

static void bl_packet_handler(packet_desc_t *packet)
{
    switch (packet->opcode)
    {
        case PACKET_OPCODE_INQUERY:
            bl_opcode_inquery_handler(packet);
            break;
        case PACKET_OPCODE_REQUEST:
            bl_opcode_request_handler(packet);
            break;
        case PACKET_OPCODE_ERASE:
            bl_opcode_erase_handler(packet);
            break;
        case PACKET_OPCODE_PROGRAM:
            bl_opcode_program_handler(packet);
            break;
        case PACKET_OPCODE_VERIFY:
            bl_opcode_verify_handler(packet);
            break;
        case PACKET_OPCODE_RESET:
            bl_opcode_reset_handler(packet);
            break;
        case PACKET_OPCODE_BOOT:
            bl_opcode_boot_handler(packet);
            break;
        default:
            log_w("Unknown command: %02X", packet->opcode);
            break;
    }
}

static void bl_usart_rx_handler(const uint8_t *data, uint32_t length)
{
    if (__unlikely(current_packet == NULL)) // current_packet为NULL的概率极低，用unlikely提示编译器优化分支预测
    {
        return;
    }

    packet_desc_t *packet = current_packet; // 将全局变量放到栈上，提高访问效率

    switch (packet->_state) // switch 相比 if-else 在状态机中效率更高
    {
        case PACKET_STATE_HEADER:
            if (data[0] == PACKET_HEADER_REQUEST)
            {
                packet->header = data[0];
                packet->_state = PACKET_STATE_OPCODE;
            }
            break;
        case PACKET_STATE_OPCODE:
            // 不检查opcode，在后续处理函数中再验证opcode是否合法，提高效率
            packet->opcode = (packet_opcode_t)data[0];
            packet->_state = PACKET_STATE_LENGTH;
            break;
        case PACKET_STATE_LENGTH:
            packet->_temp[packet->_recv++] = data[0];
            if (packet->_recv == 2)
            {
                packet->_recv = 0;
                packet->length = get_u16(packet->_temp);
                if (packet->length > PAYLOAD_SIZE_MAX)
                {
                    log_e("packet length overflow: %u > %u", packet->length, PAYLOAD_SIZE_MAX);
                    packet->_state = PACKET_STATE_HEADER; // 长度错误直接丢弃整个包
                }
                else if (packet->length == 0)
                {
                    packet->_state = PACKET_STATE_CRC16;
                }
                else
                {
                    packet->_state = PACKET_STATE_PAYLOAD;
                    if (packet->opcode == PACKET_OPCODE_PROGRAM)
                    {
                        bl_usart_receive_async(packet->payload - 1, packet->length + 1);
                    }
                }
            }
            break;
        case PACKET_STATE_PAYLOAD:
            packet->payload[packet->_recv++] = data[0];
            if (packet->_recv == packet->length)
            {
                packet->_recv = 0;
                packet->_state = PACKET_STATE_CRC16;
            }
            break;
        case PACKET_STATE_CRC16:
            packet->_temp[packet->_recv++] = data[0];
            if (packet->_recv == 2)
            {
                packet->_recv = 0;
                packet->crc = get_u16(packet->_temp);
                packet->_state = PACKET_STATE_FINISH;

                BaseType_t xHigherPriorityTaskWoken = pdFALSE;
                xQueueSendFromISR(parser_queue, &packet, &xHigherPriorityTaskWoken);
                xQueueReceiveFromISR(empty_queue, &current_packet, &xHigherPriorityTaskWoken);
                portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
                current_packet->_state = PACKET_STATE_HEADER;
            }
            break;
        default:
            break;
    }
}

static void bl_usart_async_rx_handler(void)
{
    current_packet->_state = PACKET_STATE_CRC16;
}

static bool key_trap_check(void)
{
    for (uint32_t t = 0; t < BOOT_DELAY; t+=10)
    {
        tim_delay_ms(10);
        if (!key_read(key2))
            return false;
    }
    log_w("key pressed, trap into boot");
    return true;
}

static void wait_key_release(void)
{
    while (key_read(key2))
        tim_delay_ms(10);
}

static bool key_press_check(void)
{
    if (!key_read(key2))
        return false;

    tim_delay_ms(10);
    if (!key_read(key2))
        return false;

    return true;
}

bool magic_header_trap_boot(void)
{
    if (!magic_header_validate())
    {
        log_w("magic header invalid, trap into boot");
        return true;
    }

    if (!application_validate())
    {
        log_w("application validate failed, trap into boot");
        return true;
    }

    return false;
}

bool rx_trap_boot(void)
{
    // for (uint32_t i = 0; i < 3000; i += 10)
    // {
    //     tim_delay_ms(10);
    //     if (!rb_empty(rxrb))
    //     {
    //         log_w("data received, trap into boot");
    //         return true;
    //     }
    // }

    return false;
}

static inline bool is_opcode_valid(packet_opcode_t opcode)
{
    switch (opcode)
    {
        case PACKET_OPCODE_INQUERY:
        case PACKET_OPCODE_REQUEST:
        case PACKET_OPCODE_ERASE:
        case PACKET_OPCODE_PROGRAM:
        case PACKET_OPCODE_VERIFY:
        case PACKET_OPCODE_RESET:
        case PACKET_OPCODE_BOOT:
            return true;
        default:
            return false;
    }
}

static bool is_packet_valid(packet_desc_t *packet)
{
    if (packet->_state != PACKET_STATE_FINISH)
    {
        log_e("packet state invalid: %d", packet->_state);
        return false;
    }

    if (!is_opcode_valid(packet->opcode))
    {
        log_e("packet opcode invalid: %02X", packet->opcode);
        return false;
    }

    if (packet->length > PAYLOAD_SIZE_MAX)
    {
        log_e("packet length invalid: %u", packet->length);
        return false;
    }

    uint16_t crc = crc16(packet->raw, PACKET_PAYLOAD_OFFSET + packet->length);
    if (crc != packet->crc)
    {
        log_e("invalid packet crc: expected %04X, got %04X", crc, packet->crc);
        return false;
    }

    return true;
}

static void packet_parser_task(void *param)
{
    while (1)
    {
        packet_desc_t *packet;
        xQueueReceive(parser_queue, &packet, portMAX_DELAY);

        if (is_packet_valid(packet))
        {
            bl_packet_handler(packet);
        }
        else
        {
            bl_response(packet->opcode, PACKET_ERRCODE_FORMAT, NULL, 0);
        }

        // 处理完一个包后将其放回empty_queue，供下一个包使用
        if (packet->opcode != PACKET_OPCODE_PROGRAM)
        {
            xQueueSend(empty_queue, &packet, portMAX_DELAY);
        }
    }
}

static void run_program(packet_desc_t *packet)
{
    uint8_t *payload = packet->payload;
    uint32_t address = get_u32_inc(&payload);
    uint32_t size = get_u32_inc(&payload);
    uint8_t *data = payload;

    log_i("run program address=0x%08X, size=%u", address, size);

    led_on(led2);
    stm32_flash_unlock();
    stm32_flash_program(address, data, size);
    stm32_flash_lock();
    led_off(led2);

    log_i("run program done");

    firmware_programed += size;
    if (firmware_programed == firmware_size)
    {
        bl_response(PACKET_OPCODE_PROGRAM, PACKET_ERRCODE_OK, NULL, 0);
    }
}

static void packet_program_task(void *param)
{
    while (1)
    {
        packet_desc_t *packet;
        xQueueReceive(program_queue, &packet, portMAX_DELAY);
        run_program(packet);
        xQueueSend(empty_queue, &packet, portMAX_DELAY);
    }
}

void bootloader_main(void)
{
    log_i("Bootloader started.");

    empty_queue = xQueueCreate(PACKET_QUEUE_MAX, sizeof(packet_desc_t *));
    configASSERT(empty_queue);
    parser_queue = xQueueCreate(PACKET_QUEUE_MAX, sizeof(packet_desc_t *));
    configASSERT(parser_queue);
    program_queue = xQueueCreate(PACKET_QUEUE_MAX, sizeof(packet_desc_t *));
    configASSERT(program_queue);

    for (uint32_t i = 0; i < PACKET_QUEUE_MAX; i++)
    {
        packet_desc_t *packet = pvPortMalloc(sizeof(packet_desc_t));
        configASSERT(packet);
        memset(packet, 0, sizeof(packet_desc_t));
        xQueueSend(empty_queue, &packet, portMAX_DELAY);
    }
    xQueueReceive(empty_queue, &current_packet, portMAX_DELAY);

    xTaskCreate(packet_parser_task, "parser", 1024, NULL, 2, NULL);
    xTaskCreate(packet_program_task, "program", 1024, NULL, 1, NULL);

    key_init(key2);

    bl_usart_init();
    bl_usart_register_rx_callback(bl_usart_rx_handler);
    bl_usart_register_async_rx_callback(bl_usart_async_rx_handler);

    bool trapboot = false;

    // if (!trapboot)
    //     trapboot = magic_header_trap_boot();

    // if (!trapboot)
    //     trapboot = key_trap_check();

    // if (!trapboot)
    //     trapboot = rx_trap_boot();

    trapboot = true; // 强制进入bootloader，方便调试

    if (!trapboot)
        boot_application();

    led_init(led1);
    led_on(led1);
    wait_key_release();

    led_init(led2);
    led_init(led3);

    while (1)
    {
        if (key_press_check())
        {
            log_w("key pressed, rebooting...");
            tim_delay_ms(2);
            NVIC_SystemReset();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
