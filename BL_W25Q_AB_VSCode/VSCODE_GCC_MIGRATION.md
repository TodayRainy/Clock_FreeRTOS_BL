# VS Code + CMake + GCC + J-Link/GDB 迁移报告

完成日期：2026-08-18  
迁移项目：`BL_W25Q_AB_VSCode`  
来源项目：`BL_W25Q_AB`（保留原状）

## 1. 迁移结论

Bootloader、WeatherClock 和 XBIN 生成链路已经脱离 Keil 构建系统，可以在
Windows 原生 VS Code 中完成配置、增量编译、静态产物校验、J-Link 下载和
GDB 源码级调试。原 Keil 工程仍保留在副本中，作为交叉核对入口，不是 GCC 构建
所需依赖。

本次不是照搬 Boot2026 的整个架构。Boot2026 是 RAM 二级加载方案，而当前项目
必须保留内部 Flash 执行区、外部 W25Q128 A/B 槽、EEPROM BCR 和既有串口协议。
实际复用的是它已经验证过版本兼容性的三部分：

- STM32F407 GNU 启动文件的框架；
- FreeRTOS V10.4.3 对应的 GCC/ARM_CM4F 移植层；
- GNU 汇编格式的 `JumpApp`。

启动文件随后恢复了当前 Flash 执行架构必需的 `.data` 从 Flash 到 SRAM 的复制。
Boot2026 中被注释的复制代码不能原样用于这里，否则有初值的全局变量不会正确
初始化。Boot2026 的 RAM 链接布局、HAL/LL 初始化和强制停留 Bootloader 逻辑均
未引入。

## 2. 已配置工具

| 工具 | 当前版本/位置 |
| --- | --- |
| Arm GNU Toolchain | 15.2.1，`E:/GNU Toolchain/bin` |
| CMake | 4.2.1 |
| Ninja | 1.13.2 |
| VS Code | 1.133.0 |
| CMake Tools | 已安装 |
| Microsoft C/C++ | 已安装 |
| Cortex-Debug | 1.12.1，已安装 |
| J-Link | 6.48b，`C:/Program Files (x86)/SEGGER/JLink` |

工具已经在 Windows PATH 或工程配置中明确指定。构建和 VS Code Task 使用
`process` 任务直接启动程序，不依赖 PowerShell 的复杂字符串解析，因此没有必要
为效率迁移到 WSL2。J-Link、CH340 串口和现有 Windows 驱动也使 Windows 原生
工作流更直接。

当前 J-Link 可能是第三方兼容设备，工程继续使用已验证的 6.48b DLL，不要求升级
探针固件。

## 3. 工程结构

| 路径 | 作用 |
| --- | --- |
| `CMakeLists.txt` | 公共 CPU/ABI/告警/Debug/Release 选项和产物校验目标 |
| `CMakePresets.json` | Debug、Release、单目标和校验预设 |
| `cmake/arm-none-eabi-gcc.cmake` | 交叉编译器发现与裸机查找策略 |
| `cmake/bootloader.ld` | Bootloader `0x08000000`、48 KiB 链接布局 |
| `cmake/weatherclock.ld` | Application `0x08010000`、448 KiB 链接布局 |
| `cmake/stm32f407_sections.ld` | 向量、代码、数据、BSS、堆栈和 ELF 段权限 |
| `cmake/runtime/syscalls.c` | Newlib 最小系统调用和串口 `printf` 重定向 |
| `.vscode/` | CMake、任务、J-Link/Cortex-Debug 配置 |
| `tools/validate_images.py` | 向量表、Flash 边界和 XBIN 双 CRC 校验 |
| `tools/jlink/` | 下载、复位、备份、只读校验和整片救援脚本 |
| `tools/svd/STM32F407.svd` | Cortex-Debug 外设寄存器视图，不依赖 Keil 安装目录 |

