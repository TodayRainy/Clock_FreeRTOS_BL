# STM32 天气时钟外部 A/B Bootloader 完工报告

完成日期：2026-08-18  
目标硬件：STM32F407VET6、两颗 W25Q128、BL24C512、J-Link、USART3 USB-TTL  
当前项目目录：`BL_W25Q_AB_VSCode`（由 `BL_W25Q_AB` 迁移并继续演进）  
原项目目录：`BL`（保留原状，本次实现没有覆盖它）

> 本文主体保留最初A/B实现和重构阶段的设计、故障注入记录；VS Code/GCC迁移
> 及后续传输优化的最新结果见 `TRANSFER_OPTIMIZATION_REPORT.md` 和
> `SAFETY_BASELINE.md`。v3固件签名、IWDG和DEV/PROD权限是后续独立迭代，见
> `SECURE_BOOT_GUIDE.md`；本文中的v2.1板载数值作为迁移前历史基线保留，当前
> v3板载状态和最终救援镜像以`SAFETY_BASELINE.md`为准。

## 1. 结论

外部 A/B 固件存储、内部 Flash 安装、试运行确认和自动回退已经实现。2026-08-18
又完成了一轮以“状态机与协议分离、共享持久化格式、减少重复读取和明确错误返回”
为目标的重构，并在实板上重新完成双向升级、异常状态恢复、安装中断恢复、活动槽
写保护和最终版本启动测试。

当前板载状态：

| 项目 | 当前值 |
| --- | --- |
| Bootloader | `v3.0.0-signed-dev` |
| 状态 | `Normal`，BCR sequence = 146 |
| 活动/已确认槽 | Slot A |
| Slot A | `v1.4.0-260818-2038-ab`，CRC32 `0x8AB683DA`，签名及数据完整有效 |
| Slot B | `v1.4.0-260818-2038-ab`，CRC32 `0x8AB683DA`，签名及数据完整有效 |
| 内部正在运行的应用 | 与已确认 Slot A 一致 |

这套设计保证一次普通应用升级失败不会覆盖 Bootloader，也不会同时破坏已确认
固件。最坏情况下仍可通过 SWD/J-Link 或 ST-Link 写回整片救援镜像。RDP 为 Level 0，
WRP0～WRP11 均关闭，当前板具备硬救援条件。

## 2. 存储布局

### 2.1 STM32 内部 Flash

| 区域 | 地址 | 长度 | 用途 |
| --- | --- | --- | --- |
| Bootloader | `0x08000000`～`0x0800BFFF` | 48 KiB | 永久启动入口、升级与恢复逻辑 |
| XBIN Header 区 | `0x0800C000`～`0x0800CFFF` | 4 KiB | 256 字节有效头及预留空间 |
| Application | `0x08010000`～`0x0807FFFF` | 最大 448 KiB | 唯一执行区 |

STM32 不直接从外部 SPI Flash 执行。每次切换槽位时，由 Bootloader 把目标槽
安装到内部 Application 区，然后从 `0x08010000` 启动。

### 2.2 外部 Flash

| 逻辑槽 | 芯片 | SPI/片选 | 本方案使用范围 |
| --- | --- | --- | --- |
| Slot A | U31，W25Q128 | SPI1，CS = PE13 | 芯片起始 1 MiB |
| Slot B | U32，W25Q128 | SPI1，CS = PE14 | 芯片起始 1 MiB |

SPI1 引脚为 PA5/SCK、PA6/MISO、PA7/MOSI。两颗芯片实测 JEDEC ID 均为
`EF4018`。每颗实际容量为 16 MiB，本方案只限制使用前 1 MiB，余下空间没有
被升级流程占用。

### 2.3 EEPROM 状态记录

BL24C512 使用 PB6/SCL、PB7/SDA，7 位地址 `0x50`。A0/A1/A2/WP 均接地。

| 地址 | 大小 | 内容 |
| --- | --- | --- |
| `0xFD00` | 128 字节 | BCR 副本 A |
| `0xFD80` | 128 字节 | BCR 副本 B |

