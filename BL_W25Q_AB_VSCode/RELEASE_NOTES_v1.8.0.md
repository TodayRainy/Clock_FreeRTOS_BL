# v1.8.x 加密固件开发与实板报告

## Git基线

加密开发开始前，先把上一轮OTA排序与主程序优化提交为
`c60cf74 feat: refine OTA release ordering and runtime flow`，打注释标签
`v1.7.0-ota-ordering`，并把`main`和标签推送到
`TodayRainy/Clock_FreeRTOS_BL`。本轮没有混入`BL`、`BL_W25Q_AB`等历史目录的
本地修改。

## 实现内容

- 新增XBIN v3容器：保留256字节ECDSA签名Header，在偏移256增加64字节ENC3
  描述，偏移4096后的正文使用AES-256-GCM加密；
- AAD覆盖签名Header、算法、密钥ID和随机Nonce，密文、Tag和关键元数据任一修改
  都会被拒绝；
- Bootloader使用裁剪后的Mbed TLS 3.6.5同源代码流式解密，外部槽先完整认证，
  安装时再次认证，内部有效Header最后提交；
- OTA应用只验证完整密文XBIN SHA-256，明文CRC/SHA和GCM Tag由Bootloader负责；
- SerialUpgrader识别ENC3，继续按密文做4 KiB断点扫描，并在最后提交320字节头；
- DEV自动生成Git忽略的32字节AES密钥；PROD必须显式提供签名私钥、公钥和AES
  密钥，构建系统拒绝DEV密钥路径；
- 保留明文签名v2兼容与`--no-encrypt`过渡出口，不写Option Bytes。

## 自动化验证

Release构建0错误、0警告。最终默认产物为：

- Bootloader：35,036字节/48 KiB，余14,116字节；
- WeatherClock：158,452字节/448 KiB；
- 静态RAM：72,144字节/128 KiB；
- 默认XBIN：`v1.8.0-encrypted / release 2`，总长162,548字节；
- ECDSA开发公钥ID`0x7F9F8899`，AES开发密钥ID`0x10DE0ABA`。

`validate-release`与`security-test-release`全部通过，包括镜像边界、向量表、
ECDSA/SHA-256同源C测试、Bootloader流式AES-GCM同源C测试、防降级、OTA二进制
契约、断点续传，以及密文、重算CRC/SHA、元数据、安全版本、发布序号、签名、
Nonce、AES密钥ID和Tag篡改拒绝。SerialUpgrader .NET 8 Release为0错误、0警告。

## J-Link迁移

J-Link V9.40（S/N `69408257`）在VTref 3.293 V、SWD 1 MHz下正常连接。写入前，
板上512 KiB与
`stm32f407_ota_ordering_v1_7_final_20260820.bin`逐字节`Verify successful`。
随后以100 kHz只写35,036字节新Bootloader，J-Link报告实际影响前三个16 KiB扇区
并立即verify成功；应用Header和应用区未擦除。复位后原v1.7明文签名应用正常
输出温湿度，证明迁移兼容路径成立。

## USART3加密升级实测

COM29读取到`v3.5.0-encrypted-dev`和原始`Normal / active=A / sequence=303`。
加密release 2以2 Mbps写入非活动B槽：

- 串口传输及W25编程2.09秒；
- STM32全量解密、认证和激活检查0.60秒；
- 含握手总耗时2.82秒；
- 安装和Trial确认后为`Normal / active=B / sequence=307`；
- B槽版本`v1.8.0-encrypted`，CRC32`0x21897F46`，A槽v1.7保持完整。

## COS加密OTA实测

独立构建`v1.8.1-encrypted-ota-test / release 3`，XBIN总长162,556字节，整包
SHA-256为`C72A930248C4E30EA864ADEBCC4A1CB4B68A1A52C38AAA10EFA2142AF1B65467`。
先上传唯一对象，COS ETag与本地MD5一致；公网Range请求返回`206`、320字节和
正确总长后，才最后更新Manifest。

应用收到`u`后识别release 3，从偏移4096下载到非活动A槽，USART2正文阶段为
921600。约19秒后输出`verify encrypted payload=deferred, xbin=ok`和
`package verified; press RESET to install`，期间当前应用与AHT20刷新继续运行。
普通RESET后Bootloader接受OTA请求、认证解密、擦写内部应用，进入Trial；应用
启动约5秒后确认成功。最终状态：

- `Normal / active=A / pending=none / attempts=0 / sequence=311`；
- Slot A：release 3，158,460字节，CRC32`0x0ABA14CD`；
- Slot B：release 2，158,452字节，CRC32`0x21897F46`；
- 当前公网Manifest为release 3，两个外部槽均可用。

## 救援与安全边界

最终512 KiB镜像为
`../.device-backups/stm32f407_encrypted_v3_5_final_20260820.bin`，SHA-256
`D19567D780278413A892664B26EE962962D8F4337CD97BF82C76D35F473726F8`，保存后已
立即对芯片完整verify。恢复脚本为
`tools/jlink/restore_encrypted_v3_5_final.jlink`。

本轮始终保持RDP Level 0，没有修改WRP或关闭SWD。当前加密保护公开OTA文件、
串口传输和外部Flash中的正文；它不抵抗能够通过SWD读取内部明文与Bootloader
密钥的物理攻击者。这是为保留可靠救援而明确接受的第一阶段边界。
