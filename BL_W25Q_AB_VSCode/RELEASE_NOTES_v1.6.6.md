# v1.6.6 OTA阶段说明与实板报告

- 日期：2026-08-19
- 项目：`BL_W25Q_AB_VSCode`
- 应用版本：`v1.6.6-ota-resume-test`
- Bootloader：`v3.3.0-resume-dev/prod`

## 1. 本阶段完成了什么

这一版本在原有“串口升级 + 外部双W25Q128 A/B槽 + Trial自动回退”基础上，
加入了一条完整的COS OTA路径。天气时钟应用负责联网、下载和暂存，Bootloader
仍独占内部Flash安装权。两者通过BL24C512中的固定记录交接，不让联网代码直接
修改启动状态机。

最终用户流程是：

1. 天气时钟连接Wi-Fi，启动60秒后检查一次，以后每24小时检查一次；
2. 检测到新固件后，自动下载到当前活动槽之外的W25Q128；
3. 应用计算CRC32、payload SHA-256和整包SHA-256，全部通过才提交Header和
   EEPROM安装请求；
4. 三颗板载LED常亮提示“固件已准备”，当前版本继续正常运行；
5. 用户按RESET，Bootloader重新校验格式、ECDSA-P256签名、防降级版本、摘要和
   向量表，再安装到STM32内部应用区；
6. 新应用进入Trial，稳定运行5秒后确认；确认失败则恢复已确认槽。

USART1输入`u`可立即触发一次检查/下载，适合开发和手动更新。USART3原有有线
升级、KEY2维护入口以及J-Link救援全部保留，OTA不是唯一升级通道。

## 2. 代码结构

| 文件 | 职责 |
| --- | --- |
| `WeatherClock/app/ota.c` | 定时检查清单、选择非活动槽、断点续传、三项摘要校验、最后提交Header和安装请求 |
| `WeatherClock/driver/esp_at/esp_at.c` | ESP-AT命令互斥、HTTPS/Range、被动接收、921600临时提速及115200恢复 |
| `shared/ota_contract.h` | 应用和Bootloader共用的128字节进度/安装请求格式及EEPROM地址 |
| `STM32F4_Bootloader/app/ota_request.c` | 读取一次性请求，并用Bootloader自己的可信校验链复核外部槽 |
| `tools/make_ota_manifest.py` | 从已签名XBIN生成受控的扁平COS清单 |
| `tools/test_ota_contract.c` | 在宿主机验证持久化结构大小、字段和CRC契约 |
| `ota/manifest.json` | 当前已发布固件的非敏感元数据和公网下载地址 |

OTA任务与天气、SNTP共用ESP互斥锁。下载时网络业务会等待，但显示、传感器和
高优先级健康监控仍由FreeRTOS调度。大文件正文临时把USART2从115200切到
921600；HTTP会话关闭后立即恢复115200。外部Flash写入结束后反初始化SPI1和
DMA2，PA5/PA6/PA7恢复为模拟输入。

## 3. 掉电恢复与提交边界

外部槽的前256字节Header最后写入。正文未完成或摘要失败时，该槽不会被视为
有效固件，也不会生成安装请求。每完成32 KiB，应用把一个128字节进度记录写到
BL24C512 `0xFC00`；下一次运行只有在固件SHA、目标槽、安全版本和记录CRC都
匹配时才从检查点继续。

安装请求位于`0xFC80`。它只是“应用已经下载完”的交接信息，不是信任凭据。
Bootloader会重新执行XBIN Header校验、ECDSA-P256验签、payload CRC/SHA、向量
表检查和防降级判断。请求非法时清除请求并启动旧应用；下载中掉电不改变BCR，
安装中掉电由`InstallPending`重装，Trial失败由`RollbackPending`恢复。

KEY2人工维护启动不会消费OTA请求。项目没有写RDP、WRP或其他Option Bytes，
J-Link始终可以重写`0x08000000`的Bootloader；旧的USART3升级路径也可恢复两颗
外部槽。因此本阶段没有把联网功能变成新的单点故障。

## 4. 1024字节CIPRECVDATA设计

当前一次`AT+CIPRECVDATA`最多请求1024字节，和以下缓冲区共同形成明确边界：

- OTA流缓冲：1024字节；
- HTTP会话暂存：1024字节；
- AT响应/HTTP头缓冲：各1024字节；
- USART2 RX环形缓冲：2048字节。

