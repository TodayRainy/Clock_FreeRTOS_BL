# 固件签名、IWDG 与维护权限说明

本文记录 `v3.3.0-resume-dev/prod` 的签名、防降级、IWDG和维护权限、代码边界、构建方式和部署
注意事项。原有外部 A/B 状态机、掉电提交顺序和高速串口协议没有被替换。

## 1. 这次解决了什么

原方案的 CRC32 很适合发现传输错误，但任何人只要能修改固件，也能重新计算
CRC32。新方案把职责分成三层：

| 机制 | 回答的问题 | 不能解决的问题 |
| --- | --- | --- |
| CRC32 | 数据在传输/存储时是否意外损坏 | 不能证明固件是谁生成的 |
| SHA-256 | 当前数据是否与Header记录的摘要一致 | 摘要本身可被攻击者一起替换 |
| ECDSA-P256 | Header和其中的SHA-256是否由持有私钥的人签发 | 不加密固件；旧签名包由安全版本策略处理 |

Bootloader只保存公钥。生成固件的一端保存私钥并对XBIN Header签名，设备端无法
从公钥反推出私钥。攻击者即使同时修改应用、CRC32、SHA-256和Header CRC32，
仍然不能生成可通过设备验签的新签名。

另外两项改进分别覆盖运行时和维护面：

- IWDG在Bootloader启动后一直运行，应用必须继续喂狗；应用核心初始化超时或
  FreeRTOS调度停止时，硬件会复位并让Trial次数继续收敛到自动回退。
- DEV构建保留方便调试的维护接口；PROD构建只有启动时持续按住KEY2，确认人在
  设备旁边后，才开放原始读取/写入和旧内部Flash命令。
- 独立的安全版本下限阻止普通链路重新安装已淘汰的签名包；KEY2仍可明确授权
  用户回退，而Trial失败自动恢复旧活动槽不受限制。

## 2. XBIN v2 格式

XBIN仍由4096字节Header区和应用BIN组成，应用数据仍从偏移`0x1000`开始。
`magic_header_t`保持256字节，并保留旧版中数据描述字段的偏移，便于先用旧
Bootloader安装一次新签名应用，再迁移Bootloader本身。

| 偏移 | 长度 | 字段 |
| ---: | ---: | --- |
| 0 | 4 | `MAGI`魔数 |
| 4 | 4 | 格式版本，当前为2 |
| 8 | 4 | 结构大小，固定256 |
| 12 | 4 | 摘要算法，1表示SHA-256 |
| 16 | 4 | 签名算法，1表示ECDSA-P256/SHA-256 |
| 20 | 4 | 公钥ID，便于日志识别密钥 |
| 24 | 4 | 独立安全版本，用于防降级 |
| 32..48 | 20 | 类型、偏移、目标地址、数据长度、CRC32 |
| 52 | 32 | 应用数据SHA-256 |
| 96 | 64 | ASCII版本字符串 |
| 160 | 64 | ECDSA原始签名，32字节`r`加32字节`s` |
| 248 | 4 | 内部Header目标地址`0x0800C000` |
| 252 | 4 | Header前252字节CRC32 |

签名输入是域分隔字符串`ClockFW-XBIN-v2`和规范化后的256字节Header。规范化
时只把签名字段与Header CRC32清零；应用SHA-256、显示版本、安全版本、地址、长度和公钥ID都
被签名覆盖。Header CRC32最后计算，主要用于快速发现普通误码。

设备接受一个槽之前依次确认：

1. Header固定字段、地址、长度和Header CRC32正确；
2. `key_id`与Bootloader内置公钥一致，ECDSA-P256签名通过；
3. 顺序读取应用，一次性计算CRC32和SHA-256；
4. 初始MSP位于SRAM/CCMRAM且8字节对齐；
5. Reset_Handler带Thumb位并位于本应用镜像内。

安装到内部Flash后会重新执行数据摘要和向量表检查，最后才写入有效Header。
因此“外部槽校验正确但复制过程出错”也不会被提交为可启动应用。

## 3. 密钥与构建配置

### 3.1 DEV

普通`debug`和`release`预设使用DEV配置。首次运行CMake时，如果`.keys/`下没有
密钥，会生成一对本机P-256开发密钥：

```text
.keys/dev_firmware_private.pem
.keys/dev_firmware_public.pem
```

