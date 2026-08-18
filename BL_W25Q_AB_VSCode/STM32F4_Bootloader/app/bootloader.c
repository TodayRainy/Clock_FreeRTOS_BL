/**
 * @file bootloader.c
 * @brief Bootloader入口、A/B状态机和应用跳转编排。
 *
 * 本文件只决定“现在应安装、试运行、回退还是启动哪个固件”。串口帧解析和
 * Opcode处理位于boot_protocol.c，外部槽读写与安装位于firmware_slot.c。
 */

#include <stdbool.h>
#include <stdint.h>
#include "stm32f4xx.h"
#include "board.h"
#include "boot_protocol.h"
#include "bl_usart.h"
#include "tim_delay.h"
#include "w25q.h"
#include "bl24c512.h"
#include "boot_state.h"
#include "firmware_slot.h"
#include "magic_header.h"
#include "bootloader.h"

#define LOG_TAG    "boot"
#define LOG_LVL    ELOG_LVL_INFO
#include "elog.h"

#define BOOT_ENTRY_WINDOW_MS  3000u

extern void JumpApp(uint32_t base);

/**
 * @brief 关闭Bootloader使用的中断和外设，然后跳到应用向量表。
 *
 * 调用本函数前必须已经由 @ref boot_prepare_application 完成固件全量校验。
 * 函数通过汇编入口同时装载应用MSP和Reset_Handler，不会返回。
 */
static void jump_to_application(void)
{
    log_w("booting application...");
    tim_delay_ms(2);

    w25q_deinit();
    led_off(led1);
    TIM_DeInit(TIM6);
    USART_DeInit(USART1);
    bl_usart_deinit();
    NVIC_DisableIRQ(TIM6_DAC_IRQn);
    NVIC_DisableIRQ(USART1_IRQn);

    SCB->VTOR = MAGIC_DATA_ADDRESS;
    JumpApp(MAGIC_DATA_ADDRESS);
}

/**
 * @brief 在持久化状态改变后执行一次干净复位。
 *
 * 安装和回退都跨越多个启动阶段。每次关键状态落盘后复位，可以让下一阶段
 * 从统一的硬件初始状态开始，也便于掉电后沿相同路径重试。
 */
static void reset_after_state_change(void)
{
    tim_delay_ms(5);
    NVIC_SystemReset();
}

/**
 * @brief 保存指定阶段并复位。
 * @return EEPROM保存失败时返回 false；保存成功后系统复位，不会返回。
 */
static bool save_phase_and_reset(boot_state_t *state, boot_phase_t phase)
{
    state->phase = phase;
    if (!boot_state_save(state))
    {
        log_e("cannot save boot phase %u", phase);
        return false;
    }
    reset_after_state_change();
    return false;
}

/**
 * @brief 把当前事务转入回退阶段。
 */
static bool schedule_rollback(boot_state_t *state, const char *reason)
{
    log_w("%s; scheduling rollback", reason);
    return save_phase_and_reset(state, BOOT_PHASE_ROLLBACK_PENDING);
}

/**
 * @brief 处理InstallPending阶段。
 *
 * pending槽必须同时匹配BCR记录的CRC。外部校验成功前不擦内部Flash；安装完成
 * 后先保存Trial再复位，因此安装中断会在下次启动重新执行本阶段。
 */
static bool handle_install_pending(boot_state_t *state)
{
    magic_header_t header;

    if (state->pending_slot >= BOOT_SLOT_COUNT ||
        !firmware_slot_validate(state->pending_slot, &header) ||
        header.data_crc32 != state->pending_crc32)
        return schedule_rollback(state, "pending slot is invalid");

    log_w("installing slot %u", state->pending_slot);
    if (!firmware_slot_install(state->pending_slot, &header))
    {
        /* 保持InstallPending。外部副本仍完整，复位后可以安全重试。 */
        log_e("slot installation failed; state remains pending");
        return false;
    }

    state->attempts_left = BOOT_TRIAL_ATTEMPTS;
    return save_phase_and_reset(state, BOOT_PHASE_TRIAL);
}

