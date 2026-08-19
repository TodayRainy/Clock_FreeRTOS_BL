# BL_W25Q_AB_VSCode 代码说明

本文面向后续维护者，说明 `v3.3.0-resume-dev/prod` 的代码边界、数据格式、
启动状态机、升级协议、掉电恢复顺序以及 VS Code/CMake/GCC/J-Link 工作流。
设计验证记录和历次性能数据分别见 `AB_BOOTLOADER_REPORT.md`、
`TRANSFER_OPTIMIZATION_REPORT.md` 与 `SAFETY_BASELINE.md`。

## 1. 项目解决的问题

STM32F407 内部 Flash 中只运行一份天气时钟应用。两颗板载 W25Q128 各自保存
一份完整 XBIN，充当外部 Slot A 和 Slot B；BL24C512 EEPROM 保存当前确认槽、
待安装槽和启动阶段。

一次正常升级遵守三个边界：

1. 上位机只擦写非活动外部槽，不直接覆盖当前内部应用；
2. 新槽完成ECDSA验签及整包CRC32/SHA-256后，Bootloader才提交待安装固件；
3. 安装到内部 Flash 时先让内部 Header 失效，最后才写回有效 Header。

因此，传输、外部写入或内部安装中断都不会把半成品当作可启动应用。Bootloader
自身不支持在线更新，始终保留 J-Link/SWD 救援入口。

## 2. 目录和职责

| 路径 | 职责 |
| --- | --- |
| `STM32F4_Bootloader/app/` | 启动入口、A/B 状态机、协议分发、镜像校验与安装 |
| `STM32F4_Bootloader/driver/` | USART3、W25Q、EEPROM、内部 Flash、按键、LED 和延时 |
| `STM32F4_Bootloader/firmware/` | STM32F4 CMSIS 和标准外设库 |
| `WeatherClock/` | FreeRTOS 天气时钟应用及试运行确认任务 |
| `shared/` | Bootloader、应用和工具共同遵守的二进制格式 |
| `SerialUpgrader/` | C# GUI/CLI 上位机和 XBIN 解析 |
| `cmake/` | GCC 工具链、链接脚本和通用段布局 |
| `.vscode/` | 构建、升级、J-Link 下载和 GDB 调试任务 |
| `tools/firmware_crypto.py` | P-256密钥、公钥Header及XBIN签名公共工具 |
| `tools/validate_images.py` | 检查向量表、地址边界、摘要、公钥ID和XBIN签名 |
| `tools/test_firmware_security.py` | 构造篡改镜像并验证拒绝路径 |
| `tools/jlink/` | 下载、只读检查、备份与硬救援脚本 |

阅读代码时建议按以下顺序：

1. `shared/firmware_image_format.h` 和 `shared/ab_boot_contract.h`；
2. `app/bootloader.c`；
3. `app/boot_state.c`、`app/firmware_slot.c`、`app/magic_header.c`；
4. `app/boot_protocol.c`；
5. `driver/w25q.c`、`driver/bl_usart.c`、`driver/bl24c512.c`；
6. `WeatherClock/app/ab_confirm.c`；
7. `SerialUpgrader/Transfer.cs`。

## 3. 硬件资源

| 资源 | 引脚/外设 | Bootloader 用途 |
| --- | --- | --- |
| USART1 | 板载 CH340 对应通道 | 日志输出，不承载升级协议 |
| USART3 | PB10/TX、PB11/RX | 上位机升级协议，复位后 115200 baud |
| USART3 RX DMA | DMA1 Stream1 Channel4 | 16 KiB 循环接收缓冲 |
| W25Q SPI | SPI1：PA5/SCK、PA6/MISO、PA7/MOSI | 外部 A/B 固件存储 |
| Slot A CS | PE13 | 第一颗 W25Q128 |
| Slot B CS | PE14 | 第二颗 W25Q128 |
| SPI1 RX DMA | DMA2 Stream0 Channel3 | W25Q 连续读取 |
| SPI1 TX DMA | DMA2 Stream3 Channel3 | 连续发送 `0xFF` 产生读取时钟 |
| EEPROM | PB6/PB7 软件 I2C | BL24C512 中的 BCR 双副本 |
| KEY2 | PC4，按下为低 | 持续按住 3 秒强制停留在 Bootloader |
| LED1 | PE9 | Bootloader 等待状态指示 |

