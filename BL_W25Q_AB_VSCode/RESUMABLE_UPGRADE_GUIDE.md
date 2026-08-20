# Formal USART3 断点续传说明

断点续传只优化“上位机把 XBIN 写入非活动 W25Q 槽”这一段。它不改变 Formal
验签、GCM、A/B 安装、Trial 或回退条件。

## 1. 基本思路

上位机把 XBIN 的 4 KiB Header 区和密文正文按 4 KiB 划块，先在电脑上计算每块
CRC32，再用 `SLOT_RESUME_SCAN (0x56)` 让 Bootloader 对非活动槽计算相同 CRC。

- CRC 相同的块直接复用；
- CRC 不同的块需要重写；
- 擦除必须按 W25Q 边界进行，因此从第一个差异块向下对齐到 32 KiB；
- 对齐擦除覆盖到的后续块即使原本相同，也会重新发送；
- Header 最后写入，避免半包拥有有效 magic。

CRC 扫描只是传输优化，不能代替最后的 ECDSA、GCM、SHA 和全量数据校验。

## 2. 为什么不会明显拖慢完整升级

主机 CRC 读取本地 158 KiB 文件，耗时远小于串口和 Flash。设备侧按 4 KiB 顺序
读取 W25Q，当前实板扫描 39 块约 0.10 秒。

若所有块都不同，额外成本主要是这次扫描；若大部分相同，可省去擦除和串口传输。
用户也可使用 `--upgrade-full` 明确跳过扫描。

## 3. 命令

断点复用：

```powershell
dotnet SerialUpgrader/bin/Release/net8.0-windows/SerialUpgrader.dll `
  --upgrade COM29 build/release/WeatherClock/stm32f407_upgrade.xbin
```

强制完整重写：

```powershell
dotnet SerialUpgrader/bin/Release/net8.0-windows/SerialUpgrader.dll `
  --upgrade-full COM29 build/release/WeatherClock/stm32f407_upgrade.xbin
```

Formal 上位机不再把未知响应解释成旧 Bootloader 并切换旧协议。格式、状态或命令
不匹配时直接停止，避免为了兼容而擦除错误区域。

## 4. 掉电边界

- 扫描期间掉电：没有写入，不影响任何槽；
- 擦除或编程期间掉电：只影响非活动槽，活动槽和内部应用不变；
- Header 尚未提交：候选不具备有效 Formal magic；
- 完整槽验证前掉电：BCR 仍是 Normal；
- 激活后掉电：由 InstallPending/Trial 状态机继续处理。

恢复后再次执行 `--upgrade`，上位机会重新扫描实际 W25 内容，而不是相信电脑上
保存的进度，因此不需要额外的 PC 端断点文件。

## 5. 当前实测

Formal release 4，应用 158,024 字节，共 39 个 4 KiB 块：

| 场景 | 扫描 | W25 编程 | 总提交 |
|---|---:|---:|---:|
| 强制完整重写 | 跳过 | 2.11 秒 | 2.72 秒 |
| 两槽已有同一镜像 | 0.10 秒 | 0.09 秒 | 0.81 秒 |

第二个场景复用了全部 158,024 字节，随后仍执行完整 Formal 校验、Trial 和应用
健康确认，最终 BCR 为 `Normal / active=B / seq=320`。

## 6. 主机算法测试

```powershell
cmake --build build/release --target test_resume_algorithm --parallel 1
```

测试使用真实 XBIN 和合成的 10%、50%、95% 中断内容，检查 4 KiB 分块、首差异
定位、32 KiB 擦除回退和协议请求长度。