/**
 * @brief 处理RollbackPending阶段并恢复已确认固件。
 */
static bool handle_rollback_pending(boot_state_t *state)
{
    magic_header_t header;

    if (state->active_slot >= BOOT_SLOT_COUNT ||
        !firmware_slot_validate(state->active_slot, &header) ||
        header.data_crc32 != state->active_crc32)
    {
        /* 没有可信的已确认副本时停在Bootloader，等待串口或SWD救援。 */
        log_e("confirmed rollback slot is invalid");
        return false;
    }

    log_w("restoring confirmed slot %u", state->active_slot);
    if (!firmware_slot_install(state->active_slot, &header))
    {
        /* 保持RollbackPending，下一次复位继续恢复同一个已确认槽。 */
        log_e("rollback installation failed; state remains pending");
        return false;
    }

    state->phase = BOOT_PHASE_NORMAL;
    state->pending_slot = BOOT_SLOT_NONE;
    state->pending_crc32 = 0;
    state->attempts_left = 0;
    if (!boot_state_save(state))
    {
        log_e("cannot save rollback result");
        return false;
    }
    reset_after_state_change();
    return false;
}

/**
 * @brief 处理Trial阶段，并在每次实际跳转前消耗一次尝试次数。
 */
static bool handle_trial(boot_state_t *state)
{
    magic_header_t internal_header;
    magic_header_t pending_header;

    if (!firmware_internal_validate(&internal_header) ||
        internal_header.data_crc32 != state->pending_crc32)
    {
        /* 内部半成品可从仍然完整的pending槽重装；pending也损坏则回退。 */
        if (state->pending_slot < BOOT_SLOT_COUNT &&
            firmware_slot_validate(state->pending_slot, &pending_header) &&
            pending_header.data_crc32 == state->pending_crc32)
        {
            log_w("trial image is incomplete; reinstalling pending slot");
            return save_phase_and_reset(state, BOOT_PHASE_INSTALL_PENDING);
        }
        return schedule_rollback(state, "trial and pending images are invalid");
    }

    if (state->attempts_left == 0)
        return schedule_rollback(state, "trial firmware was not confirmed");

    state->attempts_left--;
    if (!boot_state_save(state))
    {
        log_e("cannot update trial attempt counter");
        return false;
    }
    log_w("booting trial image, %u attempts remain", state->attempts_left);
    return true;
}

/**
 * @brief 在BCR完全丢失且内部固件无效时，从任一完整槽重建恢复状态。
 *
 * 没有BCR就无法知道哪个槽更新，因此只把该流程作为最后的可启动性恢复。
 * 正常升级和回退始终使用BCR明确记录的槽。
 */
static bool recover_without_state(boot_state_t *state)
{
    magic_header_t header;
    uint8_t slot;

    for (slot = 0; slot < BOOT_SLOT_COUNT; ++slot)
    {
        if (!firmware_slot_validate(slot, &header))
            continue;

        log_w("recovering missing state from slot %u", slot);
        boot_state_default(state);
        state->phase = BOOT_PHASE_ROLLBACK_PENDING;
        state->active_slot = slot;
        state->active_crc32 = header.data_crc32;
        if (!boot_state_save(state))
            return false;
        reset_after_state_change();
        return false;
    }
    return false;
}

/**
 * @brief 根据BCR和固件完整性准备一次应用启动。
 *
 * @return 内部应用已经过完整校验且允许立即跳转时返回 true；需要安装、回退、
 *         复位或人工救援时返回 false。
 */
