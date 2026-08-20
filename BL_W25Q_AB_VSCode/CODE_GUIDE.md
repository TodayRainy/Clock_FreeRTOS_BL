# Formal A/B Bootloader 代码说明

本文描述当前 Formal 主线的代码组织和运行逻辑。设计目标是把关键状态转换写清楚，
保留必要的模块边界，但不为个人学习项目引入复杂框架或预见性防御层。

## 1. 存储布局

STM32F407 内部 512 KiB Flash：

| 地址 | 大小 | 内容 |
|---|---:|---|
| `0x08000000` | 48 KiB | Bootloader |
| `0x0800C000` | 4 KiB | Formal Header 区，实际使用前 320 字节 |
| `0x08010000` | 448 KiB | WeatherClock 明文应用 |

两颗 W25Q128 各自作为一个外部槽，槽内从地址 0 开始保存完整加密 XBIN。当前
XBIN 约 158 KiB，远小于单颗 16 MiB 容量。BL24C512 保存两份 128 字节 BCR、
OTA 请求和下载进度，BCR 地址是 `0xFD00` 与 `0xFD80`。

## 2. 共享契约

`shared/` 中的文件同时被 Bootloader、WeatherClock 或主机测试使用：

- `firmware_image_format.h`：320 字节 Formal Header；
- `ab_boot_contract.h`：A/B BCR 和四个阶段；
- `ota_contract.h`：应用交给 Bootloader 的一次性 OTA 请求与断点记录；
- `firmware_security_policy.h`：安全版本下限判断；
- `firmware_release_policy.h`：自动 OTA 的发布先后判断；
- `watchdog.c/.h`：Bootloader 与应用共用的 IWDG 最小实现。

这些文件只定义稳定的二进制数据和简单规则，不包含板级业务流程。

## 3. Bootloader 模块

### `app/bootloader.c`

负责一次启动的总决策：

1. 初始化 KEY2、USART3、W25Q 和 EEPROM；
2. KEY2 持续按住 3 秒时进入人工维护；
3. 非事务状态给 USART3 一个进入窗口；
4. 读取 BCR，验证内部应用和所需外部槽；
5. 推进安装、试运行、回退或正常启动；
6. 跳转前反初始化 W25Q、USART3、定时器和中断状态。

关键函数：

- `boot_prepare_application()`：唯一的启动决策入口；
- `handle_install_pending()`：从候选槽认证、解密并安装；
- `handle_trial()`：每次试运行前消耗一次尝试次数；
- `handle_rollback_pending()`：从已确认活动槽恢复；
- `adopt_matching_slot()`：首次 Formal 部署或旧 BCR 迁移时采用匹配槽；
- `jump_to_application()`：完成外设交接并进入 `0x08010000`。

采用逻辑不会制作内部固件快照。它先完整验证外部槽，再要求长度、CRC、SHA、
security、release、Key ID 和版本与内部 Header 一致。BCR 使用临时对象构造，
只有 EEPROM 保存和读回成功后才更新当前 RAM 状态，避免“保存失败但继续启动”。

### `app/boot_state.c`

实现 BCR 双副本：每次保存递增 `sequence`，奇偶交替写两页，读回比较后返回。
读取时选择 CRC 有效且 sequence 最大的记录。掉电最多损坏正在写的新页，上一页
仍可恢复。

四个阶段：

- `Normal`：内部应用和 `active_slot` 已确认；
- `InstallPending`：候选已提交，等待安装；
- `Trial`：新应用正在试运行；
- `RollbackPending`：下次启动恢复已确认活动槽。

### `app/firmware_slot.c`

集中处理固件数据，而不是状态决策：

- 加载并认证外部 Header；
- 流式验证外部密文；
- 验证内部 Header、CRC、SHA 和向量表；
- 擦写内部应用区；
- 以 4 KiB 块流式解密、计算摘要并写入；
- 最后提交内部 Header。

安装时 Header 最后写入，因此中途掉电不会把半写应用误认为完整固件。

### `app/magic_header.c`、`firmware_auth.c`、`firmware_decryption.c`

三者分别负责：

- Formal 固定字段、保留字段和 Header CRC；
- SHA-256 与 ECDSA-P256 验签；
- AES-256-GCM AAD、流式解密与 Tag 认证。

算法实现没有隐藏在多层接口后。Header 结构本身作为参数贯穿验证和安装，便于
对照格式文档调试。

### `app/boot_protocol.c`

USART3 协议使用请求头 `0xAA`、响应头 `0x55` 和 CRC16。当前命令：

| Opcode | 功能 |
|---:|---|
| `0x01` | 查询版本和 MTU |
| `0x02` | 协商波特率 |
| `0x21` / `0x22` | 复位 / 经状态机启动应用 |
| `0x40..0x44` | W25 信息、读、擦、同步/流水编程 |
| `0x48..0x4A` | EEPROM 工程维护接口 |
| `0x50` | BCR、内部镜像和 A/B 完整健康状态 |
| `0x51` | DEV 人工设置活动槽 |
| `0x52` | 完整校验并激活非活动槽 |
| `0x54` | 快速 BCR 状态 |
| `0x55` | 防降级状态 |
| `0x56` | 4 KiB 断点 CRC 扫描 |

