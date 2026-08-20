/**
 * @file ab_boot_contract.h
 * @brief Bootloader 与应用共同使用的 A/B 启动状态存储格式。
 *
 * 本文件只定义 EEPROM 中的二进制契约，不包含 EEPROM 驱动或状态迁移逻辑。
 * Bootloader 负责创建和解释状态，试运行应用只在确认成功时更新它。
 * 这些字段的顺序、大小和地址一旦改变，就会造成已保存状态不兼容。
 */

#ifndef CLOCK_F407_AB_BOOT_CONTRACT_H
#define CLOCK_F407_AB_BOOT_CONTRACT_H

#include <stdint.h>

#define BOOT_STATE_MAGIC          0x32545342u /**< 小端内存中为字符串 "BST2"。 */
#define BOOT_STATE_ADDR_A         0xFD00u     /**< EEPROM 状态副本 A。 */
#define BOOT_STATE_ADDR_B         0xFD80u     /**< EEPROM 状态副本 B。 */
#define BOOT_STATE_RECORD_SIZE    128u        /**< 每条记录恰好占一个 EEPROM 页。 */

#define BOOT_SLOT_A               0u
#define BOOT_SLOT_B               1u
#define BOOT_SLOT_COUNT           2u
#define BOOT_SLOT_NONE            0xFFu
#define BOOT_TRIAL_ATTEMPTS       3u

/**
 * @brief A/B 启动流程的持久化阶段。
 */
typedef enum
{
    BOOT_PHASE_NORMAL = 0,          /**< 内部固件与 active_slot 均已确认。 */
    BOOT_PHASE_INSTALL_PENDING,     /**< pending_slot 已提交，等待安装到内部 Flash。 */
    BOOT_PHASE_TRIAL,               /**< 新固件正在试运行，尚未由应用确认。 */
    BOOT_PHASE_ROLLBACK_PENDING,    /**< 需要从已确认的 active_slot 恢复。 */
} boot_phase_t;

/**
 * @brief EEPROM 中保存的一条 A/B 启动控制记录（BCR）。
 *
 * 两份记录在 @ref BOOT_STATE_ADDR_A 和 @ref BOOT_STATE_ADDR_B 之间交替写入。
 * `sequence` 较大的有效记录代表最新状态；`record_crc32` 覆盖其前面的124字节。
 */
typedef struct
{
    uint32_t magic;          /**< 固定为 @ref BOOT_STATE_MAGIC。 */
    uint32_t sequence;       /**< 每次保存递增，用于选择较新的有效副本。 */
    uint8_t phase;           /**< 当前阶段，取值为 @ref boot_phase_t。 */
    uint8_t active_slot;     /**< 已确认且必须保留的回退槽。 */
    uint8_t pending_slot;    /**< 等待安装或正在试运行的槽。 */
    uint8_t attempts_left;   /**< Trial 阶段剩余启动次数。 */
    uint32_t active_crc32;   /**< active_slot 中应用数据的 CRC32。 */
    uint32_t pending_crc32;  /**< pending_slot 中应用数据的 CRC32。 */
    uint32_t minimum_security_version; /**< 已确认固件形成的防降级下限。 */
    uint32_t pending_security_version; /**< 当前安装/Trial候选的安全版本。 */
    uint8_t reserved[96];    /**< 保留空间，保持记录为一个128字节页。 */
    uint32_t record_crc32;   /**< 本字段之前所有字节的 CRC32。 */
} boot_state_t;

/* ARMCC5 没有 C11 static_assert，使用数组长度在编译期锁定存储格式。 */
typedef char boot_state_record_must_be_128_bytes[
    (sizeof(boot_state_t) == BOOT_STATE_RECORD_SIZE) ? 1 : -1];

#endif /* CLOCK_F407_AB_BOOT_CONTRACT_H */
