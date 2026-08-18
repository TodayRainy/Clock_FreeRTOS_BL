# BL_W25Q_AB_VSCode

这是从 `BL_W25Q_AB` 独立复制后完成 VS Code + CMake + GCC + GDB 迁移和
实板回归的外部 A/B Bootloader 版本。原 `BL_W25Q_AB` 没有被覆盖。

- Bootloader：`STM32F4_Bootloader/`
- 天气时钟应用与 XBIN 生成：`WeatherClock/`
- WinForms/命令行升级工具：`SerialUpgrader/`
- VS Code/GCC 迁移、构建、调试和实测说明：[`VSCODE_GCC_MIGRATION.md`](VSCODE_GCC_MIGRATION.md)
- 当前代码架构、状态机、协议与维护指南：[`CODE_GUIDE.md`](CODE_GUIDE.md)
- 设计、测试、使用和 ST-Link 救援说明：[`AB_BOOTLOADER_REPORT.md`](AB_BOOTLOADER_REPORT.md)
- 初始及最终安全基线：[`SAFETY_BASELINE.md`](SAFETY_BASELINE.md)

正式升级只写非活动 W25Q128 槽，Bootloader 本身不支持在线自更新。

当前 Bootloader 为 `v2.1.0-ab-fast-spidma`：复位后仍以115200握手，默认协商
到2 Mbps；USART3使用循环DMA接收，单包固件数据为8 KiB，中间包通过接受ACK
与W25Q页编程形成流水。升级选槽使用只读BCR的快速状态命令，W25Q批量读取使用
SPI1双向DMA；最终同步包和槽全量CRC仍负责确认实际写入。312,428字节XBIN的
实测含握手升级时间为4.46～4.49秒。

常用命令：

```powershell
cmake --preset debug
cmake --build --preset validate-debug --parallel

cmake --preset release
cmake --build --preset validate-release --parallel
```

命令行升级：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM18 .\build\release\WeatherClock\stm32f407_upgrade.xbin
```

端口号以Windows当前枚举为准。命令末尾可增加 `921600`，在较长或质量一般的
TTL接线上使用较保守的高速档。

每个配置的 ELF、HEX、BIN 和 XBIN 位于 `build/debug` 或 `build/release`，不会
相互覆盖。应用仍必须通过 XBIN/A-B 流程安装；不要把裸应用 BIN 当作正常升级包。
