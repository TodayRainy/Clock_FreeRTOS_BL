# WeatherClock OTA 设计与使用说明

## 1. 目标与边界

这套 OTA 给现有 USART3 升级增加了一条联网下载路径，但不替代串口升级、
KEY2 维护入口或 J-Link 救援。天气时钟负责把签名 XBIN 下载到非活动 W25Q128；
Bootloader 仍是唯一有权擦写内部应用区、切换 A/B 状态和执行回退的组件。

当前实现刻意保持简单：腾讯云 COS 托管一个扁平 JSON 清单和一个完整 XBIN；
ESP32-C3 只建立 HTTPS 连接，不参与固件可信性判断；没有在应用中复制一套
ECDSA 验签代码。

## 2. 运行结构

```text
腾讯云 COS
  manifest.json + signed.xbin
            |
            | HTTPS / HTTP Range
            v
ESP32-C3 ESP-AT -- USART2 -- WeatherClock OTA task
                                  |
                                  | SPI1，写非活动槽
                                  v
                         W25Q128 A / W25Q128 B
                                  |
                                  | EEPROM OTA request
                                  v
Bootloader重新验签、校验、防降级 -> 内部Flash -> Trial -> 应用确认或回退
```

天气时钟创建优先级 3 的 OTA 任务后，先等待本地温湿度和在线天气各成功刷新
一次，再延迟 60 秒执行首次自动检查，以后每 24 小时检查一次。任务绝大多数
时间都阻塞，不持续轮询，不影响页面刷新。发现发布序号更大且满足安全版本规则
的固件后，会自动下载到非活动 W25Q128 槽并完成校验，但不会立即重启。

USART1 调试口保留一个学习和联调用命令：

- `u`：不等待定时器，立即检查并在有更新时下载、校验和暂存。

下载成功后 PE9、PB0、PB1 三颗低电平点亮的板载 LED 会保持常亮，表示“新固件
已准备，等待重启”。用户按板载 RESET 后，Bootloader 才开始安装。

## 3. 下载与掉电恢复

OTA 固件永远写入当前 `active_slot` 的另一槽，已确认回退槽不会被覆盖。新下载
开始时先擦除目标槽，Header 的前 256 字节保持擦除态；只有正文全部下载并通过
CRC32、payload SHA-256 和整包 SHA-256 后，才最后写入有效 Header。

下载每完成 32 KiB，在 BL24C512 的 `0xFC00` 保存一个 128 字节进度记录。掉电
后下一次自动检查或输入 `u`，程序匹配固件 SHA-256、目标槽和安全版本，然后
从最近的完整检查点继续。检查点之后可能写了一半的区域会先重新擦除，不会在
已编程 Flash 上直接覆盖。

进度记录不是关键状态，没有采用双副本：记录损坏的后果只是重新下载。真正的
A/B 启动状态仍在 `0xFD00`、`0xFD80` 以双副本保存。

## 4. 应用与 Bootloader 的交接

下载完成后，WeatherClock 在 EEPROM `0xFC80` 写一条一次性请求并继续运行当前
应用，其中记录：

- 目标槽与 XBIN 总大小；
- Header CRC32、payload CRC32 和安全版本；
- payload SHA-256。

这些字段只能证明“请求和刚下载的文件一致”，不能证明文件可信。复位后的
Bootloader 会重新读取整颗目标槽，并检查：

1. 请求记录 CRC 和目标必须有效，且只能指向非活动槽；
2. XBIN Header 格式、Header CRC 和 ECDSA-P256 签名；
3. 完整 payload 的 CRC32、SHA-256 和向量表；
4. 请求字段必须与已验签 Header 一致；
5. 安全版本不得低于设备已经确认的防降级下限。
6. 候选普通发布序号必须高于当前固件，旧包和同序号异内容包均拒绝自动安装。

用户按 RESET 后，Bootloader 才读取这条请求。全部检查通过后才写入
`InstallPending` 并进入原有流程。安装掉电会从外部候选槽重装；Trial 未确认会
恢复已确认槽；请求无效只会被丢弃并继续启动原固件。请求在提交后会被清除，
因此普通重启不会反复安装同一固件。

