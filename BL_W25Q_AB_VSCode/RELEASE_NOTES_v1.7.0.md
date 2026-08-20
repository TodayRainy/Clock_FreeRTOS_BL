# v1.7.0 OTA发布顺序与冗余清理说明

## 1. 本轮目标

旧 OTA 客户端只能用 payload SHA-256 判断“内容相同或不同”，无法判断候选固件
究竟更新还是更旧。因此 COS 上只要出现一个 SHA 不同的旧 XBIN，应用就会把它
当作更新。本轮把“普通发布顺序”“内容身份”和“安全防降级”拆成三项独立信息：

- `release_sequence`：每次发布不同内容都递增，决定自动 OTA 的新旧；
- `image_sha256`：判断是否为同一个 payload，并验证内容完整性；
- `security_version`：只在淘汰存在安全问题的旧固件时提高。

## 2. 二进制格式与兼容性

`release_sequence`复用了 XBIN v2 Header 偏移 28 的原保留字。Header 仍为256字节，
数据仍从偏移4096开始，旧 Bootloader 依赖的地址、长度和 CRC 字段没有移动。
该字段位于 ECDSA-P256 签名覆盖范围内，修改序号并重算 Header CRC 仍无法通过
Bootloader 验签。

旧 XBIN 的该保留字自然为0，新方案从1开始。云端 Manifest 继续使用格式版本1，
并保留旧客户端需要的全部字段，只新增可读的`release_sequence`。因此已经部署的
v1.6.6客户端会忽略新字段并完成迁移，不需要先发布中间版本。

## 3. 自动 OTA 判断

应用和 Bootloader 的 OTA 请求入口共用`firmware_release_policy.h`：

| 候选状态 | 自动 OTA 结果 |
| --- | --- |
| 候选序号更大 | 允许继续安全版本检查、下载或安装 |
| 序号与 SHA 均相同 | 已是当前固件，不擦写外部 Flash |
| 候选序号更小 | 旧包，忽略 |
| 序号相同但 SHA 不同 | 发布冲突，拒绝 |

Bootloader只对应用自动提交的 EEPROM OTA 请求执行该规则。KEY2/USART3人工维护
仍可在明确授权后安装合法签名的旧发布，不影响用户主动降级。

## 4. OTA代码清理

本轮只删除了能够确认没有独立职责的状态：

- 去掉全局`available_candidate`和`candidate_available`，候选只在 OTA 任务的一次
  检查/下载调用链中传递；
- 应用不再同时保存 Manifest 和 Header 两份版本、安全版本、payload CRC 与
  payload SHA；这些固件元数据统一读取 XBIN Header；
- 删除 payload SHA 与 Header/Manifest 的重复比较，只保留签名 Header 的
  payload SHA 和 Manifest 的整包 XBIN SHA；
- Manifest 解析仍保留 URL、XBIN 大小和整包 SHA，它们分别用于下载定位、HTTP
  长度检查、断点记录身份和整包传输校验。

CRC32、payload SHA、整包 SHA、EEPROM 请求和 Bootloader 二次验签没有删除：它们
分别服务于快速误码诊断、正文完整性、断点/整包身份、跨复位交接和最终信任边界，
并非同一层面的重复防御。

## 5. 构建与实板结果

- Bootloader：`v3.4.0-ota-ordering-dev`，29,312字节；
- WeatherClock：`v1.7.0-ota-ordering`，release 1，payload 158,036字节；
- XBIN：162,132字节，CRC32 `0x99B5F2A1`；
- payload SHA-256：
  `DC29C9DABC3BAA29A47A90161A13F0352668B70FFB1A465F55373527286BDBED`；
- XBIN SHA-256：
  `93175987B95E21C42E6C54C89E9C7206622D7091F3E83134D74872E32F240E41`。

主机侧`validate-release`和`security-test-release`全部通过，包括真实 C 发布策略、
断点布局、EEPROM 契约、SHA/ECDSA同源实现和发布序号篡改拒绝测试。

实板从`v1.6.6-ota-resume-test / release 0`经 COS OTA 安装 release 1，Trial 已确认，
最终 BCR 为`Normal / active=A / pending=none / security floor=1`。随后完成两项回归：

1. 云端保持 release 1 同一包，输出
   `firmware is already current (v1.7.0-ota-ordering, release 1)`；
2. Manifest 短暂指向真实 v1.6.6 release 0 旧包，输出
   `older release ignored: current=1 candidate=0`，没有下载正文或提交安装请求。

旧包测试后已把公网 Manifest 恢复为 release 1。全程没有修改 RDP、WRP、Option
Bytes 或 SWD 配置。