SPI1 的 APB2 时钟为 84 MHz，分频 4 后工作在 21 MHz。跳转应用前会关闭
SPI1、USART3 及相关 DMA，应用可重新初始化这些资源。

## 4. 存储布局

### 4.1 STM32 内部 Flash

| 地址范围 | 大小 | 内容 |
| --- | ---: | --- |
| `0x08000000..0x0800BFFF` | 48 KiB | Bootloader，链接脚本禁止越界 |
| `0x0800C000..0x0800CFFF` | 4 KiB | 内部 XBIN Header 区，前 256 字节有效 |
| `0x08010000..0x0807FFFF` | 448 KiB | WeatherClock 应用 |

Bootloader 的向量表从 `0x08000000` 开始，应用向量表从 `0x08010000` 开始。
跳转前设置 `SCB->VTOR = 0x08010000`，汇编 `JumpApp` 再从应用向量表装载 MSP
和 Reset_Handler。

### 4.2 两颗 W25Q128

每颗芯片物理容量为 16 MiB，但当前 A/B 契约只使用开头 1 MiB：

```text
offset 0x000000 : 256 字节 magic_header_t
offset 0x000100 : Header 区其余填充
offset 0x001000 : 应用 BIN 数据
slot limit      : 0x100000
```

限制为 1 MiB 是项目逻辑选择，并非芯片容量限制。扩大槽尺寸时必须同步检查
`FIRMWARE_SLOT_SIZE`、上位机边界检查和文档，不能只修改一个常量。

### 4.3 BL24C512 BCR

EEPROM 中保存两份 128 字节 `boot_state_t`：

| 副本 | 地址 |
| --- | --- |
| BCR A | `0xFD00` |
| BCR B | `0xFD80` |

每次保存先增加 `sequence`，再按奇偶写另一页，并读回逐字节比较。启动时分别
校验两页的格式和 CRC32，选择 `sequence` 较大的有效记录。正在写的新页掉电时，
旧页仍保留上一条完整状态。

BCR还保存已确认的`minimum_security_version`和当前候选的
`pending_security_version`。旧BCR对应字节为`0xFFFFFFFF`，加载后按0迁移，
记录总大小和两份地址均没有变化。

## 5. XBIN 格式

`WeatherClock/scripts/gen_magic_header.py` 把应用 BIN 包装为 XBIN：

```text
XBIN = 4096 字节 Header 区 + 原始应用 BIN
```

`magic_header_t` 固定为256字节。v2保留原数据字段偏移，并加入以下安全字段：

| 字段 | 当前要求 |
| --- | --- |
| `magic` | `0x4D414749`，小端内存显示为`MAGI` |
| `format_version/header_size` | `2 / 256` |
| `hash_algorithm` | `1`，SHA-256 |
| `signature_algorithm` | `1`，ECDSA-P256/SHA-256 |
| `key_id` | 内置公钥坐标SHA-256前4字节的标识 |
| `security_version` | 独立防降级计数，位于偏移24并受签名保护 |
| `data_type` | `1`，应用固件 |
| `data_offset` | `4096` |
| `data_address` | `0x08010000` |
| `data_length` | 应用 BIN 实际长度，最大 `0x70000` |
| `data_crc32` | 标准 IEEE CRC32，覆盖应用数据 |
| `image_sha256` | 应用数据SHA-256 |
| `version` | 64字节ASCII版本字符串 |
| `signature` | 64字节原始`r || s`签名 |
| `this_address` | `0x0800C000` |
| `this_crc32` | 覆盖 Header 前 252 字节 |

`magic_header_validate_data()`校验Header CRC及所有决定内部擦除范围的字段，
`firmware_authenticate_header()`再使用构建时固化的公钥验签。两者均通过后才允许
扫描或擦写镜像。完整格式和签名规范见`SECURE_BOOT_GUIDE.md`。

