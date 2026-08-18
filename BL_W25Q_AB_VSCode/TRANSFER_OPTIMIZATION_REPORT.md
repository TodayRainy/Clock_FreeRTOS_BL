# Bootloader 传输与恢复流程优化报告

## v2.1.0 快速状态与SPI读取DMA补充

本轮没有改变XBIN格式、A/B提交顺序或2 Mbps信用ACK传输方式，优化的是固件
发送前后的两次外部读取：

- 新增 `SLOT_STATUS_FAST (0x54)`，只返回经过双副本和CRC校验的17字节BCR；
  正常升级据此选择非活动槽，不再提前扫描两份约300 KiB的旧固件；
- 人工 `SLOT_STATUS (0x50)` 保持原有全量语义；新上位机遇到旧Bootloader不
  支持0x54时自动回退到0x50；
- W25Q连续读取改用DMA2 Stream0接收和Stream3发送固定0xFF，SPI仍为21 MHz；
- 固件校验/安装缓冲由1 KiB调整为4 KiB，CRC32算法和校验值完全不变；
- 跳转应用前停止两条SPI DMA流、关闭SPI DMA请求并复位DMA2/SPI1。

312,428字节XBIN的实板结果：

| 项目 | v2.0.1 | v2.1.0 |
|---|---:|---:|
| 升级前A/B状态 | 0.35～0.68 s | 0.02 s |
| 串口传输及W25编程 | 2.86 s | 2.86 s |
| 单槽完整校验及提交 | 0.37 s | 0.19 s |
| 含握手完整升级 | 4.95～5.29 s | 4.46～4.49 s |
| 人工双槽完整状态进程耗时 | 约1.00 s | 0.674 s |

Release Bootloader使用21,780字节Flash（48 KiB的44.31%）和35,608字节RAM
（128 KiB的27.17%）。SPI DMA实测后，单槽校验的0.19秒已经接近21 MHz读取
308 KiB的0.118秒线速下限；硬件CRC最多只剩约0.07秒理论空间，而且F407 CRC
外设需要额外处理IEEE反射位序，因此本轮不引入硬件CRC。

安全回归包括完整升级、安装、应用确认、双槽全量CRC、DMA反初始化，以及在
2 Mbps传输5.2%时强制终止上位机。中断后BCR保持
`Normal / active=B / pending=none`，活动B槽完整有效，目标A槽因Header最后写入
而保持无效；重新升级后两槽均恢复有效。最终实板为
`Normal / active=A / pending=none / sequence=123`，两槽版本均为
`v1.4.0-260818-1913-ab`，数据CRC32均为 `0x98A25921`。

最终整片内部Flash救援镜像：

```text
../.device-backups/stm32f407_ab_fast_spidma_v2_1_0_20260818.bin
SHA256 62687A3225954BCAFA13DC741CFC8CAB2D4ED46120CAF3C36818C01B9D1BCE1D
```

恢复脚本为 `tools/jlink/restore_fast_spidma_final.jlink`。该镜像已在读取后立即
对实板执行512 KiB `verifybin`，且其前21,780字节与最终Release Bootloader逐
字节一致。

## v2.0.1 DMA流水补充

在v1.9.1长连接、高速协商、21 MHz SPI和混合块擦除的基础上，当前版本进一步
加入以下改动：

- USART3_RX改为DMA1 Stream1循环接收，跳转应用前显式停止DMA并关闭请求；
- 请求MTU由4104字节提高到8200字节，固件数据块由4 KiB提高到8 KiB；
- 新增 `W25_PROGRAM_ASYNC (0x44)`，中间块通过接受ACK实现串口接收与W25Q
  页编程重叠；最后一块仍使用同步 `W25_PROGRAM (0x43)` 作为实际写入屏障；
- 高速协商增加2 Mbps，并作为GUI和CLI默认值；115200启动入口、921600等旧档位
  以及原请求帧格式均保留。

最初尝试过两个完整请求同时在途，但实板USB-TTL在MCU应答与PC大包发送重叠时
会偶发丢失一字节。最终改为信用ACK：MCU完整接收并校验第N块后先应答，再一边
写第N块、一边由DMA接收第N+1块。这样保留了流水收益，同时避免两条串口方向在
大包尾部同时工作。

312,428字节XBIN实测结果：

| 版本与配置 | 串口传输及W25编程 | 含握手总耗时 |
|---|---:|---:|
| v1.9.1，921600，4 KiB，逐包写后ACK | 6.04 s | 8.46～8.52 s |
| v2.0.1，921600，8 KiB，DMA信用ACK | 4.56 s | 6.62 s |
| v2.0.1，2 Mbps，8 KiB，DMA信用ACK | 2.86 s | 4.95～5.29 s |

当前Release Bootloader使用21,204字节Flash（48 KiB保留区的43.14%）和32,536
字节RAM（128 KiB的24.82%）。RAM增长主要来自16 KiB DMA循环缓冲和8 KiB协议
包缓冲，仍保留充足空间。