持续按住 KEY2 启动时不会消费 OTA 请求，仍优先进入人工维护模式。

## 5. COS 清单格式

清单固定地址为：

`https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/manifest.json`

当前解析器只读取项目发布工具生成的扁平字段，不是通用 JSON 库。这样可避免
为了一个约 600 字节的受控文件引入较大的解析依赖。候选固件 URL 必须使用
HTTPS，COS 对 XBIN 必须支持 HTTP Range。

清单仍输出`firmware_version`、`release_sequence`、`security_version`和 payload
摘要，以兼容已经部署的旧 OTA 客户端并方便人工检查。新客户端只解析产品、URL、
XBIN 大小和整包 SHA-256；版本、发布序号、安全版本、CRC 和 payload SHA 均以
随后读取的 XBIN Header 为唯一运行时来源，避免清单与 Header 各保存一份状态。

生成清单：

```powershell
python tools/make_ota_manifest.py `
  build/release-candidate/WeatherClock/stm32f407_upgrade.xbin `
  ota/manifest.json `
  --url https://weather-clock-ota-1331831322.cos.ap-beijing.myqcloud.com/clock-f407/releases/weatherclock-v1.7.0-ota-ordering-r1.xbin
```

发布时必须先上传带唯一版本名的 XBIN，确认公网可读及 Range 正常，最后再覆盖
`manifest.json`。这样设备不会看到一个指向尚未上传文件的新清单。

```powershell
coscli cp build/release-candidate/WeatherClock/stm32f407_upgrade.xbin `
  cos://codex_ota/clock-f407/releases/weatherclock-v1.5.1-ota-test.xbin
coscli cp ota/manifest.json `
  cos://codex_ota/clock-f407/manifest.json
```

Bucket 维持“公有读、私有写”：设备只需要公开下载地址，腾讯云 Secret ID 和
Secret Key 只保存在开发机 COSCLI 配置中，绝不能编译进 STM32 固件。

## 6. 构建两个测试版本

首次部署的 bootstrap 版本包含 OTA 客户端：

```powershell
cmake --preset release `
  -DWEATHERCLOCK_VERSION=v1.7.0-ota-ordering `
  -DFIRMWARE_RELEASE_SEQUENCE=1
cmake --build --preset release --target validate_images
```

真实 OTA 测试需要另一个显示版本。使用独立构建目录，避免覆盖 bootstrap：

```powershell
cmake -S . -B build/release-candidate -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi-gcc.cmake `
  -DCMAKE_BUILD_TYPE=Release `
  -DWEATHERCLOCK_VERSION=v1.7.1-ota-test `
  -DFIRMWARE_RELEASE_SEQUENCE=2
cmake --build build/release-candidate --target validate_images
```

`WEATHERCLOCK_VERSION` 同时写入应用日志中的 `APP_VERSION` 和签名 XBIN Header。
`FIRMWARE_RELEASE_SEQUENCE` 是每次发布不同固件都递增的普通发布序号，自动 OTA
用它判断新旧；序号相同时还必须具有相同 SHA，否则按发布冲突拒绝。旧 XBIN 的
这个保留字为 0，新方案从 1 开始平滑迁移。

`FIRMWARE_SECURITY_VERSION` 是另一条独立的单调安全计数，只有淘汰存在安全问题
的旧固件时才提高。USART3/KEY2 人工维护路径仍允许安装合法签名的旧发布，普通
发布序号不会剥夺用户主动降级的能力。

## 7. 验证清单

主机侧：

```powershell
cmake --build --preset release --target validate_images
cmake --build --preset release --target test_ota_contract
cmake --build --preset release --target test_firmware_security
```

实板分阶段验证：