## 6. Bootloader 启动流程

`app/main.c` 只完成底层时钟/外设时钟、USART1 日志、TIM6 延时和 EasyLogger
初始化，然后进入 `bootloader_main()`。

`bootloader_main()` 的顺序为：

```text
初始化 KEY2、USART3 协议、两颗 W25Q、BL24C512
        |
        +-- KEY2 连续按下 3 秒？ -------- 是 --> 留在协议循环
        |
        +-- BCR 是否处于事务阶段？ ------ 是 --> 立即推进安装/试运行/回退
        |
        +-- 3 秒内 USART3 收到数据？ ---- 是 --> 留在协议循环
        |
        +-- 校验并准备内部应用 ---------- 成功 --> 反初始化并跳转应用
        |
        +-- 无可启动固件 -----------------------> 留在 Bootloader 等待救援
```

事务阶段跳过普通串口等待，以缩短升级后自动安装时间；KEY2 判断仍优先执行，
因此异常状态下也可以用按键阻止自动擦写并进入人工调试。

## 7. A/B 状态机

状态定义在 `shared/ab_boot_contract.h`。

### 7.1 Normal

- `active_slot` 指向已经由应用确认的回退槽；
- 内部固件应与 `active_crc32` 一致；
- 上位机只允许擦写另一个槽；
- 内部固件损坏时，转入 `RollbackPending` 从活动槽恢复。

### 7.2 InstallPending

- `pending_slot` 是刚完成上传和全量 CRC 的新槽；
- 先再次校验该槽并匹配 `pending_crc32`，成功后才擦内部 Flash；
- 安装失败或中断时保持 `InstallPending`，下次复位重试同一个外部副本；
- 安装成功后保存 `Trial` 并复位。

### 7.3 Trial

- 内部固件必须匹配 `pending_crc32`；
- 每次真正跳转应用前减少一次 `attempts_left`，初始值为 3；
- 内部半成品但 pending 槽仍有效时，重新进入 `InstallPending`；
- 尝试次数耗尽或 pending 也损坏时，进入 `RollbackPending`。

### 7.4 RollbackPending

- 只接受 `active_slot + active_crc32` 指定的已确认副本；
- 重新安装成功后恢复 `Normal` 并清空 pending 字段；
- 回退源也无效时停止自动擦写，留在 Bootloader 等待串口或 SWD 救援。

正常升级的状态序列为：

```text
Normal
  -> 上位机校验新槽并保存 InstallPending
  -> Bootloader 安装后保存 Trial
  -> 应用稳定运行 5 秒并确认
  -> Normal，active_slot 切换到新槽
```

## 8. 内部安装为什么能应对中断

`firmware_slot_install()` 的固定顺序是：

1. 调用前，外部目标槽已完成Header、ECDSA、CRC32、SHA-256和向量表校验；
2. 先擦除内部 `0x0800C000` Header 区；
3. 擦除并从 W25Q 复制应用数据到 `0x08010000`；
4. 对内部应用重新计算CRC32/SHA-256并检查向量表；
5. 全部正确后，最后写入256字节有效Header；
6. 再校验内部 Header，才报告安装成功。

第 2 步之后发生任何中断，内部 Header 都是无效的。下次启动不会跳进半成品，
而会根据仍然保存在 EEPROM 的 `InstallPending` 再次安装。快照内部应用到外部槽
时也采用“先数据、最后 Header”的相同提交顺序。

## 9. 应用试运行确认

`WeatherClock/app/ab_confirm.c`是应用侧唯一需要理解BCR的模块；
`health_monitor.c`负责IWDG和核心启动健康信号。

应用启动IWDG后给予核心初始化20秒宽限。`board_init()`、`ui_init()`和欢迎页显示
成功后报告ready；Wi-Fi和互联网不作为固件可启动条件。确认任务等到ready后再
观察调度器稳定运行5秒，然后：

