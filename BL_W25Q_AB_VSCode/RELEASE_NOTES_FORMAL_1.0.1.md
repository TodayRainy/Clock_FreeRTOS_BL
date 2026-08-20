# Formal 1.0.1 发布说明

- 发布日期：2026-08-20
- 发布序号：5
- 安全版本：2
- XBIN 格式：Formal revision 1

## 发布定位

本版本完成项目级编码规范重构。它没有改变 Formal Header、A/B 状态记录、OTA 请求、
串口 Opcode、Flash 分区、密钥或安全策略的二进制契约；主要变化是统一自研代码风格、
补齐关键注释、清理确定无用的路径，并修复审阅和实板回归中发现的真实问题。

## 主要变化

- 为共享契约、Bootloader 状态机、USART3 协议、WeatherClock 任务和底层驱动补齐
  Doxygen 接口说明及关键状态转换注释；
- 统一协议字段的小端读写，消除未对齐宽指针访问；
- 复用项目 CRC32，删除 A/B 确认模块中的重复实现；
- 修复天气路径最大长度的一字节越界；
- 修复 AHT20 I2C 失败路径未统一发送 STOP、RTC 日历快照读取顺序等驱动问题；
- 删除没有实际调用者的 USART1 TX DMA 路径，保留轮询日志输出；
- 修复 USART1 未注册回调时未读取 DR、可能持续触发 RXNE 中断的问题；
- 整理 C# 上位机的异步流程和显式小端解析，并新增固定帧协议回归测试；
- 修复重构后上位机误用响应帧偏移构造请求帧的问题。

## 发布产物

| 产物 | 长度 | SHA-256 |
|---|---:|---|
| Bootloader BIN | 34,896 B | `2217230AF198053E38D459E0CBCBC43E60DC0EBADBED3521D709E650DAF29F9E` |
| WeatherClock BIN | 157,680 B | `D0986C7A785B98B4A9907A221038D76CBDF62D2105A1B8638B11134B9CFF88E5` |
| Formal XBIN | 161,776 B | `C3FD621C1D52CD46E4CD4C6D0B0AC26FD4DA660C4C4FFB1A6D53CCE1ECFCAFE0` |

WeatherClock payload CRC32 为 `0x6927330E`，签名 Key ID 为 `0xCA9EEE70`，
AES Key ID 为 `0x9B5260DC`。

公网对象：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/releases/weatherclock-formal-1.0.1-r5.xbin`

Manifest：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/manifest.json`

## 实板验收

- J-Link SWD 连接、写入、整片回读 Verify 均通过，VTref 约 3.293 V；
- USART3 在 2 Mbaud 下完成两次非活动槽完整写入，W25 编程约 2.1 秒，提交总耗时
  约 2.7 秒；同镜像完整复用总耗时 0.81 秒；
- `InstallPending -> Trial -> Normal` 和应用健康确认通过；
- 连续复位使 Trial 次数耗尽后自动回退，且在 `RollbackPending` 安装过程中再次复位
  仍能继续恢复；
- 旧版 `formal-1.0.0` 应用通过手机热点获取 Manifest、下载 release 5、提交请求，
  复位后由 Bootloader 安装并确认 `formal-1.0.1`；
- 最终状态为 `Normal / active=A / pending=none / attempts=0 / seq=363`，A、B 两槽
  和内部应用均为 `formal-1.0.1`。

## 救援

最终内部 Flash 镜像：

`../.device-backups/stm32f407_formal_1_0_1_refactored_20260820.bin`

长度 524,288 B，SHA-256：

`67722EEAEC432F1202E5C3F26BEDC94B49B5859EABE084F039DB4A5B9552F76A`

保存后及最终验收后均完成整片 Verify。项目保持 RDP0、SWD 和 Option Bytes 不变；
`formal-1.0.0` 的已验证救援镜像也继续保留。