两条记录交替写入。每条包含魔数、递增 sequence、阶段、活动槽、待安装槽、
剩余试运行次数、两个 CRC32 和整条记录 CRC32。上电时选择 CRC 有效且 sequence
更新的记录，因此写 EEPROM 时掉电仍可使用上一条记录。

## 3. 状态机

| 阶段 | 含义 | 下一步 |
| --- | --- | --- |
| `Normal` | 内部应用与 active slot 已确认 | 正常启动；收到新版本后进入 `InstallPending` |
| `InstallPending` | pending slot 已完整校验，等待/正在安装 | 安装成功后进入 `Trial` 并复位 |
| `Trial` | 新固件试运行，最多 3 次启动 | 应用确认后回到 `Normal`；次数耗尽后进入 `RollbackPending` |
| `RollbackPending` | 需要恢复已确认 active slot | 重装 active slot，成功后回到 `Normal` |

应用启动 5 秒后执行一次确认任务。确认只要求 FreeRTOS 调度器和应用基本启动
正常，不依赖 Wi-Fi 联网成功；这样网络故障不会被误判为固件启动失败。

三次试运行次数在每次跳转应用前递减。如果应用持续在确认前复位，第四次进入
Bootloader 时会自动安装原 active slot。

## 4. 掉电安全顺序

### 4.1 上位机写外部非活动槽

1. 查询 BCR，确定活动槽。
2. 只擦除非活动槽；Bootloader 拒绝对活动槽执行 W25 擦写命令。
3. 先写 XBIN 的 `256..end`，最后写开头 256 字节有效头。
4. Bootloader 从外部 Flash 重新读取全部数据并计算 CRC32。
5. 只有完整校验成功才写入 `InstallPending`。

在步骤 2～4 掉电只会留下无有效头或 CRC 错误的非活动槽；内部应用和活动槽
不变。

### 4.2 外部槽安装到内部 Flash

1. 安装前再次完整校验外部槽。
2. 先擦除内部 Header 区，使半成品应用必然无效。
3. 擦除并写入 Application 数据。
4. 对内部数据计算 CRC32。
5. 最后写入 256 字节有效 Header，作为提交点。
6. 保存 `Trial` 状态后复位。

步骤 2～5 任意时刻中断，BCR 仍为 `InstallPending`。下次启动不会跳入半成品，
而是从完整外部槽重新安装。即使在 Header 已提交、`Trial` 尚未保存时掉电，
下次也只是再次安装同一份 pending 固件。

### 4.3 试运行和回退

应用通过交替 BCR 页把 `Trial` 提交为 `Normal`。确认前掉电会继续消耗试运行
次数，连续三次未确认后重装旧 active slot。回退安装也保留
`RollbackPending`，中断后会自动重试。

## 5. XBIN 约束

最终生成文件：`WeatherClock/generated/stm32f407_upgrade.xbin`

| 字段 | 值 |
| --- | --- |
| XBIN 总长度 | 299624 字节 |
| Header 实际结构 | 前 256 字节 |
| Application 文件偏移 | 4096 |
| Application 地址 | `0x08010000` |
| Application 长度 | 295528 字节 |
| 固件类型 | 1（Application） |
| Header 地址字段 | `0x0800C000` |
| 版本 | `v1.4.0-260818-1715-ab`（构建产物；板上两槽为同数据的 1702 版头） |
| Application CRC32 | `0x49F1DEA4` |
| XBIN SHA-256 | `F8D2E58E8BD2E4426A99F9CB124A581F490CC69CDFE9B928DB667DDD20CFAF39` |

生成器、Windows 上位机和 Bootloader 都校验类型、偏移、目标地址、长度、头部
CRC32 和数据 CRC32。最大 Application 长度为 `0x70000`。

## 6. 代码组成

### 6.1 Bootloader

