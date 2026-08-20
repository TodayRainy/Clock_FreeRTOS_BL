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

## 3. 主要调用关系

这三个入口分别覆盖启动、日常运行和升级，定位问题时先判断当前处在哪条链路：

```text
复位
 └─ Bootloader/main.c
     └─ bootloader_run()
         ├─ boot_state：读取 BCR、选择 Normal/Install/Trial/Rollback
         ├─ firmware_slot：认证外部槽、解密并安装内部应用
         ├─ boot_protocol：KEY2 或串口窗口内处理 USART3 命令
         └─ jump_to_application：反初始化后跳转 WeatherClock

WeatherClock/main.c
 └─ app_init()
     ├─ 页面任务：传感器、RTC、显示与按键
     ├─ Wi-Fi 工作任务：独占 ESP-AT 请求/响应事务
     ├─ OTA 任务：等首轮温湿度和天气成功后再检查 COS
     ├─ health_monitor：只在核心任务健康时刷新 IWDG
     └─ ab_confirm：Trial 稳定窗口满足后提交 Normal

SerialUpgrader
 └─ Transfer.UpgradeExternalAB()
     ├─ 查询 BCR、选择非活动槽
     ├─ 断点 CRC 扫描或完整擦写
     ├─ USART3 流水写入 W25Q，最后提交 Header
     └─ SLOT_ACTIVATE：由 Bootloader 完整认证后写 InstallPending
```

Bootloader 不运行 FreeRTOS，因为启动阶段只有一条受控状态机；引入调度器不会提高
Flash 擦写或密码学吞吐，反而会扩大代码和故障面。WeatherClock 中 ESP-AT 则由单一
工作任务串行访问，其他任务通过队列提交工作，避免多个任务交叉发送 AT 命令。

## 4. Bootloader 模块

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

## 5. WeatherClock 模块

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

## 6. 上位机

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

## 7. 启动时序

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

## 8. 关键设计取舍

这些取舍也是面试时最值得讲清楚的部分：

- **外部 A/B，而非内部 A/B**：STM32F407VG 内部 512 KiB Flash 无法容纳两份
  448 KiB 应用；两颗 W25Q128 分别保存已确认版本和候选版本，内部 Flash 只运行
  一份明文应用。
- **应用下载、Bootloader 安装**：WeatherClock 擅长联网和后台下载，但没有安装
  权限；Bootloader 只接收已经落在外部槽中的完整候选包，并在擦内部 Flash 前重新
  验签、校验 GCM Tag、CRC、SHA 和向量表。
- **Header 最后提交**：无论 USART3 还是 OTA，正文先写，固定 Header 最后写；因此
  掉电留下的是“不可启动的半成品”，而不是貌似有效的损坏固件。
- **BCR 双副本**：用 sequence 和 CRC 选择最新有效页，换取简单明确的 EEPROM
  掉电恢复；没有引入文件系统或日志数据库。
- **签名与加密分工**：ECDSA 决定“是不是受信发布”，AES-GCM 隐藏外部槽和传输中的
  固件内容并检测密文篡改。RDP0 下物理接触者仍可经 SWD 读取 Bootloader 中的 AES
  密钥，因此当前加密不等同于硬件级防逆向。
- **断点续传与流水 ACK**：4 KiB CRC 找到可复用前缀，8 KiB 数据块让 PC 发送和
  W25Q 编程重叠；最后一块改用同步确认，避免“已接收”被误解为“已落盘”。
- **有限 Trial，而非无限自愈**：新应用必须主动证明核心任务健康；连续失败后恢复
  已确认槽。逻辑围绕实际掉电和看门狗故障设计，没有堆叠假想恢复层。

## 9. 构建、大小和测试

Release 使用 `-Os` 和 `--gc-sections`，没有为个别模块强制 `-O2`。当前实测：

- Bootloader 34,896 字节，占 48 KiB 的 71.00%，余 14,256 字节；
- WeatherClock 157,680 字节，占 448 KiB 的 34.37%，余 301,072 字节。

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
