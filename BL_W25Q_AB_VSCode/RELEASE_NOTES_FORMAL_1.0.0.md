# Formal 1.0.0 发布说明

日期：2026-08-20

## 目标

把尚未对外发布的旧 XBIN 签名/加密演进版本收口成一个不带兼容分支的正式格式，
并同步 Bootloader、WeatherClock OTA、Windows 上位机、构建脚本和文档。

## 已确认的格式决策

- 公开名称使用 Formal，不命名为 v4；
- Header 内保留 `format_revision=1`；
- 首个 Formal 发布使用 `release_sequence=4`、`security_version=2`；
- ECDSA-P256 和 AES-256-GCM 都是必选；
- 生成新的 Formal 签名/AES 密钥，并按项目所有者许可提交私人 Git；
- 删除内部固件到外部槽的快照功能；
- 迁移通过“预置完整外部槽并采用与内部应用相同的镜像”完成。

## 主要代码变化

- 用单一 320 字节 Header 替代旧 Header + Encryption Descriptor；
- 删除明文容器、旧格式解析和 `--no-encrypt`；
- 删除快照 opcode、旧内部 Flash 维护命令和旧激活参数；
- 外部槽始终执行签名、GCM、CRC、SHA 和向量检查；
- 内部 Flash 保存同一 Header 与认证后的明文正文；
- 上位机只接受 Formal，完整状态增加内部镜像健康结果；
- OTA 只接受 Manifest v2 与 Formal revision 1；
- Manifest 的发布判断使用 release sequence + SHA，不再把不同旧包误判为新包；
- 首次采用使用事务化 BCR 对象，EEPROM 成功保存后才更新 RAM 状态；
- 删除外部快照后，BCR 丢失恢复只使用已经存在的完整 Formal 槽。

## 构建与主机测试

- GCC Release：0 error，0 warning；
- .NET Release：0 error，0 warning；
- Bootloader：34,800 / 49,152 字节；
- WeatherClock：158,024 / 458,752 字节；
- `validate_images` 通过；
- 原生 C ECDSA/SHA 测试通过；
- 原生 C 流式 AES-GCM 测试通过；
- 防降级与 OTA 发布策略测试通过；
- OTA EEPROM 二进制契约测试通过；
- 4 KiB 断点规划测试通过；
- 密文、CRC、SHA、版本、安全版本、发布序号、签名、Nonce、Key ID 和 Tag 的
  篡改均被拒绝。

## 实板结果

迁移前状态为旧 BCR `RollbackPending / seq=311`，Slot B 已预置 Formal XBIN。
使用纯 ASCII 路径烧录并逐字节验证 Bootloader、内部 Header 和应用后：

- 自动采用 Slot B，得到 `Normal / active=B / seq=312`；
- 完整 USART3 写 Slot A：W25 编程 2.11 秒，总提交 2.72 秒；
- Trial 确认后切换 Slot A；
- 断点扫描 Slot B：复用全部 158,024 字节，总提交 0.81 秒；
- 再次确认后切回 Slot B；
- 最终 `Normal / active=B / pending=none / seq=320`；
- A/B 两槽与内部 Formal 镜像全部有效；
- J-Link 全程连接正常，未修改 Option Bytes。

## OTA 发布

COS 已按固件先上传、Manifest 后更新的顺序发布：

- XBIN：`clock-f407/releases/weatherclock-formal-1.0.0-r4.xbin`；
- Manifest：`clock-f407/manifest.json`；
- XBIN 长度：162,120 字节；
- XBIN SHA-256：`F4440CCD01F7CC06A0D9F31BDA17B105F8F7A7A2F6E7B6BEB1B73441477C496B`；
- 两个公网 URL 均验证为 HTTP 200。

## 救援镜像

`../.device-backups/stm32f407_formal_1_0_0_final_20260820.bin`

- 长度：524,288 字节；
- SHA-256：`FBCB8245952C87C6333FC71CAA8A5C5DB7A9987A2573788C3855707B43BA94DF`；
- 保存后立即完成整片 `verifybin`；
- 配套 backup/restore/verify 脚本位于 `tools/jlink/`。