1. 保持 J-Link 可连接，并记录当前已确认回退槽。
2. 先更新 Bootloader，再通过原 USART3 升级路径安装 bootstrap XBIN。
3. 确认热点 `RAIN` 联网、应用稳定和 Trial 已自动确认。
4. 上传 candidate XBIN，最后上传新清单。
5. 等待自动检查，或在 USART1 输入 `u` 立即触发检查和下载。
6. 观察 32 KiB 进度以及应用侧完整校验，确认三颗 LED 常亮且当前应用不重启。
7. 按 RESET，观察 Bootloader 独立验签、安装、Trial 和应用确认。
8. 确认新版本日志以及 BCR 切换到 Normal。
9. 下载中途断电一次，确认从检查点恢复；Trial 阶段阻止确认一次，确认回退。

任何 OTA 故障都不需要修改 STM32 Option Bytes。只要 SWD/J-Link 可连接，仍可
重新烧录 `0x08000000` 的 Bootloader；现有外部活动槽也保留了自动恢复路径。

## 8. 当前资源与限制

- Bootloader：约 29 KiB / 48 KiB Flash。
- WeatherClock：约 320 KiB / 448 KiB Flash。
- WeatherClock 静态 RAM：约 122 KiB / 128 KiB，其中包含 92 KiB FreeRTOS heap。
- OTA 只支持 HTTPS GET、`Content-Length` 和 HTTP Range，不处理重定向或
  chunked response；腾讯云 COS 当前响应满足这些条件。
- ESP-AT 与天气/SNTP 请求共用互斥锁。升级期间网络业务会等待，但 UI 和高
  优先级看门狗任务不停止。
- 清单和普通天气业务保持 USART2 115200；大文件下载前通过 `AT+UART_CUR`
  临时切换到 921600，HTTP 连接关闭后恢复 115200。
- 下载结束后调用 `w25q_deinit()`，释放 SPI1 和 DMA2，并将 PA5/PA6/PA7 恢复
  为模拟输入；屏幕使用 SPI2，整个过程不会占用显示总线。
- 当前应用没有其他 SPI1 使用者。以后启用共享总线上的 TF 卡时，应在 SPI1
  设备层增加互斥锁。

RAM 余量比加入 OTA 前更紧，后续新增大缓冲区时应先查看 ELF size；无需为了
“可能有用”预留更多常驻缓冲。

## 9. 2026-08-19 实板结果

本次使用板载双 W25Q128、BL24C512、ESP32-C3 ESP-AT v4.1.1、J-Link V9 和
USART1/USART3 三条观测/升级链路完成验证：

- USART3 以 2 Mbaud 安装 OTA bootstrap，Trial 在 5 秒健康窗口后确认；
- `RAIN` 热点连接、COS 完整 GET 与 256 字节 Range GET 均成功；
- 首次 324,520 字节 OTA 完整下载、应用侧校验、Bootloader 独立验签和安装成功；
- 下载到 69,632 字节时用 J-Link 普通复位模拟掉电，BCR 保持 Normal，活动槽
  继续启动，非活动槽 Header 保持无效；
- 再次输入 `u` 从 69,632 字节继续，最终 CRC32、Header/manifest payload
  SHA-256 和完整 XBIN SHA-256 全部通过；
- 最终 BCR 为 `Normal / active=B / pending=none / sequence=215`，A、B 两槽均
  通过完整校验，最终应用版本为 `v1.5.3-ota-final`；
- 同时修正了 COS 响应头超过 512 字节、ESP 遗留 SSL socket 和 newlib-nano
  SNTP `sscanf` 长度修饰符三个在实板才暴露的问题。

测试中还主动经历过一次 ESP `busy p...` 和一次最终摘要不匹配；两次都只让本轮
OTA 失败并保留检查点，没有写有效 Header、没有提交安装请求，也没有改变活动
槽。这验证了失败路径的安全性，而不是只验证理想网络下的成功路径。

### 9.1 暂存后重启安装与高速串口实测

随后使用 `v1.6.0-ota-stage` 对新的交互流程完成实板回归：

- 应用从 36,864 字节检查点继续下载 324,816 字节候选，USART2 在正文阶段使用
  921600，并在完整接收后通过 `AT+UART_CUR` 成功恢复到 115200；
