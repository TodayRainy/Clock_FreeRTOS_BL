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

## v3签名、IWDG与维护权限开发基线

- Bootloader版本：`v3.0.0-signed-dev/prod`；
- DEV Release：Flash 27,216字节/48 KiB，RAM 35,608字节/128 KiB；
- DEV Release Bootloader BIN SHA-256：
  `9A34311453748EA07BD1859EC865BFFA79BBE12E753A66B35B8E88CB1F584CFF`；
- WeatherClock Release：Flash 308,788字节/448 KiB，RAM 115,536字节/128 KiB；
- XBIN格式：v2，SHA-256 + ECDSA-P256，DEV测试公钥ID `0x7F9F8899`；
- 当前XBIN文件SHA-256：
  `B37D46FCDECDC294CB80DD8FA4A02A508CD116F7254C3F5995A8ACFC743BBA76`；
- XBIN版本：`v1.4.0-260818-2038-ab`，Payload CRC32 `0x8AB683DA`，
  Payload SHA-256
  `58AC5D402849DDA1F6B40E35D7F8107EEB42AE796DDB8390D3860B601DDC0639`；
- Debug、DEV Release与独立测试PROD Release均完成构建和完整镜像校验；
- Bootloader同源C语言SHA-256/micro-ecc验签自测通过；
- Payload损坏、无私钥重算CRC/SHA、元数据修改、签名修改四类负向测试均被拒绝；
- SerialUpgrader .NET 8 Release：0 warning、0 error。

### v3实板迁移和故障注入结果

J-Link V9.40（S/N `69408257`）已能稳定附着，VTref约3.29 V。本轮没有修改RDP、
WRP或其他Option Bytes，并在每次Bootloader写入后执行`verifybin`。

1. 先读取迁移前512 KiB镜像
   `../.device-backups/stm32f407_pre_signed_v3_20260818.bin`，SHA-256为
   `62687A3225954BCAFA13DC741CFC8CAB2D4ED46120CAF3C36818C01B9D1BCE1D`。
2. 第一次从旧v2.1迁移签名应用时，实板发现应用在LSI尚未启动时等待IWDG寄存器
   同步。连续Trial失败后，原A槽被自动重装，BCR回到`Normal / active=A`，证明
   当次故障没有破坏已确认固件。修正为先启动IWDG再配置PR/RLR后重新构建。
3. 用旧Bootloader依次把签名XBIN安装到B、A槽，再用J-Link只更新前48 KiB的
   v3 DEV Bootloader。v3在STM32上对两个槽完成SHA-256和ECDSA验签。
4. v3下完成一次A到B的正常升级；2 Mbps链路总耗时4.67秒，之后应用健康确认使
   BCR回到`Normal / active=B`。
5. 将Payload改1字节、重新计算CRC32/SHA-256/Header CRC而保留旧签名的伪造包
   完整上传。设备在`SLOT_ACTIVATE`验签阶段拒绝它，active B与sequence 142保持
   不变；随后用合法XBIN恢复A槽。
6. 使用同一测试公钥构建PROD Bootloader做权限回归：未按KEY2时W25原始读取返回
   `DENIED`，A/B状态查询和正常启动不受影响。刷回DEV后同一只读探针返回
   `ALLOWED`。
7. 最终状态为`v3.0.0-signed-dev`、`Normal / active=A / pending=none /
   attempts=0 / sequence=146`；Slot A/B均为上述2038版本并通过完整校验。应用连续
   运行超过一个约32秒IWDG周期后，J-Link读取PC=`0x080111FE`，确认仍在应用区
   正常执行。

最终512 KiB救援镜像为
`../.device-backups/stm32f407_signed_v3_final_20260818.bin`，长度524,288字节，
SHA-256为`09C7EB93519822166D819A03910E8DC0394C2E89056E8618443C59A942DC89F7`。
J-Link已对该文件和芯片执行完整`verifybin`；镜像前27,216字节又与DEV Release
Bootloader逐字节比较一致。恢复脚本为
`tools/jlink/restore_signed_v3_final.jlink`。由于IWDG跨软件复位继续运行，最终
备份脚本不先复位目标，而是在已运行的DEV应用设置调试冻结位后暂停并读取；这也
避免旧J-Link在100 kHz读取512 KiB时被看门狗中断。

## v3.2防降级与人工回退救援基线

- Bootloader版本：`v3.2.0-antirob-dev`；
- DEV Release：Flash 27,856字节/48 KiB，RAM 35,624字节/128 KiB；
- WeatherClock Release：Flash 308,868字节/448 KiB，RAM 115,536字节/128 KiB；
- 固件安全版本：Slot A/B均为`1`，设备最低安全版本为`1`；
- 最终BCR：`Normal / active=A / pending=none / attempts=0 / sequence=171`；
- Slot A/B：版本均为`v1.4.0-260819-1310-ab`，Payload CRC32均为
  `0x45A5024D`，Header、SHA-256、ECDSA签名及全量数据校验均有效。

实板先安装并确认安全版本1，再完整上传一份合法签名的安全版本0固件。普通启动
Bootloader时，设备在`SLOT_ACTIVATE`阶段返回`DENIED`，活动槽和安全版本下限均
保持不变。随后用J-Link仅把RAM中的`downgrade_authorized`置为1，模拟KEY2检测
完成后的授权状态；相同的安全版本0固件成功安装并由应用确认，但最低安全版本仍
保持为1。再次复位后授权自动消失。该测试覆盖了物理按键检测之后的完整回退策略，
实际机械按下KEY2的电气路径仍保留为人工验收项。最后连续两次安装安全版本1，恢复
为上述双槽同版本的最终状态。

最终512 KiB救援镜像为
`../.device-backups/stm32f407_antirob_v3_2_final_20260819.bin`，长度524,288字节，
SHA-256为`365043913AD00CCF6D924A919AFF0A28F48753901FFC66B27CB9F7ED016EB9BE`。
镜像前27,856字节与DEV Release Bootloader逐字节一致，并已通过J-Link完整
`verifybin`。恢复与只读验证脚本分别为
`tools/jlink/restore_antirob_v3_2_final.jlink`和
`tools/jlink/verify_antirob_v3_2_final.jlink`。本轮未修改RDP、WRP或其他Option
Bytes；最终J-Link探测到VTref约3.29 V，PC位于应用区`0x08013DCC`，板卡处于正常
运行状态。
