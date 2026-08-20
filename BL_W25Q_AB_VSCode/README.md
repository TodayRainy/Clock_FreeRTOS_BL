# Clock STM32F407 Formal A/B Bootloader

本目录是天气时钟项目当前主线：STM32F407、双 W25Q128、BL24C512、ESP-AT、
FreeRTOS、USART3 上位机和腾讯云 COS OTA 共同组成一套可回退的固件升级系统。

当前固件容器统一称为 **XBIN Formal revision 1**。它只保留一种形态：

- ECDSA-P256 签名为必选；
- AES-256-GCM 加密为必选；
- 320 字节统一 Header；
- 外部 W25Q 保存密文，STM32 内部 Flash 保存认证后的明文应用；
- 不接受旧 XBIN，也不再提供内部固件外部快照命令。

Formal 是产品线名称，不使用“v4”作为格式名称。Header 中仍保留数值型
`format_revision=1`，用于严格解析二进制布局。

## 当前基线

| 项目 | 当前值 |
|---|---|
| Bootloader | `formal-1.0.0-dev` |
| WeatherClock | `formal-1.0.0` |
| XBIN | Formal revision 1 |
| release sequence | 4 |
| security version | 2 |
| 签名 Key ID | `0xCA9EEE70` |
| AES Key ID | `0x9B5260DC` |
| Bootloader Flash | 34,800 / 49,152 字节，70.80% |
| WeatherClock Flash | 158,024 / 458,752 字节，34.45% |
| 公网 Manifest | `https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/manifest.json` |

2026-08-20 的实板最终状态为 `Normal / active=B / pending=none / seq=320`，
内部应用与 A/B 两槽均通过 Formal Header、ECDSA、AES-GCM、CRC32、SHA-256 和
向量表检查。完整 USART3 重写的 W25 编程耗时 2.11 秒，总提交耗时 2.72 秒；
相同镜像断点复用 158,024 字节，总提交耗时 0.81 秒。

## 工程组成

- `STM32F4_Bootloader/`：启动决策、A/B 状态机、验签、解密、安装、回退和协议；
- `WeatherClock/`：FreeRTOS 天气时钟、联网、COS OTA 下载和试运行确认；
- `SerialUpgrader/`：Windows USART3 上位机与命令行升级工具；
- `shared/`：Formal Header、BCR、OTA 请求和安全策略的跨固件契约；
- `tools/`：打包、Manifest、主机负向测试和 J-Link 救援脚本；
- `keys/`：本私人学习项目使用的 Formal 签名与加密密钥；
- `ota/manifest.json`：当前可发布的 Formal OTA 清单。

核心说明：

- [CODE_GUIDE.md](CODE_GUIDE.md)：代码边界和完整运行流程；
- [ENCRYPTED_FIRMWARE_GUIDE.md](ENCRYPTED_FIRMWARE_GUIDE.md)：Formal 签名与加密设计；
- [OTA_GUIDE.md](OTA_GUIDE.md)：COS OTA 下载、提交和发布顺序；
- [RESUMABLE_UPGRADE_GUIDE.md](RESUMABLE_UPGRADE_GUIDE.md)：USART3 断点续传；
- [SECURE_BOOT_GUIDE.md](SECURE_BOOT_GUIDE.md)：防降级、DEV/PROD 和信任边界；
- [SAFETY_BASELINE.md](SAFETY_BASELINE.md)：救援镜像与实板测试记录。

## 构建和验证

Windows 原生工作流需要 CMake、Ninja、`arm-none-eabi-gcc`、Python 3、主机
GCC、OpenSSL 和 .NET 8。

```powershell
cmake --preset release
cmake --build --preset release
cmake --build build/release --target validate_images --parallel 1
cmake --build build/release --target test_firmware_security --parallel 1
dotnet build SerialUpgrader/SerialUpgrader.csproj -c Release
```

关键产物：

- `build/release/STM32F4_Bootloader/stm32f407.bin`；
- `build/release/WeatherClock/stm32f407.bin`；
- `build/release/WeatherClock/stm32f407_formal_header.bin`；
- `build/release/WeatherClock/stm32f407_upgrade.xbin`；
- `SerialUpgrader/bin/Release/net8.0-windows/SerialUpgrader.exe`。

版本参数通过 CMake cache 设置，不应直接修改生成文件：

```powershell
cmake --preset release `
  -DWEATHERCLOCK_VERSION=formal-1.0.1 `
  -DFIRMWARE_RELEASE_SEQUENCE=5 `
  -DFIRMWARE_SECURITY_VERSION=2
```

普通功能发布递增 `release_sequence`。只有修复了不能再允许回退的安全问题时，
才递增 `security_version`。

## USART3 升级

上位机先以 115200 连接，再协商到 2 Mbaud。完整重写：

```powershell
dotnet SerialUpgrader/bin/Release/net8.0-windows/SerialUpgrader.dll `
  --upgrade-full COM29 build/release/WeatherClock/stm32f407_upgrade.xbin
```

允许断点复用：

```powershell
dotnet SerialUpgrader/bin/Release/net8.0-windows/SerialUpgrader.dll `
  --upgrade COM29 build/release/WeatherClock/stm32f407_upgrade.xbin
```

上位机只写非活动槽。完整 Formal 校验和 `InstallPending` 提交成功后才复位；
Bootloader 安装到内部 Flash，应用稳定运行并完成健康确认后才把新槽设为活动槽。

## 首次部署与旧设备迁移

Formal Bootloader 不再把内部应用复制到外部 Flash。首次部署应按以下顺序：

1. 保留 SWD/J-Link 和一份已验证的整片内部 Flash 备份；
2. 先把完整 Formal XBIN 放入任一外部槽；
3. 再写入 Formal Bootloader、320 字节 Header 和明文应用；
4. Formal Bootloader 验证外部槽与内部发布完全一致后，重建 BCR 并采用该槽。

若 BCR 是旧状态，但不再能引用任何 Formal Header，Bootloader 只在找到一份
签名、GCM、摘要、版本和内部应用全部匹配的外部槽时才重建关系。它不会凭空制造
回退副本，也不会覆盖现有槽。

## 救援边界

项目保持 RDP0、SWD 和 Option Bytes 不变。当前整片内部 Flash 救援镜像为：

`../.device-backups/stm32f407_formal_1_0_0_final_20260820.bin`

长度 524,288 字节，SHA-256：

`FBCB8245952C87C6333FC71CAA8A5C5DB7A9987A2573788C3855707B43BA94DF`

配套脚本位于 `tools/jlink/backup_formal_1_0_0_final.jlink`、
`restore_formal_1_0_0_final.jlink` 和 `verify_formal_1_0_0_final.jlink`。
这些脚本只读写 STM32 内部 512 KiB Flash，不修改 RDP、WRP 或其他 Option Bytes。

## 密钥说明

用户明确允许把 `keys/` 提交到私人 Git 仓库，以方便个人项目复现和救援。这不是
商业密钥管理方案：RDP0 下，能接触 SWD 或读取 Bootloader 的人仍可能取得 AES
密钥。当前加密主要防止 COS 文件、串口传输和外部 W25Q 内容被直接读取，不以
抵抗物理拆机为目标。
