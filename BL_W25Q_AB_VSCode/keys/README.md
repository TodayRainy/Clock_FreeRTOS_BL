# Formal firmware keys

本目录是私人学习项目 `Clock_FreeRTOS_BL` 的 Formal 固件密钥集合：

- `formal_firmware_private.pem`：ECDSA-P256签名私钥；
- `formal_firmware_public.pem`：编入Bootloader的验签公钥；
- `formal_firmware_aes256.key`：打包器与Bootloader共用的32字节AES密钥。

项目所有者明确允许把这些密钥提交到私人Git仓库，以简化个人项目的构建和恢复。
它们不具备商业生产密钥的隔离条件，严禁在将来的商业产品或其他设备系列中复用。

如果私钥或AES密钥丢失，现有Formal Bootloader将无法接收后续新包；除Git远端外，
仍建议保留一份离线备份。当前设备保持RDP0和SWD开放，所以拥有板卡和调试接口的
人仍可能提取Bootloader中的AES密钥。这里的加密目标是保护COS、串口和W25Q中的
固件内容，而不是对抗物理拆机读取。