1. 读取并校验两份 BCR；
2. 确认当前阶段为 `Trial`；
3. 校验内部 Header CRC，并确认其CRC和安全版本均等于BCR pending字段；
4. 将 pending 槽提升为 active 槽；
5. 以`max(旧下限, pending版本)`更新下限，再清空pending并写入`Normal` BCR。

如果20秒仍未ready，监控任务停止喂狗，IWDG复位后由Trial次数决定重试或回退。
DEV调试暂停CPU时冻结IWDG；PROD运行不启用调试冻结。

## 10. 串口协议

请求和响应均采用小端字段：

```text
请求: AA | opcode | payload_length:u16 | payload | crc16:u16
响应: 55 | opcode | error | data_length:u16 | data | crc16:u16
```

请求 Payload 最大 8200 字节。每次复位从 115200 baud 开始，可通过 `SET_BAUD`
协商到 230400、460800、921600 或 2000000 baud。Bootloader 先用旧速率完整发送
ACK，再切换 USART；上位机切换后用版本查询复核链路。

主要 Opcode：

| Opcode | 名称 | 说明 |
| --- | --- | --- |
| `0x01` | INQUERY | 查询版本或 MTU |
| `0x02` | SET_BAUD | 协商当前会话波特率 |
| `0x21` | RESET | 响应后系统复位 |
| `0x22` | BOOT | 通过统一状态机准备并启动应用 |
| `0x40` | W25_INFO | 返回两颗 W25Q 的 ID、状态和 UID |
| `0x41` | W25_READ | 调试读取，单次最大 4096 字节 |
| `0x42` | W25_ERASE | 擦除允许写入的外部槽区域 |
| `0x43` | W25_PROGRAM | 写完后 ACK，用作流水最终屏障 |
| `0x44` | W25_PROGRAM_ASYNC | 请求合法后先 ACK，再执行页编程 |
| `0x48..0x4A` | EEPROM 命令 | 查询、读取和受限维护写入 |
| `0x50` | SLOT_STATUS | BCR、两个 Header 和两槽全量 CRC |
| `0x51` | SLOT_SET_ACTIVE | 初始化或维护已确认槽 |
| `0x52` | SLOT_ACTIVATE | 全量校验新槽并提交 InstallPending |
| `0x53` | SLOT_SNAPSHOT | 首次部署时快照当前内部固件 |
| `0x54` | SLOT_STATUS_FAST | 只返回 17 字节 BCR，供升级选槽 |
| `0x55` | SECURITY_STATUS | 返回安全版本下限、pending版本和KEY2授权状态 |
| `0x56` | SLOT_RESUME_SCAN | 使非活动槽Header失效并返回4 KiB CRC续传位置 |
| `0x81..0x83` | 旧内部命令 | 兼容维护；Bootloader 区禁止擦写 |

错误码还包含`DENIED`。DEV构建开放工程维护命令；PROD只有长按KEY2进入时开放
W25/EEPROM原始访问、手工槽维护和旧内部Flash命令。正常签名升级不受影响。
所有改变持久化状态
的命令都在返回 OK 前完成必要的状态保存或设置明确的后续恢复阶段。

## 11. 2 Mbps 信用 ACK 流水

上位机把固件主体按最多 8192 字节拆包。中间包使用 `0x44`：

```text
MCU 收齐第 N 包并验证协议 CRC16
  -> MCU 发送“请求已接受”ACK
  -> MCU 对 W25Q 编程第 N 包
  -> PC 同时发送第 N+1 包，USART3 RX DMA 自动接收
```

最后一包使用同步 `0x43`。它只有在当前页编程全部完成后才返回，用作“前面异步
写入已经落地”的屏障。随后 `SLOT_ACTIVATE` 再从 W25Q 读取整份固件并计算
CRC32，只有结果与上位机 XBIN 相同才提交 BCR。

这种模式不是允许 PC 无限发送；每个 ACK 都是一份发送信用。没有 ACK 时上位机
不会继续，因此 16 KiB DMA 环形缓冲不会在 W25Q 忙时被无界覆盖。

## 12. 快速状态与 SPI DMA