链接 ELF 显式分为代码 `R+X`、数据 `R+W`，不再产生 RWX LOAD 段。Windows GDB
无法稳定打开包含中文的绝对 DWARF 路径，因此编译使用 `-fdebug-prefix-map` 把源码
记录为工作区相对路径；VS Code 和命令行 GDB 都能定位到真实源码。

## 4. 构建与产物

在 VS Code 中直接打开 `BL_W25Q_AB_VSCode` 文件夹。CMake Tools 会读取 Preset；
也可以在终端执行：

```powershell
cmake --preset debug
cmake --build --preset validate-debug --parallel

cmake --preset release
cmake --build --preset validate-release --parallel
```

`validate-*` 会先构建两个固件，再检查：

- Bootloader 不超过 `0x08000000..0x0800BFFF`；
- WeatherClock 不超过 `0x08010000..0x0807FFFF`；
- 初始 MSP 位于 SRAM 且 8 字节对齐；
- Reset_Handler 是位于本镜像内的 Thumb 地址；
- XBIN 类型、偏移、目标地址、长度和 Header 地址符合契约；
- XBIN Header CRC32、数据 CRC32 和 BIN 内容完全一致。

产物不会跨配置覆盖：

| 配置 | Bootloader | WeatherClock / XBIN |
| --- | --- | --- |
| Debug | `build/debug/STM32F4_Bootloader/` | `build/debug/WeatherClock/` |
| Release | `build/release/STM32F4_Bootloader/` | `build/release/WeatherClock/` |

每个目录包含 `stm32f407.elf/.map/.hex/.bin`，WeatherClock 目录额外包含
`stm32f407_upgrade.xbin`。旧的 `WeatherClock/generated` 只服务于原 Keil/手工
生成流程；CMake 升级任务不读取它。

最终 Release 静态结果：

| 固件 | Flash | RAM（含 BSS/预留堆栈） | 向量复位地址 |
| --- | ---: | ---: | --- |
| Bootloader | 20364 B / 48 KiB（41.43%） | 17184 B / 128 KiB（13.11%） | `0x0800204D` |
| WeatherClock | 308332 B / 448 KiB（67.21%） | 115528 B / 128 KiB（88.14%） | `0x08013201` |

WeatherClock 的 RAM 余量约 15.5 KiB，后续增加大数组或任务栈时必须继续观察
链接器的 Memory Usage；这不是本次迁移新增的占用，但已经接近需要主动管理的范围。

## 5. VS Code 任务

`Terminal -> Run Task` 中已经提供：

- Debug/Release 全量构建；
- Bootloader 或 WeatherClock 单独构建；
- Debug/Release 完整镜像校验；
- 通过 J-Link 下载 Release Bootloader 并回读校验；
- 通过 USART3 把 Debug 或 Release XBIN 写入非活动 A/B 槽；
- 读取 A/B 状态；
- 使用最终 512 KiB 镜像做 J-Link 硬救援。

串口任务会询问端口号，当前 USART3 USB-TTL 是 COM18，但应以 Windows 实时枚举
为准。命令行任务使用 `dotnet SerialUpgrader.dll`，而不是直接启动 WinExe；这样
VS Code 会等待整个擦写过程结束，不会在升级尚未完成时提前结束任务。

## 6. GDB 调试方式

`.vscode/launch.json` 有两个入口：

1. `J-Link: Bootloader (flash + debug)`：构建 Debug Bootloader、用 J-Link 下载
   ELF，在 `main` 停住。它会改写内部 Bootloader 区，但不会擦写外部槽；整片救援
   镜像始终可用。
2. `J-Link: WeatherClock (attach, no flash)`：只附加并加载符号，不直接下载裸应用。
   调试前必须先执行 `A/B: Upgrade WeatherClock Debug XBIN`，让板上代码与 Debug
   ELF 完全一致，然后再附加。直接下载裸应用会绕过内部 Header/BCR 契约，也可能
   被 Bootloader 在下次复位时重新安装，因此没有提供这种易误用的配置。

