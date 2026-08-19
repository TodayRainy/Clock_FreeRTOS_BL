# 断点续传设计与使用说明

本文说明 `v3.3.0-resume-dev/prod` 的外部A/B断点续传协议、掉电边界、上位机
行为、兼容策略和实板验证结果。该功能只优化XBIN写入非活动W25Q槽的过程，不
改变BCR格式、XBIN签名格式、内部Flash安装流程或Trial/回滚状态机。

## 1. 目标与边界

断点续传解决的是“串口上传到外部非活动槽时中断，下一次不必从头发送”的问题。
它不尝试从内部Flash安装过程的中间位置继续；内部安装仍由`InstallPending`阶段
从完整外部槽重新执行，因为该阶段时间较短，并且重新安装更容易保证一致性。

本方案保持以下安全边界：

- 只允许扫描、擦除和写入非活动槽；
- 扫描开始时立即使非活动槽Header的magic失效；
- 应用数据完成后才擦除并重写有效Header；
- `SLOT_ACTIVATE`仍重新执行Header CRC、数据CRC32、SHA-256、ECDSA-P256签名、
  安全版本和向量表检查；
- 分块CRC只用于定位续传位置，不作为固件可信性的依据；
- 任何续传失败都不会改写当前活动槽、内部应用或BCR。

## 2. 为什么不持久化“最后写入地址”

`W25_PROGRAM_ASYNC`的ACK表示Bootloader已经接受完整数据包，ACK发出后W25Q页编程
仍可能正在执行。若把最后一个ACK直接当成落盘进度，PC断开或设备掉电时可能跳过
尚未真正写完的块。

本方案不把进度写入EEPROM，也不依赖上一次会话的RAM状态。每次重新连接后都读取
W25Q并重新计算4 KiB块CRC，因此异步ACK、PC强制退出和MCU复位都不会产生虚假的
续传进度，同时避免增加EEPROM写放大和进度记录的双副本事务。

## 3. 4 KiB分块规则

XBIN中只有应用数据区参与断点比较：

```text
0x000000..0x0000FF  256字节签名Header，最后提交
0x000100..0x000FFF  保持擦除态，不参与续传
0x001000..data_end  应用数据，按4 KiB顺序计算CRC32
```

最后一块只计算实际数据，不把扇区尾部的`0xFF`计入CRC。当前308,868字节应用形成
76个CRC32，请求总长度为311字节，远低于8,200字节协议MTU。

Bootloader从第一块开始顺序比较，在首个不同块立即停止。因此全新或差异较大的
固件通常只读取一个4 KiB块；只有真正存在长前缀时才继续扫描，而这些读取随后能
换来对应数据的复用。

## 4. `SLOT_RESUME_SCAN`协议

Opcode为`0x56`，采用现有请求帧和CRC16封装。

请求Payload：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 1 | 目标槽，0为A、1为B |
| 1 | 4 | 应用数据长度，小端uint32 |
| 5 | 2 | 4 KiB块数量，小端uint16 |
| 7 | `count * 4` | 每块IEEE CRC32，小端uint32数组 |

成功响应固定为4字节槽内绝对偏移：

- 返回`0x1000 + N * 4096`：第N块开始需要重写；
- 返回`0x1000 + data_length`：全部应用数据已经存在，只需重建Header并激活。

`PARAM`表示槽号、长度、块数或请求大小不符合契约；`VERIFY`表示Header失效操作或
W25Q扫描失败；`OPCODE`表示旧Bootloader不支持该命令。

该命令在扫描前把非活动槽首4字节编程为0。NOR Flash在这里仅发生`1 -> 0`变化，
不需要先擦除；随后即使立刻掉电，该槽也不会被识别为有效固件。

## 5. 上位机续传流程

默认升级步骤如下：

1. 查询BCR，选择`active_slot ^ 1`作为目标；
2. 上位机对当前XBIN应用数据一次性计算分块CRC；
3. 发送`SLOT_RESUME_SCAN`，设备使旧Header失效并返回首个不一致位置；
4. 为优先使用W25Q 32 KiB/64 KiB Block Erase，上位机向前回退到最近的32 KiB
   边界；最多重复写入不到32 KiB已经匹配的数据；
5. 擦除该边界至候选固件末端，只发送剩余应用数据；
6. 必要时擦除首4 KiB扇区，最后写入256字节有效Header；
7. 调用`SLOT_ACTIVATE`执行完整验签和防降级检查；
8. 原A/B状态机进入`InstallPending -> Trial -> Normal`。

因此这里的“断点”是可靠的4 KiB匹配边界，并为擦除效率回退到32 KiB边界，不是
精确到最后一个串口字节。这个取舍可避免大量零散4 KiB擦除，同时仍能保留大部分
已完成内容。