| 文件 | 职责 |
| --- | --- |
| `STM32F4_Bootloader/app/bootloader.c` | 启动入口、进入条件、A/B 状态机与应用跳转 |
| `STM32F4_Bootloader/app/boot_protocol.c/.h` | USART3 帧解析、命令分发、响应与写保护策略 |
| `app/boot_state.c/.h` | 双页 BCR 读取、CRC 与原子式轮换保存 |
| `app/firmware_slot.c/.h` | 外部槽校验、内部安装、首次固件快照 |
| `app/magic_header.c/.h` | XBIN Header 严格校验 |
| `driver/w25q.c/.h` | 两颗 W25Q128 的 SPI1 驱动 |
| `driver/bl24c512.c/.h` | BL24C512 软件 I2C 驱动 |
| `driver/stm32_flash.c/.h` | 内部 Flash 擦写与错误返回 |
| `driver/crc32.c/.h` | 一次性和增量 CRC32 |
| `shared/ab_boot_contract.h` | Bootloader 与应用共享的 BCR 持久化格式 |
| `shared/firmware_image_format.h` | Bootloader 与应用共享的 XBIN Header 格式 |

Bootloader 的在线协议不能擦写 `0x08000000`～`0x0800BFFF`。Bootloader 自身
升级只允许通过 J-Link 或 ST-Link 进行。

### 6.2 WeatherClock

| 文件 | 职责 |
| --- | --- |
| `WeatherClock/app/ab_confirm.c/.h` | 启动 5 秒后确认 Trial 固件 |
| `WeatherClock/driver/bl24c512/` | 应用侧 EEPROM 驱动 |
| `WeatherClock/app/main.c` | 启动确认任务并输出版本 |
| `WeatherClock/scripts/gen_magic_header.py` | 构建后生成 XBIN |

`AB_CONFIRM_ENABLED` 默认为 1。测试回退时曾临时关闭并单独保存测试 XBIN，最终
工程已恢复为开启状态。

### 6.3 SerialUpgrader

上位机保留 WinForms 界面，并新增同一代码路径的命令行模式。普通升级不再直接
擦 STM32 内部 Flash，而是写非活动 W25 槽并提交 `InstallPending`。

常用命令：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM18 .\WeatherClock\generated\stm32f407_upgrade.xbin

dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --status COM18
```

`--status` 读取完成后会主动发送 BOOT 命令，不会把设备一直留在 Bootloader。

## 7. 扩展串口协议

请求帧沿用 `AA + opcode + payload_length + payload + CRC16`，响应帧头为 `55`。

| Opcode | 名称 | 用途 |
| --- | --- | --- |
| `0x40` | W25_INFO | 读取两颗 Flash 的 ID、状态和 UID |
| `0x41` | W25_READ | 调试读取外部 Flash |
| `0x42` | W25_ERASE | 擦除非活动槽 |
| `0x43` | W25_PROGRAM | 同步写入非活动槽，写完后应答 |
| `0x44` | W25_PROGRAM_ASYNC | 流水中间块，校验请求后先应答再写入 |
| `0x48` | EEPROM_INFO | 检测 BL24C512 |
| `0x49` | EEPROM_READ | 调试读取 EEPROM |
| `0x4A` | EEPROM_WRITE | 只允许写 `0xFC00` 以上保留区 |
| `0x50` | SLOT_STATUS | 返回 BCR 和两个槽的完整校验结果 |
| `0x51` | SLOT_SET_ACTIVE | 初始化/维护已确认槽 |
| `0x52` | SLOT_ACTIVATE | 校验目标槽并提交试运行 |
| `0x53` | SLOT_SNAPSHOT | 把当前有效内部固件快照到外部槽 |
| `0x54` | SLOT_STATUS_FAST | 只返回17字节BCR，升级选槽时不扫描固件数据 |

旧的内部 `ERASE/PROGRAM/VERIFY` 调试命令仍保留，但前 48 KiB 被保护，正常 GUI
升级流程不会调用它们。

## 8. 实板测试结果

| 测试 | 方法 | 结果 |
| --- | --- | --- |
| Bootloader 最终构建 | Keil ARMCC 5.06u7 | 0 error，0 warning；Code 18578，RO 2622，RW 124，ZI 16156 |
| WeatherClock 最终构建 | Keil ARMCC 5.06u7 | 0 error，0 warning；Code 33944，RO 261320，RW 264，ZI 110400 |
| SerialUpgrader Release | .NET 8 | 0 error，0 warning |
| W25 探测 | 读取 ID/状态/UID | U31/U32 均为 `EF4018` |
| W25 擦写 | U32 尾部测试扇区写 700 字节、回读、擦除 | 数据一致，擦后全 `FF` |
| EEPROM 擦写 | 备用页写入、回读、恢复 | 数据一致并恢复原内容 |
| 初始快照 | 旧内部天气时钟快照到 Slot A | 全量 CRC 通过 |
| 正常升级 | A → B，应用 5 秒后确认 | 进入 `Normal`，active = B |
| 自动回退 | 写入不确认测试固件，连续 3 次在确认前复位 | 第 4 次启动自动恢复旧固件 |
| 安装中断 | 内部安装时通过 ST-Link 复位/停机 | Header 读为 `FFFFFFFF`，BCR 保持 pending；恢复运行后重装成功 |
| BCR 全失 | 备份后把两个 BCR 页置 `FF` | 上位机自动快照当前内部固件并完成首次 A/B 初始化 |
| 重构后双向升级 | C# Release 上位机执行 B→A、A→B、B→A | 外部校验、安装、Trial、应用确认均通过 |
| 活动槽保护 | 在 Normal 和 InstallPending 阶段尝试写活动槽 | 均返回参数错误，A/B 固件保持完整 |
| 待安装 CRC 错误 | 注入结构有效但 pending CRC 故意错误的 InstallPending BCR | seq 22 自动进入回退，最终 `Normal` seq 24 |
| Trial 次数耗尽 | 注入 attempts=0 的结构有效 Trial BCR | seq 25 自动回退，最终 `Normal` seq 27 |
| 重构后安装中断 | 内部安装阶段使用 J-Link 停机/复位 | 再上电自动重装成功，最终 `Normal`、active A、seq 31 |
| 最终状态读取 | `--status COM18` | Bootloader `v1.2.0-ab`，`Normal`、active A、CRC `49F1DEA4`；A/B 均完整 |
| 最终应用运行 | COM15 复位后读取日志 | W25/EEPROM 探测正常，ESP AT 初始化正常，并发送 SSID `AAAAA` |
| Bootloader 一致性 | J-Link `verifybin` 及最终整片回读比较 | 21324 字节逐字节一致 |

重构后的安装中断测试覆盖了 CPU 在危险写入阶段被停机/复位后的恢复路径。由于
STM32 正在执行内部 Flash 操作时 J-Link 无法稳定 Halt，本轮没有捕获到中断瞬间
Header 的 `FFFFFFFF` 中间态；恢复后的状态和固件完整性已经核验。尚未做真实拔电
和可控棕断电压测试，这是后续验证电源完整性与 SPI/EEPROM 边界的最高价值测试。

## 9. 构建和使用流程

### 9.1 构建

1. 用 Keil 打开 `STM32F4_Bootloader/mdk/stm32f407.uvprojx`，执行 Rebuild。
2. 用 Keil 打开 `WeatherClock/mdk/stm32f407.uvprojx`，执行 Rebuild。
3. WeatherClock 的 After Build 会输出 BIN，并调用 Python 生成
   `WeatherClock/generated/stm32f407_upgrade.xbin`。
4. 在 `SerialUpgrader` 下执行 `dotnet build -c Release`。

两个 Keil 工程现已改用 `J-LINK / J-TRACE Cortex`，接口为 SWD 1 MHz，Flash
Algorithm 为 `STM32F4xx_512.FLM`。Bootloader 工程允许调试前自动下载；
WeatherClock 工程关闭自动下载，避免裸应用被直接写入后与内部 XBIN Header 的
CRC32 不一致。调试 WeatherClock 时应先通过 A/B XBIN 安装对应版本，再附加调试
并加载 AXF 符号。

修改正式版本号时，需要同步检查：

- `WeatherClock/scripts/gen_magic_header.py` 中的 XBIN 版本；
- `WeatherClock/app/app.h` 中应用控制台版本；
- `STM32F4_Bootloader/app/boot_protocol.c` 中 Bootloader 版本。

### 9.2 进入 Bootloader

Bootloader 升级串口是 USART3：PB10/TX 接 USB-TTL RX，PB11/RX 接 USB-TTL TX，
两端共地，使用 3.3 V TTL 电平。本次重新枚举后的实测端口为 COM18；拔插后端口号
可能再次改变，应以上位机实时枚举结果为准。

可以持续按住 KEY2（代码按 PC4 低有效处理）并复位，保持约 3 秒后松开；也可以
先复位，然后在 3 秒串口接收窗口内启动上位机命令。应用控制台为板载 USART1，
本次端口为 COM15。

天气时钟应用会把 PB10/PB11 重新配置为 AHT20 的 I2C2。升级完成后应断开或确保
USB-TTL 的 TX/RX 为高阻，尤其不能让 USB-TTL TX 持续推挽驱动 PB11，否则会与
I2C SDA 冲突。

### 9.3 正常升级

1. 进入 Bootloader。
2. 在 WinForms 选择当前 USB-TTL 端口和 XBIN 后开始升级，或使用 `--upgrade`。
3. 上位机写入并校验非活动槽，然后提交试运行。
4. Bootloader 安装并启动新应用。
5. 应用运行 5 秒后确认；下次读取状态应为 `Normal`。

首次使用且 EEPROM 没有 BCR 时，上位机会先把当前有效内部应用快照到 Slot A，
再把新版本写入 Slot B。因此首次部署前，内部必须已经有一份 Header 和数据 CRC
都有效的天气时钟应用。本板已完成这一步。

## 10. 救援方案

### 10.1 首选：Bootloader 内恢复

- 进入 Bootloader 后用 `--status` 检查两个槽。
- 如果 Trial 连续失败，不做人工操作也会在三次后回退。
- 如果内部 Header/CRC 无效，Bootloader 会从已确认 active slot 重装。
- 如果 BCR 全失且内部应用也无效，Bootloader 会扫描两个完整外部槽并恢复首个
  可用槽。

### 10.2 硬救援：J-Link 或 ST-Link

最终整片镜像：

`../.device-backups/stm32f407_ab_refactored_v1_2_20260818.bin`

在仓库根目录执行：

```powershell
& 'E:\STM32CubeProgrammer\bin\STM32_Programmer_CLI.exe' `
  -c port=SWD mode=UR `
  -d '.\.device-backups\stm32f407_ab_v1_4_aaaaa_20260818.bin' 0x08000000 `
  -v -rst
```