static bool boot_prepare_application(void)
{
    boot_state_t state;
    magic_header_t internal_header;
    magic_header_t active_header;
    bool state_found = boot_state_load(&state);

    log_i("state=%s phase=%u active=%u pending=%u attempts=%u seq=%u",
          state_found ? "valid" : "default", state.phase, state.active_slot,
          state.pending_slot, state.attempts_left, state.sequence);

    switch ((boot_phase_t)state.phase)
    {
        case BOOT_PHASE_INSTALL_PENDING:
            return handle_install_pending(&state);

        case BOOT_PHASE_TRIAL:
            return handle_trial(&state);

        case BOOT_PHASE_ROLLBACK_PENDING:
            return handle_rollback_pending(&state);

        case BOOT_PHASE_NORMAL:
            break;
    }

    if (firmware_internal_validate(&internal_header) &&
        (state.active_slot == BOOT_SLOT_NONE ||
         internal_header.data_crc32 == state.active_crc32))
        return true;

    if (state.active_slot < BOOT_SLOT_COUNT &&
        firmware_slot_validate(state.active_slot, &active_header) &&
        active_header.data_crc32 == state.active_crc32)
        return schedule_rollback(&state, "internal image is invalid");

    if (!state_found)
    {
        recover_without_state(&state);
        log_e("no bootable internal or external firmware");
    }
    else
    {
        log_e("confirmed firmware is unavailable; waiting for rescue");
    }
    return false;
}

/**
 * @brief 检测KEY2是否持续按下整个进入窗口。
 */
static bool key_trap_requested(void)
{
    uint32_t elapsed;

    for (elapsed = 0; elapsed < BOOT_ENTRY_WINDOW_MS; elapsed += 10)
    {
        tim_delay_ms(10);
        if (!key_read(key2))
            return false;
    }
    log_w("key held, staying in bootloader");
    return true;
}

/**
 * @brief 在启动窗口内检测USART3数据。
 */
static bool serial_trap_requested(void)
{
    uint32_t elapsed;

    for (elapsed = 0; elapsed < BOOT_ENTRY_WINDOW_MS; elapsed += 10)
    {
        tim_delay_ms(10);
        if (boot_protocol_data_pending())
        {
            log_w("serial data received, staying in bootloader");
            return true;
        }
    }
    return false;
}

/**
 * @brief 判断BCR是否要求Bootloader立即推进一个恢复事务。
 *
 * InstallPending、Trial和RollbackPending都依赖有限次数或可重试的状态转换。
 * 这些阶段跳过普通串口等待可以缩短升级后的自动安装时间。KEY2检测仍在本
 * 判断之前执行，因此持续按住KEY2仍可中止自动流程并进入人工救援。
 */
static bool boot_transition_pending(void)
{
    boot_state_t state;

    return boot_state_load(&state) && state.phase != BOOT_PHASE_NORMAL;
}

/**
 * @brief 等待进入Bootloader时使用的按键释放，避免立刻触发复位。
 */
static void wait_key_release(void)
{
    while (key_read(key2))
        tim_delay_ms(10);
}

/**
 * @brief 对Bootloader运行期间的复位按键做10ms消抖。
 */
static bool key_pressed(void)
{
    if (!key_read(key2))
        return false;
    tim_delay_ms(10);
    return key_read(key2);
}

void bootloader_main(void)
{
    bool stay_in_bootloader;

    log_i("Bootloader started.");
    key_init(key2);
    boot_protocol_init();

    w25q_init();
    log_i("W25 A=%06X, B=%06X", w25q_read_jedec_id(W25Q_CHIP_A),
          w25q_read_jedec_id(W25Q_CHIP_B));
    bl24c512_init();
    log_i("BL24C512 %s", bl24c512_is_present() ? "ready" : "not found");

    stay_in_bootloader = key_trap_requested();
    if (!stay_in_bootloader && !boot_transition_pending())
        stay_in_bootloader = serial_trap_requested();

    if (!stay_in_bootloader && boot_prepare_application())
        jump_to_application();

    led_init(led1);
    led_on(led1);
    wait_key_release();

    while (1)
    {
        boot_protocol_action_t action;

        if (key_pressed())
        {
            log_w("key pressed, rebooting...");
            tim_delay_ms(2);
            NVIC_SystemReset();
        }

        action = boot_protocol_process();
        if (action == BOOT_PROTOCOL_ACTION_BOOT_APPLICATION &&
            boot_prepare_application())
            jump_to_application();
    }
}
