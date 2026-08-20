# XBIN v3 固件加密设计与使用说明

## 1. 第一阶段目标

本阶段在现有签名、A/B、断点续传和OTA流程上增加固件保密能力，不修改内部
Flash布局，也不启用RDP、WRP或其他不可逆Option Bytes。J-Link/SWD救援继续
保留，这是当前个人学习板“不变砖”要求的最高优先级。

加密和签名解决的是两个不同问题：

- AES-256-GCM让公开存放在COS、串口传输或外部W25Q中的升级正文不再直接暴露
  程序机器码；
- ECDSA-P256继续决定“这个版本是否由项目所有者授权”。知道AES密钥不能替代
  签名私钥发布任意固件；
- CRC32用于发现普通传输错误，SHA-256用于核对解密后的完整内容，它们都不替代
  AES-GCM认证或ECDSA签名。

## 2. XBIN v3 布局

```text
0x0000..0x00FF  原有256字节签名Header（格式仍为v2）
0x0100..0x013F  64字节ENC3加密描述
0x0140..0x0FFF  零填充
0x1000..末尾    与应用BIN等长的AES-256-GCM密文
```

保留原有签名Header的好处是版本号、发布序号、防降级、安全版本和ECDSA签名契约
无需复制一份。`ENC3`描述包含算法、AES密钥ID、12字节随机Nonce、16字节GCM Tag、
保留区和描述CRC32。

GCM附加认证数据（AAD）是“完整256字节签名Header + ENC3前32字节”。因此修改
版本、摘要、算法、密钥ID或Nonce都会使Tag失效；修改密文或Tag本身也会在认证
阶段被拒绝。每次打包使用系统密码学随机源生成新Nonce，相同应用重复打包也会
得到不同密文。

## 3. 设备端安装顺序

外部W25Q槽保存完整的加密XBIN，STM32内部应用区仍保存可直接执行的明文。安装
流程为：

1. 校验签名Header、ENC3结构和对应密钥ID；
2. 顺序读取外部密文，流式解密并验证GCM Tag，同时计算明文CRC32、SHA-256并
   检查向量表；
3. 只有完整候选第一次验证通过，才擦除内部应用Header和应用区；
4. 第二次流式解密并把明文写入内部Flash；
5. 再次验证GCM Tag、内部明文摘要和向量表；
6. 最后写入原有256字节有效Header，完成原子提交。

第二次认证或写入中途掉电时，内部Header仍无效。Bootloader下次启动会从已经
验证过的活动外部槽恢复，不会把半写入的内部应用当作有效程序。外部槽快照仍是
明文签名v2，这是为了让已确认的运行镜像成为简单可靠的回退源。

## 4. OTA与USART3的行为

WeatherClock应用和PC上位机不持有AES密钥，也不自行解密固件：

- OTA用320字节Range请求读取签名Header和ENC3描述；下载时只处理密文字节；
- OTA完成后核对Manifest中的完整XBIN SHA-256，明文摘要与GCM Tag延后交给
  Bootloader；
- USART3上位机校验Header/ENC3结构并按原协议上传密文；断点续传的4 KiB分块CRC
  直接覆盖密文，不影响恢复算法；
- 两条路径都先写正文和Header区填充，最后提交包含ENC3的前320字节。有效签名
  Header在最后写入，半包不会成为候选。

明文签名v2仍被新Bootloader接受，便于迁移和恢复；生成脚本的`--no-encrypt`
选项可显式产生这种过渡包。旧Bootloader遇到加密v3时会把密文当作正文，其
CRC32/SHA-256必然不匹配，因此会安全拒绝，而不是错误安装。

## 5. 密钥与构建

DEV首次配置会在Git忽略的`.keys/`目录生成：

```text
.keys/dev_firmware_private.pem
.keys/dev_firmware_public.pem
.keys/dev_firmware_aes256.key
```

AES文件必须恰好32字节。构建只把AES密钥编入Bootloader，WeatherClock、
SerialUpgrader、Manifest和XBIN中都不包含明文密钥。常规验证命令为：

```powershell
cmake --preset release
cmake --build --preset validate-release
cmake --build --preset security-test-release
dotnet build .\SerialUpgrader\SerialUpgrader.csproj -c Release
```

PROD构建要求三项外部路径：

```powershell
$env:CLOCK_FW_SIGNING_KEY='D:\secure\clock_private.pem'
$env:CLOCK_FW_VERIFY_KEY='D:\secure\clock_public.pem'
$env:CLOCK_FW_ENCRYPTION_KEY='D:\secure\clock_aes256.key'
cmake --preset release-prod
cmake --build --preset validate-release-prod
```

私钥和AES密钥都需要离线备份。丢失签名私钥将无法签发新版本；丢失AES密钥将
无法为现有Bootloader生成可解密的v3包。当前实现只内置一把AES密钥，轮换密钥
属于后续阶段，不在这次简单实现中预埋复杂状态机。

## 6. 安全边界

当前方案主要防止别人从公开OTA文件、串口抓包或拆下外部Flash后直接获得固件。
为了保留J-Link救援，MCU仍是RDP Level 0，内部Flash也保存运行所需明文。因此
能够接触板卡并通过SWD读取芯片的人，仍可能提取Bootloader中的AES密钥和内部
应用。这是本阶段主动接受的取舍，不应把它描述成物理防拆方案。

如果以后确实需要抵抗物理提取，应单独规划RDP、Bootloader写保护、安全密钥
注入和量产救援流程；这些设置可能影响甚至永久关闭当前救援能力，不能作为本次
加密开发的顺手改动。

## 7. 安全部署顺序

板上当前旧Bootloader不认识ENC3，所以迁移必须按以下顺序：

1. 确认J-Link连接，保留并校验当前512 KiB整片救援镜像；
2. 只烧录并verify新的`v3.5.0-encrypted` Bootloader前48 KiB；
3. 复位确认它仍能验证并启动当前明文签名v2应用；
4. 再通过USART3或OTA把`v1.8.0-encrypted / release 2`写入非活动槽；
5. 由Bootloader安装、进入Trial并等待应用确认；
6. 最后读取BCR、两槽状态和运行日志，必要时再生成新的整片救援镜像。

在步骤2之前不能发布指向v3密文的公网Manifest。本项目不会修改Option Bytes；
任何阶段失败都可用现有J-Link恢复脚本重写`0x08000000`起的已知良好镜像。

## 8. 已覆盖的自动化测试

当前测试会验证：正常加密包可还原为原始BIN；Bootloader同源流式AES-GCM实现
接受正确Tag并拒绝错误Tag；密文、Nonce、密钥ID、Tag、签名、元数据和发布序号
任一被修改均拒绝；旧Bootloader语义下密文CRC不可能被当作有效明文；断点续传
继续按密文块定位。

2026-08-20实板已完成两条路径：USART3安装`v1.8.0-encrypted / release 2`，随后
COS OTA安装`v1.8.1-encrypted-ota-test / release 3`。最终BCR为Normal、active A，
A/B槽均完整有效；详细日志、耗时和救援镜像见`RELEASE_NOTES_v1.8.0.md`与
`SAFETY_BASELINE.md`。
