# 历史版本：原始 BL

本分支保存天气时钟项目最初的串口 Bootloader 方案，主要代码位于 `BL/`。
它是后续外部 Flash A/B 方案的基线，不再作为当前开发主线。

## 版本定位

- `BL/STM32F4_Bootloader/`：运行于 STM32F407 内部 Flash 前部的 Bootloader。
- `BL/WeatherClock/`：从 `0x08010000` 运行的天气时钟应用及 XBIN 生成流程。
- `BL/SerialUpgrader/`：通过串口向 Bootloader 发送 XBIN 的 Windows 上位机。
- 开发环境为 Keil MDK/ARMCC，Bootloader 升级协议使用 USART3。

## 方案特点与边界

该版本由上位机直接擦写 STM32 内部应用区，结构简单，适合学习原始协议、XBIN
格式和跳转流程。它没有外部固件 A/B 槽、试运行确认和自动回退机制；写内部应用
区时若发生掉电，应用可能不完整，但 Bootloader 区仍可通过 J-Link/ST-Link 恢复。

本快照同时保留了开发期间实际使用的 Keil Pack/ARMCC 配置、USART3 发送等待修正，
以及用于辨认测试固件的 Wi-Fi SSID `XXXYYY`。这些属于历史测试设置，使用前应按
实际环境修改。

## 后续版本

- `history/bl-w25q-ab-keil`：加入双 W25Q128 外部 A/B 存储、掉电恢复与自动回退。
- `main`：当前维护版本，迁移到 VS Code + CMake + GCC + GDB/J-Link 工作流，并继续
  优化串口和 SPI 传输性能。