升级选槽只需要 BCR 中的阶段和活动槽。`SLOT_STATUS_FAST` 不读取两份旧固件，
实测把升级前状态步骤从约 0.35～0.68 秒缩短到 0.02 秒。人工 `--status` 仍使用
`SLOT_STATUS` 对两个槽做完整健康检查。新上位机遇到旧 Bootloader 不支持
`0x54` 时会自动回退到 `0x50`。

W25Q 连续读取的命令和 24 位地址仍由 CPU 发送；数据阶段同时启动两条 DMA：

- TX DMA 反复从同一个 RAM 字节读取 `0xFF`，为 SPI 主机产生时钟；
- RX DMA 把 `SPI1->DR` 连续搬入目标缓冲；
- 单次超过 65535 字节时保持片选有效并分段重装 DMA 计数器；
- 固件槽层以 4096 字节为单位增量计算现有 IEEE CRC32。

当前单槽 308 KiB 全量校验约 0.19 秒，已接近 21 MHz SPI 的线速下限。F407
硬件 CRC 与当前反射式 IEEE 数据输入约定需要额外位序适配，剩余收益不足以抵消
复杂度，因此当前仍使用已经验证的软件 CRC32。

## 13. 上位机升级顺序

`SerialUpgrader/Transfer.cs::UpgradeExternalAB()` 默认执行：

1. 115200 查询版本和 MTU；
2. 协商到默认 2 Mbps并再次查询版本；
3. 快速读取 BCR，目标槽固定为 `active_slot ^ 1`；
4. 首次部署没有 BCR 时，把内部有效应用快照到 Slot A；
5. 对XBIN应用区一次性生成4 KiB分块CRC，发送`SLOT_RESUME_SCAN`；
6. Bootloader先把非活动槽magic编程为0，再顺序扫描至首个不一致块；
7. 上位机回退到前一个32 KiB擦除边界，只擦除并发送剩余应用数据；
8. 擦除首扇区，最后写入前256字节有效Header；
9. `SLOT_ACTIVATE` 携带槽号和上位机解析出的数据CRC；
10. Bootloader全量验签并检查安全版本；正常降级返回`DENIED`；
11. 保存`InstallPending`和pending安全版本并复位。

先失效旧Header、最后提交新Header，意味着上传进程被终止时目标槽保持
`header=false`，不会成为可安装镜像。`0x56`不受支持时，上位机自动使用旧版全量
流程；`--upgrade-full`或GUI复选框也可主动禁用续传。详细协议见
`RESUMABLE_UPGRADE_GUIDE.md`。

## 14. CMake 构建与产物

需要CMake、Ninja、Python 3、OpenSSL、GNU Arm Embedded Toolchain和.NET 8。

```powershell
cmake --preset debug
cmake --build --preset validate-debug --parallel

cmake --preset release
cmake --build --preset validate-release --parallel

cmake --build --preset security-test-release --parallel

dotnet build .\SerialUpgrader\SerialUpgrader.csproj -c Release
```

主要产物位于被 Git 忽略的 `build/debug` 和 `build/release`：

```text
STM32F4_Bootloader/stm32f407.elf/.hex/.bin
WeatherClock/stm32f407.elf/.hex/.bin
WeatherClock/stm32f407_upgrade.xbin
```

`validate_images.py`会检查：

- Bootloader 和应用是否超出各自链接区；
- 初始 MSP 是否位于 SRAM 且满足对齐；
- Reset_Handler 是否是 Thumb 地址并落在镜像内；
- XBIN v2字段、Header CRC、应用CRC32和SHA-256；
- 公钥ID和ECDSA-P256签名；
- XBIN Payload 是否与刚构建的应用 BIN 逐字节相同。

DEV预设会自动创建被Git忽略的本机密钥；PROD预设必须显式提供外部公私钥。密钥
管理、迁移顺序和负向测试详见`SECURE_BOOT_GUIDE.md`。

## 15. VS Code 和 J-Link

常用任务位于 `.vscode/tasks.json`：