新流水语义仍通过以下顺序保证A/B安全：先擦非活动槽、先写XBIN主体、最后同步
写Header、完整槽CRC通过后才提交InstallPending。实测在2 Mbps写入约10.5%时
强制终止上位机，BCR保持 `Normal / active=A`，目标Slot B为
`header=False/full=False`；随后完整重写并恢复为双槽有效。

最终设备状态为 `Normal / active=B / pending=none / sequence=110`，Slot A/B
均为版本 `v1.4.0-260818-1858-ab`、数据CRC32 `0x35587D08`。最终整片救援镜像：

```text
../.device-backups/stm32f407_ab_dma_pipeline_v2_0_1_20260818.bin
SHA256 1BD66F11090ED9FED23D1BC3883138121C33F12F251CEB80D59EB6FBF9D86BFE
```

恢复脚本为 `tools/jlink/restore_dma_pipeline_final.jlink`。以下章节保留v1.9.1
阶段的历史优化和测试记录。

## 1. 最终结论

本轮在不改变 XBIN 格式、不增加 DMA/压缩/滑动窗口等复杂机制的前提下，
把 312,428 字节 XBIN 的外部 A/B 提交时间从 115200 优化版的 35.50 秒降到
约 8.48 秒，速度约为原来的 4.2 倍。

最终 Bootloader 版本为 `v1.9.1-ab-fastboot`。设备每次复位仍固定从
115200 baud 开始，确认双方通信正常后才切换到 921600 baud，因此高速模式
不会破坏原有串口救援入口。

最终实机状态：

- BCR：`Normal`，active Slot A，pending none；
- Slot A/B：Header 和数据 CRC32 均有效；
- 两槽固件数据 CRC32：`0x74406F8F`；
- Bootloader：20,796 字节，占 48 KiB 保留区 42.31%；
- Bootloader RAM：17,184 字节，占 128 KiB SRAM 13.11%。

## 2. 代码改动

### 2.1 上位机保持一个串口会话

升级开始时打开 COM 口，所有 4 KiB 请求复用同一连接，结束时统一释放。
这消除了 Windows 和 CH340 对每个数据包重复打开、配置和关闭串口的开销。

### 2.2 高速波特率协商

新增 `SET_BAUD (0x02)`：

1. PC 和 Bootloader 先以 115200 完成版本和 MTU 查询；
2. PC 发送带 CRC16 的目标波特率；
3. Bootloader 用旧波特率完整发送 ACK，并等待停止位离开发送移位寄存器；
4. MCU 和 PC 同时切换波特率；
5. PC 在新速率再次查询版本，复核高速链路后才允许擦除目标槽。

支持 115200、230400、460800 和 921600。CLI 和 GUI 当前默认 921600，CLI
可在命令末尾显式指定其他速率。不支持的速率会在任何擦除发生前被拒绝。

### 2.3 减少重复的全量槽校验

新版 `SLOT_ACTIVATE` 可携带 `slot + expected_data_crc32` 共 5 字节。Bootloader
对目标槽做一次完整 CRC32，并同时确认其 Header CRC 与上位机所选 XBIN 一致，
之后才写入 `InstallPending`。旧版 1 字节槽号格式仍然兼容。

安装函数复用 A/B 状态机刚刚验证得到的 Header，不再立即重复扫描同一外部槽。
内部 Flash 仍执行写后 CRC32，且仍然最后写入有效 Header，所以掉电安全顺序未变。

### 2.4 W25Q SPI 与混合块擦除

SPI1 由 5.25 MHz 提升到 10.5 MHz，再经整槽回归后提升到 21 MHz。21 MHz
远低于 W25Q128 普通读写允许的时钟，并已在两颗板载芯片上完成写入和 CRC 回归。

擦除从 77 次 4 KiB Sector Erase 改为按地址自动组合：

- 64 KiB Block Erase，命令 `0xD8`；
- 32 KiB Block Erase，命令 `0x52`；
- 4 KiB Sector Erase，命令 `0x20`。

算法只选择完全位于请求范围内的最大对齐块，尾部使用 4 KiB 精确收尾，不会
额外擦除目标固件范围之外的数据。315,392 字节区域的实测擦除时间由约 3.41 秒
降到约 1.29 秒。

### 2.5 缩短自动安装阶段

`Normal` 阶段仍保留原有 3 秒 USART3 捕获窗口。`InstallPending`、`Trial` 和
`RollbackPending` 已有明确的持久化事务，不再等待普通串口数据，而是立即推进
状态机。

KEY2 检测始终位于该判断之前。在任何阶段持续按住 KEY2 约 3 秒仍可停留在
Bootloader；J-Link/SWD 救援也不受影响。实测升级提交后 8 秒内已经完成安装、
进入应用、等待 5 秒并确认到 `Normal`，旧流程通常需要约 14 至 16 秒。

### 2.6 跳转应用前反初始化SPI1

最终保留21 MHz W25Q速率，但在直接跳转应用前调用 `w25q_deinit()`：等待
SPI1空闲、保持PE13/PE14片选为高、关闭并复位SPI1、关闭APB2外设时钟，并把
PA5/PA6/PA7恢复为模拟输入。应用运行期间的J-Link寄存器实测为：

