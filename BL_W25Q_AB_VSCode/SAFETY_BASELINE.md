# A/B Bootloader 安全基线

本目录从 `BL` 的当前已验证状态复制，原目录不参与后续双区开发。

## 设备备份

- 备份时间：2026-08-18
- MCU：STM32F407VETx，内部 Flash 512 KiB
- 文件：`../.device-backups/stm32f407_before_ab_20260818.bin`
- 文件长度：524288 字节
- SHA-256：`0F0856AB11F3277027A7A63DDA8AADCBE2585E2E820815101B423D9706D416A8`

Option Bytes：

- RDP：`0xAA`，Level 0
- BOR：`0x3`
- Watchdog：软件模式
- WRP0～WRP11：均未启用写保护

恢复时可使用 STM32CubeProgrammer 和 ST-Link，把上述文件写回 `0x08000000`。

## 固定的内部 Flash 布局

| 区域 | 地址 | 长度 |
| --- | --- | --- |
| Bootloader | `0x08000000` | 48 KiB，至 `0x0800BFFF` |
| XBIN 固件头 | `0x0800C000` | 4 KiB |
| 应用程序 | `0x08010000` | 最大 448 KiB，至 `0x0807FFFF` |

STM32 只从内部应用区运行。两颗 W25Q128 用作外部 Slot A/Slot B，Bootloader
负责把选中的完整固件安装到内部应用区。

## 开发约束

1. 擦除外部 Flash 或 EEPROM 前先读取并备份目标区域。
2. 安装应用时先使内部固件头失效，最后才写入有效固件头。
3. 开发阶段保留 ST-Link，不启用 Bootloader 扇区写保护。
4. 每增加一个持久化步骤，都要在该步骤前后做复位恢复测试。

## 重构后完工状态与最终救援镜像

- 当前整片内部 Flash：`../.device-backups/stm32f407_ab_refactored_v1_2_20260818.bin`
- 文件长度：524288 字节
- SHA-256：`4F59A817F0ACF2A00AE69075FFB4EACF5B49186D98C2EB2EF1FE62218E2CD1A9`
- Bootloader：`v1.2.0-ab`
- 已确认活动固件：Slot A，`v1.4.0-260818-1702-ab`，CRC32 `0x49F1DEA4`
- 备用固件：Slot B，与 Slot A 同版本、同 CRC32，完整校验有效
- 上一版整片备份 `stm32f407_ab_v1_4_aaaaa_20260818.bin` 仍保留，可恢复到重构前状态。

最终状态已通过上位机 `--status COM18` 实机读取，并在读取后重新启动应用。板载
Bootloader 区与最终构建的 `stm32f407.bin` 前 21324 字节逐字节一致。

## J-Link 调试基线

- 探针：J-Link V9.40，序列号 `69408257`，目标电压约 3.29 V。
- 接口：SWD，Keil 默认 1 MHz；Keil 驱动为 `Segger\JL2CM3.dll`。当前接线在
  4 MHz 下出现过一次连接失败，涉及 Bootloader 写入时使用 100 kHz 保守速度。
- Flash Algorithm：`STM32F4xx_512.FLM`，地址 `0x08000000`，长度 512 KiB。
- Bootloader 工程允许调试前更新 Flash，便于修改和单步调试 Bootloader。
- WeatherClock 工程关闭“Update Target before Debugging”。正式应用应先生成并
  通过 A/B XBIN 流程安装，再用 Keil 附加和加载符号，避免只写裸应用 BIN 后
  造成内部 Header 的 CRC32 与数据不一致。
- 已使用 Keil 自带 J-Link DLL 对最终 512 KiB 镜像完成只读全片校验，结果为
  `Verify successful`；配置切换过程中没有擦写板载固件。

当前探针来源尚未确认为 SEGGER 原厂或授权 OEM。保持现有 J-Link DLL/固件版本，
不要运行 Configurator 的固件替换，也不要在新版安装器中启用“Update DLL in
other applications”。确认硬件身份之前，升级探针软件存在使非授权兼容硬件失效
的风险。

## VS Code/GCC 迁移后的救援基线

