# XBIN Formal 签名与加密设计

本文说明当前主线唯一接受的固件容器。它不再兼容旧 XBIN，也不存在“明文模式”
或额外 Encryption Descriptor。公开名称是 **Formal**，内部二进制修订号是 1。

## 1. 三种校验分别解决什么问题

Formal 同时使用 CRC32、ECDSA-P256 和 AES-256-GCM，但三者职责不同：

- CRC32：快速发现串口、Flash 或文件损坏，不提供安全性；
- ECDSA-P256：证明 Header 是由持有私钥的人签发，攻击者不能随意替换版本、
  地址、摘要、Nonce 或 Tag；
- AES-256-GCM：让外部 XBIN 正文不可直接阅读，并在解密时认证密文与 Header。

签名解决“这个固件是不是我们发布的”，加密解决“别人能不能直接看到固件”。
只加密不签名仍可能接受攻击者构造的数据；只签名不加密则文件内容仍可反汇编。

## 2. 文件布局

```text
0x0000 ┌──────────────────────────┐
       │ Formal Header，320 字节  │
0x0140 ├──────────────────────────┤
       │ 保留填充到 4 KiB         │
0x1000 ├──────────────────────────┤
       │ AES-256-GCM 密文         │
       └──────────────────────────┘
```

外部 W25Q 槽保存以上完整 XBIN。STM32 内部 Flash 使用相同 Header，但
`0x08010000` 后保存已经通过 GCM 认证的明文应用。因此复位后无需边运行边解密，
应用执行速度与普通内部 Flash 程序相同。

320 字节 Header 的固定偏移如下：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 4 | magic：小端 `FORM` |
| 4 | 4 | `format_revision=1` |
| 8 | 4 | `header_size=320` |
| 12 | 4 | 固件类型 |
| 16 | 4 | 正文偏移 `0x1000` |
| 20 | 4 | 内部地址 `0x08010000` |
| 24 | 4 | 明文长度 |
| 28 | 4 | 明文 CRC32 |
| 32 | 4 | `security_version` |
| 36 | 4 | `release_sequence` |
| 40 | 12 | SHA、签名、加密算法编号 |
| 52 | 8 | 签名 Key ID、AES Key ID |
| 60 | 4 | 保留，必须为 0 |
| 64 | 32 | 明文 SHA-256 |
| 96 | 12 | GCM Nonce |
| 108 | 16 | GCM Tag |
| 124 | 64 | ASCII 版本字符串 |
| 188 | 64 | ECDSA `r || s` 签名 |
| 252 | 64 | 保留，必须为 0 |
| 316 | 4 | Header CRC32 |

`shared/firmware_image_format.h` 是 C 端唯一权威定义；Python 打包器和 C# 上位机
使用相同偏移，C 编译期 `_Static_assert` 会阻止结构体无意漂移。

## 3. 打包顺序

`WeatherClock/scripts/gen_magic_header.py` 按固定顺序生成 Formal 包：

1. 读取明文应用，计算 CRC32 和 SHA-256；
2. 填写版本、地址、算法、Key ID 和随机 96 位 Nonce；
3. 将 Tag、签名和 Header CRC 清零，把整份 320 字节 Header 作为 GCM AAD；
4. 用 AES-256-GCM 加密正文并写入 16 字节 Tag；
5. 将签名和 Header CRC 清零，使用域 `ClockFW-XBIN-Formal-r1` 对 Header 签名；
6. 把 DER 签名规范化为固定 64 字节 `r || s`；
7. 对前 316 字节计算 Header CRC32；
8. 输出 Header、填充和密文，同时另存可烧入 `0x0800C000` 的 Header 文件。

GCM AAD 绑定除 Tag、签名、Header CRC 外的全部 Header；ECDSA 又把最终 Tag
绑定进签名。这样不会形成 Tag 与签名互相依赖的循环，同时所有重要元数据都受保护。

## 4. Bootloader 验证和安装

外部槽的验证顺序是：