整个`.keys/`目录被Git忽略。私钥丢失后，已经烧入对应公钥的Bootloader不会
接受另一把密钥签发的新固件；开发板仍可通过J-Link重写Bootloader恢复。若要
长期使用同一开发固件链，应单独备份这两个文件。

```powershell
cmake --preset release
cmake --build --preset validate-release --parallel
cmake --build --preset security-test-release --parallel
```

### 3.2 PROD

PROD不会自动生成密钥，必须显式提供一对匹配的PEM文件。建议签名私钥保存在
构建机之外或离线介质中；源码仓库和设备只需要公钥。

```powershell
$env:CLOCK_FW_SIGNING_KEY='D:\secure\clock_prod_private.pem'
$env:CLOCK_FW_VERIFY_KEY='D:\secure\clock_prod_public.pem'

cmake --preset release-prod
cmake --build --preset validate-release-prod --parallel
```

同一套构建会完成两件事：把公钥坐标和`key_id`生成到构建目录并编入
Bootloader；再用对应私钥对WeatherClock XBIN签名。CMake会拒绝缺少文件、
公私钥不匹配或不是P-256的配置。

## 4. IWDG 与 Trial 健康确认

`shared/watchdog.c`由Bootloader和WeatherClock共同编译。当前使用最大分频与
最大重装值，按典型32 kHz LSI估算约32秒超时。IWDG一旦启动就不能由软件关闭，
所以跳转应用前不会尝试“反初始化”；应用会重新写入相同配置并继续刷新。

Bootloader在以下正常长操作中刷新IWDG：

- 等待协议和主循环；
- W25Q块擦除忙等待；
- 外部槽CRC32/SHA-256扫描；
- 外部槽向内部Flash复制；
- 内部Flash复核和外部快照。

WeatherClock在启动调度器前创建健康监控任务：

1. 最多给核心初始化20秒宽限，并每秒刷新IWDG；
2. `board_init()`、`ui_init()`和欢迎页显示成功后调用
   `health_monitor_mark_ready()`；
3. 网络连接不是“固件可启动”的条件，临时断网不会触发回退；
4. A/B确认任务等到ready，再额外观察调度器5秒，才把Trial提交为Normal；
5. 若20秒仍未ready，监控任务停止喂狗，IWDG最终复位系统。

这是一套有明确边界的健康标准：它能覆盖核心初始化卡死、调度器不运行和确认前
反复复位；它不是所有业务任务的心跳框架，也不会因为天气服务器或Wi-Fi不可用
而回退。后续若要扩大健康条件，应只加入确实代表“这版固件不可用”的稳定信号。

DEV构建在CPU被调试器暂停时冻结IWDG，方便单步调试；PROD不设置该调试冻结位。

## 5. DEV/PROD 维护权限

正常升级需要的状态查询、W25擦除/写入非活动槽、签名校验、`SLOT_ACTIVATE`、
BOOT和RESET在两种配置中都可用。以下接口属于工程维护命令：

- W25原始读取；
- EEPROM原始读取和受限写入；
- 手工设置活动槽与内部固件快照；
- 旧版内部Flash ERASE/PROGRAM/VERIFY。

DEV始终开放这些命令。PROD只有长按KEY2完整通过3秒进入窗口时开放；仅在串口
窗口发送数据进入Bootloader不会解锁。未授权调用返回`DENIED`错误码。

即使在DEV中，EEPROM原始写入也只允许`0xFC00..0xFCFF`维护区，不能覆盖位于
`0xFD00/0xFD80`的两份BCR。Bootloader内部48 KiB仍不能通过在线协议擦写。

### 5.1 防降级与人工回退

XBIN中的`security_version`与`v1.4.0`等显示版本完全独立。BCR保存已确认的
`minimum_security_version`和当前Trial的`pending_security_version`：

1. 正常`SLOT_ACTIVATE`只接受候选版本不低于已确认下限的签名镜像；
2. 新值在InstallPending和Trial阶段只作为pending保存，不提前提高下限；
3. Trial失败仍可安装旧active槽，成功确认时下限才取两者较大值；
4. KEY2物理授权可安装较低的合法签名镜像，但确认后原下限仍保持不变；
5. J-Link始终可以作为开发板最终救援手段，本方案不写Option Bytes。

因此功能版本可以通过重新签名的回退包退回；若旧包已经低于安全下限，则必须
发布一个安全版本不低于下限的新回退包，或由设备所有者在现场使用KEY2授权。