ESP可能暂时返回`busy p...`或没有可取数据。驱动会完整消费这条响应，在同一个
绝对截止时间内短暂重试；总超时不会因重试次数成倍增加。若数据前缀、正文长度
或最终`OK`的边界已经不明确，则终止本轮HTTP并保留最近检查点。这里没有通过
继续扩大请求长度换速度，因为超过本地缓冲能力会增加环形缓冲溢出、AT正文和
控制行混淆的概率。

## 5. 构建与产物

本次DEV Release使用安全版本1和开发测试公钥ID`0x7F9F8899`。私钥位于被Git
忽略的`.keys/`，没有进入提交。构建命令为：

```powershell
cmake --preset release -DWEATHERCLOCK_VERSION=v1.6.6-ota-resume-test
cmake --build --preset validate-release --parallel
cmake --build --preset security-test-release --parallel
```

| 产物 | 大小 | SHA-256/摘要 |
| --- | ---: | --- |
| Bootloader BIN | 29,112字节 | `AA504C964D3BE777F1ED5CEAB19D3E5C4BDD866B32245DB11508E556FCA8E382` |
| WeatherClock payload | 322,468字节 | `196FD4DDD5C7565DA9211C5327EE985D8A371D0911A36F7C62F2C45B16E831E3` |
| 签名XBIN | 326,564字节 | `C1DB903C5A016FE6F73F3A4A2995100904C2A815101C5F6F6085360EB10CB4C9` |

XBIN payload CRC32为`0x33933CF0`。Release ELF占用为：

| 镜像 | Flash | RAM |
| --- | ---: | ---: |
| Bootloader | 29,112 / 49,152字节（59.23%） | 36,076 / 131,072字节（27.52%） |
| WeatherClock | 322,468 / 458,752字节（70.29%） | 122,928 / 131,072字节（93.79%） |

应用RAM余量约8 KiB，后续添加常驻缓冲区前必须重新查看ELF size。当前1024字节
拉取没有增加新的临时大数组，RAM占用相对前一版不变。

## 6. 自动化与实板结果

主机侧`security-test-release`已通过：

- Bootloader和应用链接区、向量表及XBIN一致性检查；
- SHA-256/ECDSA-P256同源C实现测试；
- payload、摘要、元数据和签名篡改拒绝测试；
- 防降级策略宿主机C测试；
- OTA EEPROM契约测试。

实板使用STM32F407VET6、双W25Q128、BL24C512、ESP32-C3 ESP-AT v4.1.1、
J-Link和USART1/USART3完成：

1. `v1.6.5`到`v1.6.6`从零下载326,564字节，三项摘要全部通过；
2. 在36,864字节检查点后复位，Bootloader安全启动旧应用；
3. 应用发现ESP仍在921600并自动恢复到115200；
4. 再次触发后从36,864字节续传，约289,700字节耗时约10.1秒，约28 KiB/s；
5. 下载结束后USART2恢复115200，三颗LED常亮，应用未自动重启；
6. 按RESET后Bootloader独立验签并安装，Trial确认成功；
7. 最终为`Normal / active=A / pending=none`，再次检查报告固件已是当前版本；
8. J-Link以SWD 4 MHz保持可连接，VTref约3.29 V；USART3升级路径仍可用。

## 7. COS发布状态

当前清单：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/manifest.json`

当前固件：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/releases/weatherclock-v1.6.6-ota-resume-test.xbin`

发布顺序固定为“先上传唯一文件名XBIN并验证GET/Range，再覆盖manifest”。Bucket
保持公有读、私有写。COS Secret ID/Secret Key只存在于开发机COSCLI配置，不在
源码、清单或固件中。仓库只保存非敏感`manifest.json`，不保存构建出的XBIN。

## 8. 当前已知边界

- OTA只支持HTTPS GET、`Content-Length`和HTTP Range；不处理重定向与chunked
  response，当前COS响应满足这些条件；
- 自动检查会在发现更新后直接下载，但不会自动复位安装；
- Wi-Fi凭据目前仍是编译期配置，适合当前个人学习板，不是量产配网方案；
- OTA进度记录为单副本，损坏只会导致重新下载；关键BCR仍是双副本；
- Bootloader本身不支持OTA自更新，只能由J-Link等SWD工具维护；
- PROD发布必须从外部提供并妥善保管签名私钥，不能使用仓库外的DEV测试密钥
  作为产品密钥。

更完整的OTA设计见`OTA_GUIDE.md`，签名与权限见`SECURE_BOOT_GUIDE.md`，串口
断点续传见`RESUMABLE_UPGRADE_GUIDE.md`，历史救援镜像见`SAFETY_BASELINE.md`。