- 迁移前、GCC 应用已确认而 Bootloader 尚未替换的整片镜像：
  `../.device-backups/stm32f407_pre_gcc_boot_20260818.bin`
- 长度：524288 字节
- SHA-256：`BA609B747883CBCA08874857723C50EBC3381682092C6802B4960828C6765411`
- 迁移最终整片镜像：
  `../.device-backups/stm32f407_vscode_gcc_final_20260818.bin`
- 长度：524288 字节
- SHA-256：`E3276FCD9F75A3B53DF863DA97EF57490DF0E8D3983FA1EAC28616FD1883C793`

最终镜像已用 J-Link 对内部 Flash `0x08000000..0x0807FFFF` 做 512 KiB 全量
`verifybin`，结果为 `Verify successful`。对应恢复任务为
`J-Link: Restore Known-Good 512 KiB Image`；它会覆盖整片内部 Flash，只用于
硬救援。原 Keil/ARMCC 救援镜像继续保留，形成两个独立恢复点。

最终实板状态为 Bootloader `v1.3.0-ab-gcc`、`Normal`、active Slot B、sequence
35。Slot A 仍是原 ARMCC 已确认固件；Slot B 是经完整 A/B 流程安装并由 FreeRTOS
应用确认的 GCC 固件，版本 `v1.4.0-260818-1749-ab`，CRC32 `0x7AD86F2A`。

## DMA流水优化后的救援基线

- Bootloader：`v2.0.1-ab-dma-pipeline`；
- 最终内部Flash：`../.device-backups/stm32f407_ab_dma_pipeline_v2_0_1_20260818.bin`；
- 长度：524,288字节；
- SHA-256：`1BD66F11090ED9FED23D1BC3883138121C33F12F251CEB80D59EB6FBF9D86BFE`；
- 恢复脚本：`tools/jlink/restore_dma_pipeline_final.jlink`；
- 最终BCR：`Normal / active=B / pending=none / sequence=110`；
- Slot A/B：版本均为 `v1.4.0-260818-1858-ab`，数据CRC32均为
  `0x35587D08`，Header和全量数据校验均有效。

该镜像已用J-Link读取完整512 KiB后立即对芯片执行 `verifybin`，结果为
`Verify successful`。写入非活动槽约10.5%时强制终止上位机的测试中，活动槽
和内部已确认应用保持不变；重新完整升级后恢复为双槽有效。

旧版J-Link DLL偶尔无法直接执行STM32专用的复位附着。此时可先以通用
`Cortex-M4` 目标运行 `tools/jlink/prehalt_core.jlink` 停住内核，再切回
`STM32F407VE` 执行下载或复位；该步骤只控制CPU运行状态，不擦写Flash。

## 快速状态与SPI读取DMA后的救援基线

- Bootloader：`v2.1.0-ab-fast-spidma`；
- 最终内部Flash：
  `../.device-backups/stm32f407_ab_fast_spidma_v2_1_0_20260818.bin`；
- 长度：524,288字节；
- SHA-256：`62687A3225954BCAFA13DC741CFC8CAB2D4ED46120CAF3C36818C01B9D1BCE1D`；
- 恢复脚本：`tools/jlink/restore_fast_spidma_final.jlink`；
- Release Bootloader：21,780字节，SHA-256
  `19E2AE182A0DDD94252E68419403DA0C9C503C2AD9D9CB6ECBB37D4EF9F9ED0B`；
- 最终BCR：`Normal / active=A / pending=none / sequence=123`；
- Slot A/B：版本均为 `v1.4.0-260818-1913-ab`，数据CRC32均为
  `0x98A25921`，Header和全量数据校验均有效。

最终镜像由J-Link读取完整512 KiB后立即执行 `verifybin`，结果为
`Verify successful`；镜像前21,780字节与最终Release Bootloader逐字节一致。
2 Mbps升级在5.2%处中断时，活动B槽和BCR保持有效，未完整写入的A槽因Header
最后提交而保持无效；随后重新完整升级、安装和应用确认，恢复为上述双槽有效
状态。SPI1 CR1/CR2及DMA2 Stream0/Stream3在应用跳转后均确认没有Bootloader
遗留的使能状态。