协议不再包含外部快照、旧内部 Flash 擦写或旧激活参数。高速链路空闲超时后回到
115200；数据块使用 8 KiB MTU 与 DMA ACK 流水，避免每块都等待一个完整往返。

### `driver/`

- `w25q.c`：SPI1 21 MHz，双片选，DMA 连续读和页编程；
- `bl24c512.c`：PB6/PB7 软件 I2C，128 字节页写与 ACK polling；
- `bl_usart.c`：USART3 DMA 环形接收和同步发送；
- `tim_delay.c`：Bootloader 微秒计时；
- `key.c`、`led.c`、`console.c`：直接板级驱动。

跳转应用前会反初始化 SPI1、DMA、USART3 和 Bootloader 定时器，避免把隐含外设
状态交给 FreeRTOS 应用。

## 4. WeatherClock 模块

### `app/ota.c`

应用只负责联网下载，不拥有安装权限：

1. 等本地温湿度和在线天气各成功刷新一次；
2. 应用 ready 后 60 秒进行首次检查，以后每 24 小时检查；
3. 只接受 Manifest v2、`xbin_format=formal`、revision 1 和本产品；
4. 比较 release sequence 和 payload SHA，旧版、同版和冲突发布都不下载；
5. 下载到非活动 W25Q，32 KiB 保存一次进度；
6. 校验完整 XBIN 文件 SHA、Header CRC 和元数据；
7. 最后写 Header 和 OTA 请求，点亮三颗 LED；
8. 用户按 RESET 后由 Bootloader 再做完整密码学验证和安装。

OTA 下载把 ESP USART 波特率临时提高到 921600，W25Q 仍使用 21 MHz SPI；完成后
恢复正常网络参数。WeatherClock 不在下载后立即强制重启，也不会为了 OTA 重复
拉取天气。

### `app/ab_confirm.c`

只有应用核心板级初始化、显示、网络/页面健康条件和额外稳定窗口满足后，才把
`Trial` 转成 `Normal`。确认记录同样使用 BCR 双副本。若应用在确认前反复重启，
Bootloader 消耗有限尝试次数并恢复旧活动槽。

### `app/health_monitor.c`

FreeRTOS 健康任务定期刷新 IWDG。Wi-Fi 等待循环会阻塞让出 CPU，`configASSERT`
失败会停在不可返回处理函数中，不会因空转或错误返回造成周期重启。

## 5. 上位机

`SerialUpgrader/MagicHeader.cs` 只解析 Formal 320 字节 Header，并在开始传输前
检查固定字段、文件长度和 Header CRC。`Transfer.cs` 负责：

- 115200 握手与 2 Mbaud 协商；
- 查询活动槽并只写另一槽；
- 完整重写或 4 KiB CRC 断点复用；
- 32 KiB 对齐擦除规划；
- 8 KiB 流水编程；
- 完整槽校验与五字节 `slot + expected_crc` 激活；
- 状态、安全版本和内部固件健康报告。

Formal 上位机不会在“不支持断点”时回退成旧协议，也不会自动制作外部快照。若
BCR、活动槽或格式不满足要求，它会停止并要求先完成迁移或 J-Link 救援。

## 6. 启动时序

正常升级的持久化时序：

```text
上位机/OTA写非活动槽
        ↓ 完整Formal校验
InstallPending
        ↓ 认证、解密、内部Flash安装
Trial（attempts=3）
        ↓ 应用稳定运行并确认
Normal，新槽成为active
```

任何一步掉电都不会先覆盖活动外部槽：

- 写非活动槽掉电：活动槽和内部应用不变；
- 安装掉电：`InstallPending` 保留，可重装；
- Trial 未确认：次数耗尽后进入 `RollbackPending`；
- BCR 写掉电：上一份 CRC 有效记录仍在。

## 7. 构建、大小和测试

Release 使用 `-Os` 和 `--gc-sections`，没有为个别模块强制 `-O2`。当前实测：

- Bootloader 34,800 字节，占 48 KiB 的 70.80%，余 14,352 字节；
- WeatherClock 158,024 字节，占 448 KiB 的 34.45%，余 300,728 字节。

回归入口：

```powershell
cmake --build --preset release
cmake --build build/release --target validate_images --parallel 1
cmake --build build/release --target test_firmware_security --parallel 1
dotnet build SerialUpgrader/SerialUpgrader.csproj -c Release
```

实板烧录时，旧 SEGGER Commander 对中文固件路径不可靠。应把 `.bin` 临时复制到
纯 ASCII 路径再执行 `loadbin`，并单独运行 `verifybin`；本次迁移正是通过二次
逐字节校验发现并纠正了 Unicode 路径导致的误报。