- `CMake: Validate Images (Debug/Release)`；
- `J-Link: Flash Bootloader (Release)`；
- `A/B: Upgrade WeatherClock Debug/Release XBIN`；
- `A/B: Read Status`；
- `J-Link: Restore Known-Good 512 KiB Image`。

调试配置位于 `.vscode/launch.json`：

- Bootloader 使用 launch，可下载 Debug ELF 并运行到 `main`；
- WeatherClock 使用 attach，不直接烧裸应用，符号来自 Debug ELF；
- 应用正式更新必须走 XBIN/A-B 流程，否则内部 Header CRC 与裸 BIN 可能不一致。

当前 J-Link 可能是第三方兼容硬件，继续使用已验证的 SEGGER 6.48b DLL，不执行
探针固件升级。如果 STM32 专用连接偶发失败，先以通用 `Cortex-M4`、SWD 50 kHz
运行 `tools/jlink/prehalt_core.jlink` 停住内核，再以 `STM32F407VE` 连接。

## 16. 救援基线

最终内部 512 KiB 镜像位于工作区根目录的 `.device-backups`，该目录故意不进入
Git，避免把设备固件和大量二进制备份推送到仓库。

当前已验证镜像：

```text
.device-backups/stm32f407_ab_fast_spidma_v2_1_0_20260818.bin
SHA256 62687A3225954BCAFA13DC741CFC8CAB2D4ED46120CAF3C36818C01B9D1BCE1D
```

恢复脚本：`tools/jlink/restore_fast_spidma_final.jlink`。恢复会覆盖 STM32 内部
512 KiB Flash，不会恢复两颗 W25Q 或 EEPROM；外部槽/BCR 的诊断应先使用
`--status`，只有内部 Bootloader/应用确实损坏时才执行整片恢复。

## 17. 修改代码时必须保持的契约

以下内容不能在单一模块中孤立修改：

- `magic_header_t`字段、大小、签名规范、地址和CRC覆盖范围；
- `boot_state_t` 字段顺序、128 字节大小、EEPROM 地址和 CRC 覆盖范围；
- Bootloader/app 链接地址与 XBIN 生成脚本；
- 协议 MTU、主机拆包上限和 USART3 DMA 缓冲容量；
- Slot A/B 与 PE13/PE14 的映射；
- 应用确认任务对 Trial/Normal 的状态转换。

修改安装或恢复代码时，至少回归：

1. Debug/Release 全部镜像校验；
2. 完整状态读取和应用跳转；
3. A 到 B、B 到 A 两个方向升级；
4. 上传中断后活动槽和 BCR 不变；
5. 安装后 Trial 被应用确认；
6. 跳转后 USART3、SPI1 和 DMA 无 Bootloader 遗留使能；
7. J-Link 仍能连接、下载和验证救援镜像。

涉及签名或Header的改动还必须运行`security-test-release`，确认同源C验签和四类
篡改镜像拒绝测试全部通过。

## 18. 当前版本实测基线

| 项目 | 结果 |
| --- | ---: |
| Bootloader版本 | `v3.3.0-resume-dev/prod` |
| Release Flash（DEV） | 28,268 / 49,152字节，57.51% |
| Release RAM | 36,072 / 131,072 字节，27.52% |
| 应用Payload | 308,868字节 |
| 内容不同的自动续传升级 | 4.36秒 |
| 强制完整重写 | 4.58秒 |
| 数据100%复用 | 0.69～0.70秒 |
| 96.7%中断后恢复 | 复用290,816字节，总计1.14秒 |
| 升级前快速状态 | 0.02 秒 |
| 单槽完整校验与提交 | 0.40秒（含SHA-256/ECDSA） |
| 最终 BCR | `Normal / active=B / pending=none / security=1/0 / sequence=191` |
| 最终 Slot A/B | `v1.4.0-260819-1353-ab`，CRC `0x45A5024D`，完整有效 |

防降级迁移、正常拒绝、人工授权、双槽恢复及三次断点续传故障注入已在实板完成；
详细边界与测试方式见`SECURE_BOOT_GUIDE.md`和`RESUMABLE_UPGRADE_GUIDE.md`。