该命令会覆盖 STM32 整个 512 KiB 内部 Flash，应只在救援时使用。若只恢复
Bootloader，可下载
`STM32F4_Bootloader/mdk/Objects/stm32f407.bin` 到 `0x08000000`。

Bootloader 不在线自更新、SWD 未关闭、RDP 为 Level 0，并保存了升级前和最终
整片镜像，因此软件升级故障具备明确的恢复路径。若 Bootloader 区因手工 ST-Link
误写而损坏，仍可通过 `mode=UR` 在复位下重新连接并恢复。

当前也已使用 J-Link 对上述最终整片镜像完成 512 KiB 全量只读校验。J-Link 可用
`loadfile <bin>, 0x08000000` 写回同一镜像；ST-Link 仍建议保留为独立的第二救援
工具。

## 11. 备份与产物校验值

| 文件 | SHA-256 |
| --- | --- |
| 升级前内部整片 `stm32f407_before_ab_20260818.bin` | `0F0856AB11F3277027A7A63DDA8AADCBE2585E2E820815101B423D9706D416A8` |
| 上一版内部整片 `stm32f407_ab_final_20260818.bin` | `607A933D793A2899168A2F076A9D56C57DC6F127017DAF6291288B59BCBEAF62` |
| 重构前内部整片 `stm32f407_ab_v1_4_aaaaa_20260818.bin` | `DED6E004E1DA3E7DEFB3FB9FE6F6B9643B9793AD8F6201F1E499CDD862F36C3D` |
| 当前内部整片 `stm32f407_ab_refactored_v1_2_20260818.bin` | `4F59A817F0ACF2A00AE69075FFB4EACF5B49186D98C2EB2EF1FE62218E2CD1A9` |
| Bootloader BIN | `B944254B519C5F209A567726F4D495D55938C90B8F13DBECFB5E7D1CF47F6337` |
| WeatherClock BIN | `B4C75439E51C54E1A1F73055BF4A6EE0D7A485B55E01854A37205FD8A4755E4E` |
| 最终 XBIN | `F8D2E58E8BD2E4426A99F9CB124A581F490CC69CDFE9B928DB667DDD20CFAF39` |
| SerialUpgrader Release DLL | `5654E8AC23185F8FD33BE88C0D92C572B05AA100B135DCFCB73B28BB3C6724E4` |
| U31 升级前首 1 MiB | `EA653225F12386792650C726075BBFF6003A4730AA66FE543C9235303267DC60` |
| U32 升级前首 1 MiB | `EA653225F12386792650C726075BBFF6003A4730AA66FE543C9235303267DC60` |
| EEPROM 升级前最后 1 KiB | `D55774F938D1F1524951F66CFC3214A3C136581F614C0E6BDA71C5974EF54511` |

