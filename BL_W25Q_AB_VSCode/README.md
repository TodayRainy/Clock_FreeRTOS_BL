# BL_W25Q_AB_VSCode

这是从 `BL_W25Q_AB` 独立复制后完成 VS Code + CMake + GCC + GDB 迁移和
实板回归的外部 A/B Bootloader 版本。原 `BL_W25Q_AB` 没有被覆盖。

- Bootloader：`STM32F4_Bootloader/`
- 天气时钟应用与 XBIN 生成：`WeatherClock/`
- WinForms/命令行升级工具：`SerialUpgrader/`
- VS Code/GCC 迁移、构建、调试和实测说明：[`VSCODE_GCC_MIGRATION.md`](VSCODE_GCC_MIGRATION.md)
- 当前代码架构、状态机、协议与维护指南：[`CODE_GUIDE.md`](CODE_GUIDE.md)
- 固件签名、看门狗和 DEV/PROD 权限说明：[`SECURE_BOOT_GUIDE.md`](SECURE_BOOT_GUIDE.md)
- 设计、测试、使用和 ST-Link 救援说明：[`AB_BOOTLOADER_REPORT.md`](AB_BOOTLOADER_REPORT.md)
- 初始及最终安全基线：[`SAFETY_BASELINE.md`](SAFETY_BASELINE.md)

正式升级只写非活动 W25Q128 槽，Bootloader 本身不支持在线自更新。

当前 Bootloader 为 `v3.2.0-antirob-dev/prod`。它保留原有2 Mbps、USART3 DMA、
8 KiB流水传输和W25Q SPI DMA，同时把XBIN升级为签名格式v2：Bootloader必须用
内置公钥通过ECDSA-P256验签，并复核应用SHA-256、CRC32和向量表后才允许安装。
Bootloader与应用共用IWDG，Trial只有在核心板级/显示初始化完成并继续稳定调度
5秒后才会确认并提高安全版本下限。普通升级禁止安装低于下限的固件；长按KEY2
可人工安装合法签名的旧版本，但不会降低下限。PROD的原始维护命令也要求KEY2。

常用命令：

```powershell
cmake --preset debug
cmake --build --preset validate-debug --parallel

cmake --preset release
cmake --build --preset validate-release --parallel

# 运行篡改镜像和Bootloader同源C验签测试
cmake --build --preset security-test-release --parallel
```

`FIRMWARE_SECURITY_VERSION`是独立于显示版本的非负整数，默认值为1。只有修复安全
问题、决定淘汰旧签名固件时才提高它；普通功能版本发布不必每次递增。

第一次DEV配置会在被Git忽略的`.keys/`下生成本机开发密钥。正式发布必须使用
`release-prod`和外部提供的密钥，不能把DEV私钥或PROD私钥提交到仓库。

命令行升级：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM18 .\build\release\WeatherClock\stm32f407_upgrade.xbin
```

端口号以Windows当前枚举为准。命令末尾可增加 `921600`，在较长或质量一般的
TTL接线上使用较保守的高速档。

只读检查当前构建是否开放工程维护接口：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --maintenance-probe COM18
```

当前实板已完成v3签名升级、伪造签名拒绝、Trial自动回退、DEV/PROD权限和IWDG
持续运行测试；板载最终状态与可恢复的512 KiB镜像记录在`SAFETY_BASELINE.md`。

每个配置的 ELF、HEX、BIN 和 XBIN 位于 `build/debug` 或 `build/release`，不会
相互覆盖。应用仍必须通过 XBIN/A-B 流程安装；不要把裸应用 BIN 当作正常升级包。
