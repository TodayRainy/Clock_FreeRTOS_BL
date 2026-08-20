# Formal OTA 设计与发布说明

OTA 是把 XBIN 送到非活动 W25Q 槽的另一条下载路径，不替代 Bootloader 的认证、
A/B 安装或回退。USART3 和 OTA 最终都提交同一种 Formal XBIN、同一个 BCR 状态机。

## 1. 职责边界

WeatherClock 应用负责：联网、读取 Manifest、判断是否值得下载、断点下载、校验
完整文件 SHA-256、写非活动槽和提交一次性 EEPROM 请求。

Bootloader 负责：重新读取请求、验证外部 Header、ECDSA、AES-GCM、明文摘要、
安全版本和发布策略，再进入 `InstallPending → Trial → Normal`。应用不能仅凭自己
的下载结果直接改写内部 Flash。

## 2. 运行时机

OTA 不是高频轮询任务：

- 本地温湿度与在线天气至少各成功刷新一次后，才允许启动 OTA；
- 应用 ready 后 60 秒进行第一次检查；
- 之后每 24 小时检查一次；
- 下载在独立 FreeRTOS 任务中执行，等待网络时会阻塞让出 CPU；
- 下载完成后不立即重启，三颗板载 LED 全亮提示用户；
- 用户按 RESET 后 Bootloader 才安装。

这样先保证天气时钟主体可用，再把网络和 Flash 带宽让给后台更新。OTA 期间不会
主动刷新在线天气，但已有温湿度和天气页面继续显示；下一次天气刷新仍按原周期。

## 3. Manifest v2

当前公网地址：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/manifest.json`

当前结构：

```json
{
  "manifest_version": 2,
  "product": "clock-f407",
  "channel": "stable",
  "firmware_version": "formal-1.0.1",
  "release_sequence": 5,
  "security_version": 2,
  "xbin_format": "formal",
  "format_revision": 1,
  "xbin_size": 161776,
  "xbin_sha256": "C3FD621C1D52CD46E4CD4C6D0B0AC26FD4DA660C4C4FFB1A6D53CCE1ECFCAFE0",
  "payload_size": 157680,
  "payload_crc32": "6927330E",
  "payload_sha256": "D0986C7A785B98B4A9907A221038D76CBDF62D2105A1B8638B11134B9CFF88E5",
  "signing_key_id": "CA9EEE70",
  "encryption": "AES-256-GCM",
  "encryption_key_id": "9B5260DC",
  "download_url": "https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/releases/weatherclock-formal-1.0.1-r5.xbin"
}
```

应用只接受 `manifest_version=2`、正确产品、`xbin_format=formal`、revision 1 和
HTTPS URL。旧 Manifest 或旧 XBIN 不进入下载兼容分支。

## 4. 什么叫“新版本”

自动 OTA 主要比较 `release_sequence`，不是只看文件 SHA：

- 候选 sequence 更大：允许更新；
- sequence 更小：是旧版本，忽略；
- sequence 相同且 SHA 相同：同一发布，忽略；
- sequence 相同但 SHA 不同：发布冲突，拒绝。

`security_version` 是另一条单调安全下限。候选低于设备已确认下限时，Bootloader
拒绝安装。用户主动回到旧功能版应走 KEY2 + USART3 人工维护流程，自动 OTA 不
承担回退职责。

## 5. 下载与掉电续传

Manifest 通过后，应用先读取 320 字节 Formal Header，再选择非活动槽。下载正文
使用 1024 字节网络块，ESP 链路临时切到 921600，W25Q SPI1 保持 21 MHz。

EEPROM 每 32 KiB 保存一次进度，包括目标槽、XBIN 大小、Header CRC、文件 SHA、
已写偏移和前缀 CRC。掉电后重新检查这些身份字段和已写前缀，匹配才继续；不匹配
就重新擦写候选区域。进度记录频率在重复写入与恢复粒度之间取折中，不对每 1 KiB
写 EEPROM。

提交顺序刻意把 Header 放到最后：

1. 擦除需要改写的非活动槽范围；
2. 下载并写入填充与密文；
3. 校验完整 XBIN 文件 SHA-256；
4. 最后写 320 字节 Header；
5. 写 OTA 安装请求；
6. 点亮三颗 LED。

因此掉电时不完整槽通常连有效 Header 都没有，Bootloader 不会误安装。

## 6. 发布流程

先配置版本并构建：

```powershell
cmake --preset release `
  '-DWEATHERCLOCK_VERSION=formal-1.0.1' `
  '-DFIRMWARE_RELEASE_SEQUENCE=5' `
  '-DFIRMWARE_SECURITY_VERSION=2'
cmake --build --preset release
cmake --build build/release --target test_firmware_security --parallel 1
```

生成 Manifest：

```powershell
python tools/make_ota_manifest.py `
  build/release/WeatherClock/stm32f407_upgrade.xbin `
  ota/manifest.json `
  --url https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/releases/weatherclock-formal-1.0.1-r5.xbin `
  --aes-tool $env:LOCALAPPDATA/ClockF407Tools/firmware_aes_gcm_tool.exe `
  --encryption-key keys/formal_firmware_aes256.key
```

COS 发布必须先固件、后 Manifest：

```powershell
coscli cp build/release/WeatherClock/stm32f407_upgrade.xbin `
  cos://codex_ota/clock-f407/releases/weatherclock-formal-1.0.1-r5.xbin
coscli ls cos://codex_ota/clock-f407/releases/weatherclock-formal-1.0.1-r5.xbin
coscli cp ota/manifest.json cos://codex_ota/clock-f407/manifest.json
```

固件对象使用不可变的新文件名。不要先覆盖 Manifest，否则设备可能在固件尚未
上传完成时得到一个暂时无效的下载 URL。

## 7. 当前发布验证

2026-08-20 已发布 `formal-1.0.1 / release 5 / security 2`：

- XBIN 远端长度161,776字节，下载回读SHA-256与本地文件一致；
- Manifest 和 XBIN URL 均返回HTTP 200，Manifest字段与XBIN Header一致；
- 实板先运行`formal-1.0.0 / release 4`，通过热点联网、读取Manifest并下载release 5；
- 复位后Bootloader完成OTA安装，Trial应用完成健康确认并回到Normal；
- 最终内部应用及A/B两槽均为release 5，日常检查不会重复下载；
- USART3对release 5完成完整写入、差异写入和全块断点复用验证。

release 4对象只作为历史回归包保留，不再是当前Manifest指向的发布。

## 8. 已知限制

- ESP-AT 与天气 API 共用网络模块，OTA 下载时在线天气请求需要排队；
- 下载端验证文件 SHA 和 Header 一致性，真正的私钥授权和 GCM 认证仍在复位后；
- 只有一个稳定频道，没有灰度比例、设备分组或服务器端回滚编排；
- COS 公开读适合个人项目；商业产品应增加访问控制、审计和独立发布服务；
- 当前 AES 密钥对同系列设备共用，不具备一机一密和硬件安全存储。