```text
SPI1_CR1    = 0x00000000
RCC_APB2ENR = 0x00000010  (SPI1EN = 0)
GPIOA_MODER = 0xA828FCA0  (PA5/PA6/PA7 = analog)
GPIOE_ODR   = 0x0000623C  (PE13/PE14 = 1)
```

随后再次完成一轮完整A/B升级并在8秒内确认到Normal，证明下一次Bootloader
能够重新初始化SPI1和两颗W25Q。

## 3. 性能实测

测试镜像：

- XBIN：312,428 字节；
- 应用 Payload：308,332 字节；
- 数据 CRC32：`0x74406F8F`。

| 阶段 | 配置 | 含握手总耗时 |
|---|---|---:|
| 长连接基线 | 115200，仍有写后重复状态扫描 | 36.75 s |
| 去除重复校验 | 115200 | 35.50 s |
| 高速协商 | 460800 | 15.07 s |
| 高速协商 | 921600，SPI 5.25 MHz | 11.95 s |
| SPI 提速 | 921600，SPI 10.5 MHz，4 KiB 擦除 | 11.03 s |
| 混合块擦除 | 921600，SPI 10.5 MHz | 8.51 s |
| 最终配置 | 921600，SPI 21 MHz，混合块擦除 | 8.46–8.52 s |

最终一次典型分项为：

- 读取并校验 A/B 状态：0.68 s；
- 擦除非活动槽：1.29 s；
- 串口传输和 W25Q 页编程：6.04 s；
- 完整槽校验与提交：0.37 s；
- 升级内部计时：8.38 s；
- 包含初始握手：8.48 s。

SPI 从 10.5 MHz 提到 21 MHz 仅再节省约 0.05 秒，说明当前瓶颈主要是 W25Q
页编程/擦除的内部忙等待和 UART 数据量。继续加入 DMA、压缩或滑动窗口会明显
增加代码和故障定位成本，本轮没有采用。

## 4. 安全与恢复实测

### 4.1 外部写入中断

一次升级在写入非活动 A 槽约 68% 时 USB-TTL 物理掉线。设备保持
`Normal / active=B`，A 槽 Header 无效。重新连接后从擦除开始完整重写 A 槽，
最终通过 CRC 并正常确认。这验证了“最后写 Header”和提交前不修改活动槽。

### 4.2 InstallPending 重试

内部安装过程中进行了连续复位。状态保持 `InstallPending`，外部完整副本没有
被修改；停止复位后 Bootloader 重新执行安装并成功进入 Trial。

### 4.3 Trial 三次失败回滚

通过 USART1 日志检测每次 `booting trial image` 后立即用同一个 J-Link 会话复位，
实测尝试次数依次为 2、1、0。下一次启动记录：

```text
trial firmware was not confirmed; scheduling rollback
state=valid phase=3 active=1 pending=0 attempts=0
restoring confirmed slot 1
```

最终 BCR 为 `Normal / active=B / pending=none`，A/B 两槽均通过完整 CRC32。

### 4.4 非法波特率

请求 12345 baud 时 Bootloader 返回参数错误，上位机报告“升级尚未开始”。之后
BCR 序号、active B 和两槽 CRC 均未变化。

## 5. 构建和产物校验

Release 镜像验证结果：

```text
Bootloader: 20796 bytes, end=0x0800513C
WeatherClock: 308332 bytes, end=0x0805B46C
XBIN: payload=308332, data CRC=0x74406F8F, header CRC=0xF4FAEB5D
All image checks passed.
```

Bootloader Release BIN SHA256：

```text
5509F9FE0831F43BDE0B3BBC1AEEC9B1722DC2957B52BDD79AF6197506C4E154
```

XBIN SHA256：

```text
DD00F3A45B8E0E5108B2A661DEEB4A6FEFF54FBCA8FF88B2973F5B6D0275D278
```

## 6. 使用方法

默认 921600 升级：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM18 .\build\release\WeatherClock\stm32f407_upgrade.xbin
```

显式使用 460800：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM18 .\build\release\WeatherClock\stm32f407_upgrade.xbin 460800
```

无论选择哪个升级速率，刚复位时都必须以 115200 进入 Bootloader。端口号应以
Windows 当前枚举为准，不应永久假设为 COM18。

## 7. J-Link 救援镜像

当前最终内部 Flash 已备份为：

```text
../.device-backups/stm32f407_ab_fastboot_v1_9_1_20260818.bin
```

大小 524,288 字节，SHA256：

```text
BD7AFA211C03DBD60CCAB022FFFCBCE30CE66C07239347BAE76DF620EE1367E8
```

该镜像已经使用 J-Link `verifybin` 与芯片内部完整 512 KiB Flash 反向校验成功。
恢复脚本为 `tools/jlink/restore_fastboot_final.jlink`。原有 GCC 基线救援镜像仍然
保留，没有被覆盖。
