#include <stdint.h>
#include <stdio.h>
#include "console.h"
#include "tim_delay.h"
#include "elog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "led.h"
#include "board.h"

extern void board_lowlevel_init(void);
extern void bootloader_main(void);

static void main_init(void *arg)
{
    console_init();
    tim_delay_init();
    led_init(led2);
    led_off(led2);

#ifdef ELOG_OUTPUT_ENABLE
    elog_init();
    elog_set_fmt(ELOG_LVL_ASSERT, ELOG_FMT_ALL);
    elog_set_fmt(ELOG_LVL_ERROR, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME | ELOG_FMT_P_INFO);
    elog_set_fmt(ELOG_LVL_WARN, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME | ELOG_FMT_P_INFO);
    elog_set_fmt(ELOG_LVL_INFO, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    elog_set_fmt(ELOG_LVL_DEBUG, ELOG_FMT_LVL | ELOG_FMT_TAG | ELOG_FMT_TIME);
    elog_set_fmt(ELOG_LVL_VERBOSE, ELOG_FMT_TAG);
    elog_start();
#endif

    bootloader_main();

    vTaskDelete(NULL);
}

int main(void)
{
    board_lowlevel_init();

    xTaskCreate(main_init, "init", 2048, NULL, 9, NULL);
    vTaskStartScheduler();

    return 0;
}

