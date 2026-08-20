# Formal 安全启动与防降级说明

本项目的“安全启动”指 Bootloader 在安装和跳转前验证 Formal Header、ECDSA、
AES-GCM、CRC32、SHA-256、向量表和版本策略。它不是 STM32 Option Bytes 的硬件
Secure Boot，也没有关闭 SWD。

## 1. 信任根

Bootloader 编入一把 ECDSA-P256 公钥和一把 AES-256 密钥：

- 公钥决定哪些 Header 是项目签发的；
- AES 密钥认证并解密外部 XBIN 正文；
- Key ID 防止误用另一套密钥；
- Header 中的 SHA-256 把签名元数据和明文应用绑定。

Bootloader 自身由 J-Link/SWD 写入，不在软件链上自验证。因此获得 SWD 写权限的
人可以替换整个 Bootloader，这也是保留救援能力的明确代价。

## 2. 两个版本数字

`release_sequence` 表示普通发布先后：功能更新、UI 调整、Bug 修复都递增它。
自动 OTA 只接受更大的 sequence，同 sequence 不同 SHA 被视为发布冲突。

`security_version` 表示不可回退的安全代际。应用成功确认后，BCR 把它提升为设备
下限；低于下限的候选不能自动或普通串口安装。

不要每次发布都提高 security version，否则用户将失去选择旧功能版本的能力。
只有旧版存在已知安全缺陷、确认不能再运行时才提高该值。

## 3. 用户主动回退

KEY2 持续按住进入 Bootloader，代表本次会话具有物理在场授权。DEV/PROD 都可以
用它明确授权安全版本回退；授权只存在于当前上电会话，复位后消失。

自动 OTA 永不使用这条授权。它不能因为服务器配置错误把设备降级。上位机应明确
显示候选和设备安全版本，让用户知道正在执行人工回退。

## 4. DEV 与 PROD

两种 profile 使用同一 Formal 格式和项目密钥：

- DEV：工程维护命令默认开放，便于读取 W25Q/EEPROM、修复 BCR 和调试；
- PROD：原始读写、人工设置槽等维护命令只有 KEY2 物理在场时开放；
- 正常非活动槽上传、完整验证和激活在两种 profile 中均可使用；
- RDP、WRP、SWD 和 Option Bytes 均不由 profile 修改。

构建 PROD：

```powershell
cmake --preset release-prod
cmake --build --preset release-prod
cmake --build build/release-prod --target validate_images --parallel 1
```

PROD 是协议权限收紧，不等于商业级密钥隔离或物理防拆。

## 5. A/B 安全属性

活动外部槽是最后一份已确认回退源。任何上传只能写另一槽；候选必须完整验证后
才能写 `InstallPending`。内部应用安装成功后进入 `Trial`，应用健康确认前不会
替换活动槽。

失败路径：

- 候选签名或 GCM 错误：拒绝激活，活动槽不变；
- 内部安装掉电：保留 `InstallPending`，下次重装；
- Trial 重启或超时：尝试次数递减，最终回退活动槽；
- BCR 新页掉电：读取另一份旧页；
- 两槽都无效：留在 Bootloader，等待 USART3 或 J-Link 救援。

## 6. 首次 Formal 采用

外部快照已删除。迁移时先预置一份完整 Formal 外部槽，再写内部 Formal 镜像。
Bootloader 只有在外部槽与内部 Header 的长度、CRC、SHA、security、release、
版本和 Key ID 全部一致时才重建 BCR。

迁移保存使用独立临时状态，EEPROM 提交成功后才替换 RAM 状态；首次读失败会补读
一次，避免旧高 sequence 记录压过从 1 开始的新记录。这两点来自本次实板发现的
真实问题，不是泛化的预见性防御。

## 7. 看门狗和健康确认

Bootloader 启动 IWDG，长时间 W25Q 擦除、内部摘要和安装循环会主动刷新。应用的
FreeRTOS 健康任务继续喂狗；调试 DEV 时 CPU halt 会冻结 IWDG，正常运行不冻结。

应用只有在核心初始化和稳定窗口通过后才确认 Trial。`configASSERT` 不会返回，
Wi-Fi 等待使用阻塞延时，不会以空转循环饿死健康任务。

## 8. 仍然存在的安全缺口

- SWD 与 RDP0 允许物理读取或替换 Bootloader、应用和 AES 密钥；
- 私钥提交在私人 Git 仓库，仓库账户安全就是签名私钥安全的一部分；
- 单一公共 AES 密钥泄露会影响全部同类板卡；
- 没有安全元件、设备证书、一机一密或远程吊销；
- 没有对 Bootloader 本体做硬件不可变信任链。

这些限制是“个人学习项目 + 必须可救援”的主动取舍。若未来商业化，应重新设计
密钥保管、RDP/WRP、调试解锁、生产烧录和设备身份，不能直接沿用当前密钥。
