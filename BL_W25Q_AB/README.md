# BL_W25Q_AB

这是从原 `BL` 项目独立复制并完成实板验证的外部 A/B Bootloader 版本。

- Bootloader：`STM32F4_Bootloader/`
- 天气时钟应用与 XBIN 生成：`WeatherClock/`
- WinForms/命令行升级工具：`SerialUpgrader/`
- 设计、测试、使用和 ST-Link 救援说明：[`AB_BOOTLOADER_REPORT.md`](AB_BOOTLOADER_REPORT.md)
- 初始及最终安全基线：[`SAFETY_BASELINE.md`](SAFETY_BASELINE.md)

正式升级只写非活动 W25Q128 槽，Bootloader 本身不支持在线自更新。