1. 检查 magic、revision、长度、地址、算法和保留字段；
2. 校验 Header CRC32；
3. 检查编译进 Bootloader 的签名 Key ID 与 AES Key ID；
4. ECDSA-P256 验签；
5. 流式执行 AES-GCM 解密和 Tag 认证；
6. 同一遍数据流计算明文 CRC32 与 SHA-256；
7. 检查 MSP、Thumb Reset_Handler 和应用地址范围；
8. 全部通过后才把明文写入 STM32 内部应用区；
9. 最后写 320 字节 Header，并再次从内部 Flash 复核。

安装采用 4 KiB 缓冲，不需要在 RAM 中保存整个固件。断电发生在内部 Flash 擦写
期间时，外部活动槽和候选槽仍保持完整；下次复位由 BCR 的 `InstallPending` 或
`RollbackPending` 继续安装或恢复。

内部应用启动前仍会检查 Header 签名、明文 CRC32、SHA-256 和向量表。GCM Tag
不需要再次对内部明文运行；它已经在从外部槽安装的信任边界上完成认证。

## 5. 密钥

当前项目密钥位于：

- `keys/formal_firmware_private.pem`；
- `keys/formal_firmware_public.pem`；
- `keys/formal_firmware_aes256.key`。

构建时 Python 生成 `firmware_public_key.h` 和 `firmware_encryption_key.h`，后两者
编入 Bootloader。DEV 和 PROD 使用同一套 Formal 密钥；两种 profile 的差异是
维护命令权限，不是密码算法或容器格式。

用户明确允许把密钥提交到私人代码仓库。这适合个人学习和可重复构建，但不等于
工业密钥管理：商业系统应把签名私钥放在离线签名机或 HSM，把设备解密密钥放进
安全存储，并设计独立的密钥轮换和吊销机制。

当前只内置一个签名公钥和一个 AES 密钥。若要轮换，必须先发布一个同时认识新旧
Key ID 的过渡 Bootloader，再发布由新密钥生成的固件。直接替换仓库密钥并重编
上位机无法让已经部署的旧 Bootloader 自动认识新密钥。

## 6. 能保护和不能保护的范围

能够保护：

- COS 或其他文件服务器上的 XBIN 正文；
- USART3 传输中被被动抓取的固件；
- 拆下 W25Q 后直接读取的槽内容；
- 未持有 ECDSA 私钥的人伪造可安装固件；
- 修改版本、安全级别、地址、摘要、Nonce、Tag 或密文。

不能充分保护：

- RDP0 且开放 SWD 时的物理攻击者；
- 从 Bootloader 内部 Flash 提取 AES 密钥；
- 设备已经解密并写入 `0x08010000` 后读取内部应用；
- 私钥或 AES 密钥本身泄露；
- 应用代码自身的逻辑漏洞和运行时内存攻击。

本项目的红线是可救援，因此没有开启 RDP、WRP 或关闭 SWD。加密目标是明显提高
文件分发、串口和外部 Flash 的复制门槛，而不是宣称具备防拆安全芯片的能力。

## 7. 与旧格式的边界

Formal Bootloader 不接受旧 Header、明文包或额外加密描述块；上位机也只解析
Formal。旧协议中的内部 Flash 擦除/编程命令、外部快照命令、旧激活参数和兼容
回退均已删除。

旧设备迁移必须先准备一份完整 Formal 外部槽，再写 Formal 内部 Header、应用和
Bootloader。迁移代码只会采用与内部固件在长度、CRC、SHA、版本、release、
security 和 Key ID 上完全匹配的有效外部槽；找不到时留在 Bootloader 等待救援。

## 8. 验证命令

```powershell
cmake --build build/release --target validate_images --parallel 1
cmake --build build/release --target test_firmware_security --parallel 1
```

负向测试会分别破坏密文、CRC、SHA、版本、安全版本、发布序号、签名、Nonce、
Key ID 和 Tag，并确认全部被拒绝。主机测试还会直接编译 Bootloader 使用的
micro-ecc、SHA-256 和 Mbed TLS GCM 源码，避免只验证 Python/OpenSSL 一侧。