## 12. 已知边界

- 本报告对应的v2.1板载基线只使用CRC32。当前主线v3.3已经加入ECDSA-P256、
  SHA-256和EEPROM双副本安全版本下限；固件仍未加密，详见`SECURE_BOOT_GUIDE.md`。
- 主线v3的应用确认已增加核心板级/显示ready信号、IWDG和额外5秒稳定窗口，但
  仍不是覆盖所有业务任务的心跳系统；网络故障被有意排除在回退条件之外。
- 外部两槽各限制 1 MiB，已明显大于内部 448 KiB 应用上限；剩余 W25 空间目前未
  建立分区表。
- 没有启用 Bootloader 扇区硬件写保护，原因是当前仍处于开发期并需要 ST-Link
  迭代。在线协议已经做了软件保护；待 Bootloader 长期稳定后再考虑 Option Bytes
  WRP，开启前必须保留并验证解除写保护流程。

## 13. 2026-08-18 重构说明

本轮没有改变外部协议的帧格式、Opcode 或外部存储布局，因此旧上位机与已有 A/B
数据保持兼容。主要结构调整如下：

1. 将 1123 行的 `bootloader.c` 拆成启动/状态机和 `boot_protocol.c` 两个明确模块；
   BOOT 命令不再绕过恢复状态机，而是请求顶层走统一的应用准备路径。
2. BCR 和 XBIN Header 定义移到 `shared/`，Bootloader 与 WeatherClock 不再各自
   维护容易漂移的结构副本，并通过编译期数组断言固定为 128/256 字节。
3. 外部槽校验拆为“读头”“数据 CRC”“完整校验”三个直接步骤，安装过程复用已读
   Header，去掉重复 SPI 读取；内部 Flash 只擦除实际固件长度覆盖的扇区。
4. `InstallPending`、`Trial`、`RollbackPending` 对 BCR 记录的 CRC 与槽内 Header
   CRC 做一致性核对；异常 pending 转回已确认槽，回退源无效时留在 Bootloader
   等待救援，不跳转到不确定应用。
5. USART 接收回调在外设使能前注册，移除未启用 DMA 的大段注释代码；旧内部 Flash
   调试命令开始检查实际擦写返回值，错误可通过协议反馈给上位机。
6. 上位机使用强类型 `BootPhase`，只允许从 `Normal` 发起普通 A/B 升级，并去掉旧
   PROGRAM 路径逐片 LINQ 分配；公共接口补齐 XML/Doxygen 风格说明。
7. Bootloader、共享格式、Flash/W25Q/EEPROM/延时驱动的公共头文件均补充 Doxygen，
   状态提交点、Header 最后写入等关键掉电语义保留必要行内注释，没有增加与个人
   项目规模不相称的抽象层。

重构前源码快照为 `../tmp/BL_W25Q_AB_before_refactor_20260818_165232.zip`，SHA-256
为 `9CDA052A27701B489DEB80A1B467E3538A09E69C0B3ED001FFE214C267C19085`；重构前板上
512 KiB 镜像为 `../tmp/stm32_ab_before_refactor_20260818_170456.bin`，SHA-256 为
`DED6E004E1DA3E7DEFB3FB9FE6F6B9643B9793AD8F6201F1E499CDD862F36C3D`。