## 6. 迁移现有开发板的安全顺序

当前板上是无签名v1 Header和旧Bootloader时，不应直接先刷v3 Bootloader；v3会
正确拒绝旧镜像，设备虽可用J-Link救援，但会暂时停在Bootloader。

平滑迁移顺序是：

1. 保留J-Link并先读取512 KiB内部Flash备份；
2. 使用旧Bootloader和新版SerialUpgrader，把同一个签名XBIN依次完成两次A/B
   升级，使内部应用和两个外部槽都变为v2签名镜像；
3. 每次升级后确认BCR回到Normal、应用正常启动；
4. 只用J-Link写入v3 Bootloader的前48 KiB并立即verify；
5. 复位后查询版本、BCR和两槽签名状态，再做一次A到B/B到A回归。

旧Bootloader能执行这次过渡，是因为v2仍保留数据类型、偏移、地址、长度和CRC32
的原字段位置，Header CRC32也仍覆盖前252字节。旧Bootloader只会忽略新签名；
一旦v3安装完成，所有后续镜像都必须通过真正的签名验证。

## 7. 自动化验证

`security-test-release`包含两类互补测试：

- Python/OpenSSL测试分别篡改Payload、重算CRC32和SHA-256、修改元数据、修改
  安全版本和签名，确认所有伪造镜像都被拒绝，并锁定v2.1迁移字段偏移；
- 宿主机GCC直接编译Bootloader使用的`sha256.c`、`uECC.c`和
  `firmware_auth.c`，确认同一份C代码接受真实XBIN并拒绝修改后的签名/摘要。
- 另一份宿主机C测试覆盖正常升级、降级拒绝、KEY2授权和下限只增不减规则。

Debug、Release、PROD Release仍分别执行链接区、MSP、Reset_Handler、Payload
一致性、CRC32、SHA-256、公钥ID和ECDSA签名检查。

### 7.1 实板验证（2026-08-18）

自动化测试之外，当前硬件还完成了以下真实链路测试：

- 从无签名v2.1平滑迁移两个外部槽，再用J-Link写入v3 Bootloader；
- v3签名升级、Trial健康确认和A/B切换完整跑通；
- 一个重新计算CRC32、SHA-256和Header CRC但没有新私钥签名的伪造XBIN被STM32
  在激活阶段拒绝，活动槽没有改变；
- 故意安装最初存在IWDG初始化缺陷的应用后，三次Trial耗尽并自动恢复原活动槽；
- PROD未按KEY2时只读维护探针返回`DENIED`，DEV返回`ALLOWED`；
- 应用连续运行超过IWDG完整周期后，J-Link确认PC仍位于应用区。

2026-08-19继续完成防降级实板验证：旧BCR无损迁移为安全下限0；安全版本1在
Trial确认前保持`0/1`，确认后提升到1；正常链路完整接收安全版本0后在激活阶段
返回`DENIED`且活动槽不变；通过J-Link置位与KEY2相同的会话RAM标志后，版本0
可安装但下限仍为1。最后再做两次版本1升级，两槽均恢复为当前签名镜像。

上位机可用下面的无写入命令复核维护权限。它只尝试读取W25Q A槽首字节，结束后
发送BOOT命令：

```powershell
dotnet .\SerialUpgrader\bin\Release\net8.0-windows\SerialUpgrader.dll `
  --maintenance-probe COM18
```

详细序列号、CRC、备份哈希和最终BCR见`SAFETY_BASELINE.md`。

## 8. 当前尚未覆盖的工业化能力

安全版本下限保存在EEPROM双副本BCR中，没有使用不可逆硬件计数器；拥有J-Link的
设备所有者仍能恢复或重置开发板。XBIN内容没有加密，Bootloader本身也仍只通过
SWD更新，这些是后续可以独立讨论的保密和Bootloader自身保护主题。

外部槽断点续传只用分块CRC定位已经存在的数据，最终信任边界没有改变；完整流程
和中断测试见`RESUMABLE_UPGRADE_GUIDE.md`。

micro-ecc按其BSD-2-Clause许可原样放在
`STM32F4_Bootloader/third_lib/micro-ecc/`，来源提交和裁剪方式记录在同目录
`VENDOR.md`。当前只编译secp256r1验签所需能力。