WeatherClock 配置启用了 Cortex-Debug 的 FreeRTOS 线程视图和 STM32F407 SVD 外设
视图。探针序列号固定为 `69408257`，更换探针时修改 `launch.json`。

实测 GDB 链路：

- 附加 WeatherClock 后读到 PC `0x08013CB6`、PSP `0x20005A40`，向量表首两字为
  `0x20020000 / 0x08013201`，调用栈位于 FreeRTOS Idle Task；
- 对 Bootloader 执行 reset 后在 `bootloader_main()` 成功断下，PC
  `0x080008E4`，源码和调用栈可见；
- 两次测试都未通过 GDB 写入应用区，测试结束后已复位并恢复正常运行。

## 7. 实板回归结果

测试顺序刻意保留恢复余量：先用旧 Bootloader 把 GCC Release 应用写入非活动
Slot B；应用成功确认后，才替换 Bootloader。

| 检查 | 结果 |
| --- | --- |
| GCC 应用外部写入 | 只擦写非活动 Slot B，完整 CRC 校验通过 |
| 内部安装/Trial | 安装成功，FreeRTOS 启动 5 秒后确认 |
| 最终 A/B 状态 | `Normal`，active B，sequence 35 |
| Slot A | 原已确认 ARMCC 固件，CRC32 `0x49F1DEA4`，仍完整有效 |
| Slot B | GCC `v1.4.0-260818-1749-ab`，CRC32 `0x7AD86F2A`，完整有效 |
| GCC Bootloader | `v1.3.0-ab-gcc`，USART3 协议和原 BCR 完全兼容 |
| 板载器件 | W25 A/B 均 `EF4018`，BL24C512 ready |
| 应用日志 | ESP-AT 初始化成功，发送 SSID `AAAAA` |
| GDB | WeatherClock 附加和 Bootloader 断点均成功 |

当前 `build/release` 是实测之后重新生成的 17:53 版本，静态校验通过，但板上 Slot B
仍是已经实测确认的 17:49 版本。二者业务代码相同；由于日志使用 `__DATE__`/
`__TIME__`，每次重建的 BIN CRC 会变化。若要调试当前板上代码，应重新通过 A/B
部署对应 Debug XBIN，不能拿新 ELF 强行解释旧固件。

## 8. 当前 Release 校验值

| 文件 | SHA-256 |
| --- | --- |
| Bootloader BIN | `1F171219C3B1581D64E8EFC7CD0C2C0FB5711DAA67006CA524120A96F19CFD5B` |
| WeatherClock BIN | `DEEECEDFE55E750B8C7FB5ABD8056993A6D51243521E1643EBD549FD8AA99E76` |
| WeatherClock XBIN | `DD00F3A45B8E0E5108B2A661DEEB4A6FEFF54FBCA8FF88B2973F5B6D0275D278` |

当前 Release XBIN 为 `v1.4.0-260818-1753-ab`，数据长度 308332 字节，数据 CRC32
`0x74406F8F`，Header CRC32 `0xF4FAEB5D`。

## 9. 救援

最终整片镜像：

`../.device-backups/stm32f407_vscode_gcc_final_20260818.bin`

- 长度：524288 字节；
- SHA-256：`E3276FCD9F75A3B53DF863DA97EF57490DF0E8D3983FA1EAC28616FD1883C793`；
- 已用当前 J-Link 对 `0x08000000..0x0807FFFF` 做全量只读 `verifybin`；
- VS Code 任务 `J-Link: Restore Known-Good 512 KiB Image` 可写回并再次校验。

恢复任务会覆盖内部整片 Flash，只在 Bootloader 无法启动或手工误写时使用。SWD
仍开启、RDP Level 0、Bootloader 扇区未设 WRP，因此 J-Link 和保留的 ST-Link 都
能执行硬救援。外部 W25 A/B 固件不在该 512 KiB 镜像内，不会被整片内部恢复任务
擦除。