- CRC32、Header/payload SHA-256 和完整 XBIN SHA-256 全部通过；
- 下载结束后 CPU 仍运行在应用区，天气与 AHT20 任务继续工作，没有自动复位；
- J-Link 读取 GPIOB ODR=`0x000000C0`、GPIOE ODR=`0x0000603C`，确认 PB0、PB1、
  PE9 三个低有效 LED 输出均为低电平；
- 执行普通 RESET 后，Bootloader 接受 EEPROM 请求、安装目标槽并进入 Trial；
- 最后通过外部 A/B 断点复用恢复 `v1.6.0-ota-stage`，5 秒健康窗口后确认 Normal。
- 对应 XBIN 与清单已按“固件优先、清单最后”发布到 COS；公网 GET/Range 校验
  正常，板端再次检查报告 `firmware is already current`，不会重复下载。

### 9.2 1024字节拉取与真正掉电续传回归

最终以`v1.6.5-ota-full-test`到`v1.6.6-ota-resume-test`完成当前实现的回归：

- HTTPS正文阶段把单次`AT+CIPRECVDATA`请求统一为1024字节。这个数值与应用
  OTA流缓冲、ESP驱动会话缓冲和2 KiB UART环形缓冲相匹配，不再请求超过本地
  接收能力的数据；
- 每次被动拉取使用一个共享的绝对截止时间。ESP返回`busy p...`或暂时无数据时，
  驱动会先完整消费本行，再在同一个总超时内重试，不会让每次重试重新获得一份
  10秒超时；协议边界不明确时则结束本轮HTTP，而不是盲目续传；
- 从零下载326,564字节XBIN成功，CRC32、payload SHA-256和整包SHA-256全部
  通过；USART2在HTTP关闭后由921600恢复115200；
- 第二轮在36,864字节检查点后复位。Bootloader没有消费半成品Header，也没有
  改变活动槽，而是继续启动已确认应用；应用重连后识别ESP仍处于921600并恢复
  到115200；
- 再次输入`u`从36,864字节精确续传，剩余约289,700字节用时约10.1秒，正文
  吞吐约28 KiB/s；完整校验通过后写Header和OTA请求；
- 用户按RESET后Bootloader独立验签、安装并进入Trial，应用在5秒健康窗口后
  确认，最终`Normal / active=A / pending=none`；再次输入`u`报告固件已是当前
  版本，不重复擦写外部Flash。

当前发布清单是`ota/manifest.json`，XBIN总长326,564字节，payload长322,468
字节，payload CRC32为`0x33933CF0`，整包SHA-256为
`C1DB903C5A016FE6F73F3A4A2995100904C2A815101C5F6F6085360EB10CB4C9`。

这些调整改善的是ESP-AT命令边界和异常恢复，不改变A/B提交原则：正文下载失败
时目标槽Header仍无效，EEPROM安装请求只在三项摘要全部通过后写入，Bootloader
仍会在复位后重新验签。1024字节不是越大越好的“性能参数”；若以后要继续提高，
应同时扩大并重新验证UART环形缓冲、流缓冲、ESP固件可用缓存和任务调度时延。

### 9.3 发布序号与旧包拒绝回归

2026-08-20以`v1.7.0-ota-ordering / release 1`完成新旧判断实测。板上原应用为
`v1.6.6-ota-resume-test / release 0`，旧客户端能够忽略 Manifest 新增字段，下载
并安装新 XBIN；新 Bootloader重新验签并复核发布序号，应用通过Trial后回到
`Normal / active=A / pending=none`。

当前 Manifest 再次检查时输出`firmware is already current`。随后把 Manifest
短暂指向真实 v1.6.6旧 XBIN，新应用读取其签名 Header 后输出
`older release ignored: current=1 candidate=0`，没有擦除外部槽或下载旧正文。
测试结束后公网 Manifest 已恢复为 release 1。

本轮 XBIN 为162,132字节，payload为158,036字节。主机侧还验证了相同发布序号、
不同 SHA 会得到`FIRMWARE_RELEASE_CONFLICT`，以及直接篡改 Header 发布序号无法
通过 ECDSA-P256 验签。详细记录见`RELEASE_NOTES_v1.7.0.md`。