## 6. 兼容和强制全量升级

新上位机默认自动续传。旧Bootloader对`0x56`明确返回`OPCODE`时，上位机自动回退
到原有“整区擦除、完整发送、Header最后提交”流程。

命令行自动续传：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade COM20 .\build\release\WeatherClock\stm32f407_upgrade.xbin
```

命令行强制完整重写：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --upgrade-full COM20 .\build\release\WeatherClock\stm32f407_upgrade.xbin
```

图形界面对应“强制完整重写（禁用断点续传）”复选框。遇到外部Flash硬件故障、
诊断需要或希望覆盖测试全部扇区时，可以使用完整重写；普通升级保持默认即可。

## 7. 中断时的结果

| 中断位置 | 下次升级行为 | 活动固件 |
|---|---|---|
| CRC列表尚未发完 | 没有执行命令，重新查询 | 不变 |
| Header magic失效后 | 扫描数据区并重新计算断点 | 不变 |
| 擦除剩余区域时 | 首个未完整块CRC不符，从更早边界重写 | 不变 |
| 异步ACK后、页编程完成前 | 实际内容决定CRC结果，不信任ACK | 不变 |
| 数据完成、Header提交前 | 返回数据末端，只重建Header | 不变 |
| Header完成、激活前 | 再次扫描数据并重建Header，然后激活 | 不变 |
| 激活以后 | 由原有InstallPending/Trial/回滚状态机处理 | 受BCR保护 |

如果串口或W25Q错误使扫描命令没有得到明确响应，上位机直接停止，不会擅自把链路
错误解释为“旧Bootloader”并开始擦除。只有明确的`OPCODE`响应才触发兼容回退。

## 8. 性能实测

测试固件应用数据308,868字节，USART3为2 Mbps，W25Q SPI为21 MHz。

| 场景 | 扫描 | A/B升级总耗时 |
|---|---:|---:|
| 数据100%相同，只重建Header | 0.18秒 | 0.69～0.70秒 |
| `AAAAA`与`BBBBB`固件内容不同 | 0.05秒 | 4.36秒 |
| 显式强制完整重写 | 跳过 | 4.58秒 |
| 约96.7%中断后恢复，复用290,816字节 | 0.18秒 | 1.14秒 |

旧流程同一固件的实测总耗时约4.67秒。内容不同的普通升级没有变慢；相同或高度
完成的固件则显著缩短。上位机只在加载当前XBIN时生成一次CRC列表，不会在每个
数据包之间反复计算整包CRC。

## 9. 已完成验证

- Debug和Release Bootloader编译通过，Release占用28,268字节/48 KiB；
- SerialUpgrader Debug/Release为0 warning、0 error；
- `test_resume_algorithm.py`覆盖完整匹配、全不匹配、10%/50%/95%中断、32 KiB
  回退、最大固件块数和协议MTU；
- 实板在11.9%、51.7%、96.7%进度处三次强制杀死上位机；
- 52%半成品恢复时复用159,744字节；97%半成品恢复时复用290,816字节；
- 最终包通过CRC32、SHA-256、ECDSA和Trial健康确认；
- 100%复用、不同固件、强制完整重写三条路径均已完成实板闭环；
- 故障注入期间BCR保持Normal且原活动槽始终有效。

## 10. 代码入口

| 文件 | 作用 |
|---|---|
| `STM32F4_Bootloader/app/boot_protocol.c` | `0x56`请求解析、Header失效和响应 |
| `STM32F4_Bootloader/app/firmware_slot.c` | 逐块读取W25Q并定位首个不同块 |
| `SerialUpgrader/Transfer.cs` | CRC列表、兼容判断、擦除回退和续传 |
| `SerialUpgrader/Program.cs` | `--upgrade`与`--upgrade-full`命令行入口 |
| `SerialUpgrader/Form1.cs` | GUI自动续传和强制完整选项 |
| `tools/test_resume_algorithm.py` | 不依赖板卡的布局和边界回归测试 |

## 11. 当前限制

- 只复用从应用起始位置连续匹配的前缀，不做离散块去重；这样协议和擦写顺序更
  简单，也符合当前顺序传输模型；
- 若固件在很靠前的位置发生变化，断点续传收益较小，但扫描会在首个差异处立即
  停止，因此额外开销也很小；
- CRC32存在理论碰撞可能，但错误复用仍会在最终SHA-256和ECDSA检查中被拒绝，
  不会安装为有效固件；
- 断点状态只存在于非活动槽实际内容，不提供跨设备或跨目标槽迁移；
- 真正的电源棕断测试仍需要可控电源；当前实板使用进程强杀和J-Link复位覆盖软件
  会话中断与重新启动路径。
