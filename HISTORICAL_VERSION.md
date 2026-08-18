# 历史版本：W25Q 外部 A/B Bootloader（Keil）

本分支保存从原始 `BL` 演进而来的第一版外部 A/B Bootloader，核心项目位于
`BL_W25Q_AB/`。该版本已经完成实板开发与测试，但不再作为当前开发主线。

## 版本定位

- 两颗板载 W25Q128 分别作为 Slot A 和 Slot B，升级只写非活动槽。
- BL24C512 保存双副本 BCR，记录活动槽、待安装槽、Trial 次数和恢复阶段。
- STM32 内部 Flash 保留唯一应用执行区；Bootloader 将选定槽安装到内部 Flash。
- 新应用经过 Trial 启动并由应用确认，未确认达到次数上限后自动安装旧活动槽。
- USART3 延续原有帧协议，并扩展外部 Flash、EEPROM、槽状态和激活命令。
- 开发与调试环境为 Keil MDK/ARMCC + J-Link，上位机为 .NET WinForms/命令行工具。

## 安全边界

正常在线升级不会擦写 Bootloader，也不会覆盖已确认的活动槽。外部槽写入、内部
安装或 BCR 更新期间掉电，下一次启动会继续安装或执行回退。Bootloader 自身仍须
通过 J-Link/ST-Link 更新，并保留 SWD、RDP Level 0 作为最终救援手段。

该分支保存源码、工程文件与设计/测试报告；Keil、.NET 生成目录以及 BIN、XBIN、
AXF 等构建产物按仓库规则忽略，需要在本机重新生成。完整实现细节、测试结果和
救援步骤见 `BL_W25Q_AB/AB_BOOTLOADER_REPORT.md`。

## 与当前主线的关系

`main` 中的 `BL_W25Q_AB_VSCode/` 在此方案上迁移到 VS Code + CMake + GCC + GDB，
并继续实现高波特率协商、ACK 流水、SPI DMA 与跳转前外设反初始化等改进。若要继续
开发，应以 `main` 为准；本分支主要用于追溯 Keil 阶段的设计和测试基线。
