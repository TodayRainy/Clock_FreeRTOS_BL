# 代码风格与注释重构计划

本文是本轮重构的持久化执行看板。每次继续工作前先读取本文；每完成一批修改，更新
状态、验证结果和 Git commit。编码规则以 `CODING_STYLE.md` 为准，硬件救援信息以
`SAFETY_BASELINE.md` 为准。

## 1. 目标

- 统一自研 C 代码的表达方式和模块边界。
- 为关键接口、状态转换、硬件时序和失败路径提供有效注释。
- 删除翻译代码式注释、重复逻辑和确实影响理解的冗余封装。
- 保持 Formal 固件格式、A/B 行为和现有硬件功能不变。
- 形成可以用于代码学习、面试讲解和后续维护的稳定代码库。

不以注释数量、函数数量或抽象层数衡量完成度。

## 2. 分支与稳定基线

- 重构分支：`refactor/code-style-and-comments`
- 基线分支：`main`
- 基线 commit：`5b64b2a`
- 基线 tag：`formal-1.0.0`
- 基线日期：2026-08-20

当前工作区还有历史 `BL/` 和资料目录的用户修改或未跟踪文件。它们不属于本轮重构，
不得暂存到本分支提交中。

### 2.1 构建产物

| 产物 | 长度 | SHA-256 |
|---|---:|---|
| Bootloader BIN | 34,800 B | `A0E684C109DC2900C0E580DB21F87905A7E9540852E5A6D718DBC2E792EDF962` |
| WeatherClock BIN | 158,024 B | `37528CB11BCFA9F1345DCA58E9929342EBD22F4DFDDDC2EF97B26A29A911CAFC` |
| Formal XBIN | 162,120 B | `F4440CCD01F7CC06A0D9F31BDA17B105F8F7A7A2F6E7B6BEB1B73441477C496B` |

基线空间占用：

- Bootloader：34,800 B / 48 KiB，70.80%；
- WeatherClock：158,024 B / 448 KiB，34.45%。

### 2.2 Formal 发布参数

- 固件版本：`formal-1.0.0`；
- Header revision：1；
- `release_sequence`：4；
- `security_version`：2；
- 签名 Key ID：`0xCA9EEE70`；
- 加密 Key ID：`0x9B5260DC`。

这些数值只记录基线，不是编码规范。后续发布必须从当前 CMake、Header 和 Manifest
重新确认。

### 2.3 实板与救援

- MCU：STM32F407VG，J-Link SWD 可连接；
- Bootloader 升级串口：USART3，实测端口为 COM29；
- 基线实板：BCR Normal，活动槽 B，sequence 320；
- 外部 Slot A、Slot B 和内部 Formal 应用均通过完整验证；
- 安全版本下限为 2；
- 未修改 RDP、WRP、Option Bytes 或 SWD；
- 整片救援镜像：
  `.device-backups/stm32f407_formal_1_0_0_final_20260820.bin`；
- 救援镜像长度：524,288 B；
- 救援镜像 SHA-256：
  `FBCB8245952C87C6333FC71CAA8A5C5DB7A9987A2573788C3855707B43BA94DF`。

旧版 SEGGER J-Link DLL 对中文固件路径处理不可靠。实板写入和 Verify 使用 ASCII
临时路径，不能只依赖包装脚本的成功提示。

## 3. 阶段 1～5 冻结项

风格重构不得顺带改变：

- Formal Header 和 XBIN 布局；
- BCR、OTA 请求和断点记录布局；
- 串口帧、Opcode 和错误码；
- 内部 Flash 和外部槽映射；
- 密钥和 Key ID；
- 防降级和人工授权策略；
- 波特率、SPI 频率及协议超时；
- FreeRTOS 任务优先级和栈大小；
- 看门狗配置和 Trial 次数；
- COS 公共 Manifest。

发现这些项目存在真实问题时，在“待讨论问题”中记录，得到许可后作为独立功能修改。
阶段6完成全部验证后，允许只递增发布版本和序号、生成对应Manifest；仍不得改变其
二进制契约或安全策略。

## 4. 执行方法

每个模块分两遍：

1. **说明遍**：理解调用关系，补充文件/API/关键代码段注释，不改变逻辑。
2. **整理遍**：统一命名和控制流，删除确定的重复或无意义封装，并单独审阅行为差异。

每批只处理 3～6 个强相关文件：

1. 记录当前职责与调用关系；
2. 完成说明遍；
3. 必要时完成整理遍；
4. 构建和运行相关测试；
5. 检查 Git diff 与空间变化；
6. 更新本文；
7. 独立提交。

## 5. 阶段看板

| 阶段 | 范围 | 状态 |
|---|---|---|
| 0 | 指导文件、分支和稳定基线 | 已完成 |
| 1A | shared 契约、Header、签名与解密包装 | 已完成 |
| 1B | BCR、固件槽和 Bootloader 状态机 | 已完成 |
| 2 | 串口协议与 Bootloader 自研驱动 | 已完成 |
| 3 | WeatherClock 应用、任务、健康确认、Wi-Fi 与 OTA | 已完成 |
| 4 | WeatherClock 自研底层驱动 | 已完成 |
| 5 | C# 上位机、Python 工具和项目说明 | 已完成 |
| 6 | 完整主机/实板回归与 Formal 1.0.1 发布 | 已完成 |

## 6. 分阶段范围

### 阶段 0：指导与基线

- [x] 从 `formal-1.0.0` 创建独立重构分支。
- [x] 记录构建产物、实板状态和救援镜像。
- [x] 新建 `AGENTS.md`。
- [x] 新建 `CODING_STYLE.md`。
- [x] 新建 `REFACTOR_PLAN.md`。
- [x] 完成指导文件自检并单独提交阶段 0。

### 阶段 1A：共享契约与密码学入口

计划文件：

- `shared/firmware_image_format.h`；
- `shared/ab_boot_contract.h`；
- `shared/ota_contract.h`；
- `shared/firmware_release_policy.h`；
- `shared/firmware_security_policy.h`；
- `STM32F4_Bootloader/app/magic_header.c/.h`；
- `STM32F4_Bootloader/app/firmware_auth.c/.h`；
- `STM32F4_Bootloader/app/firmware_decryption.c/.h`。

重点：二进制布局、签名对象、GCM AAD、密钥选择和算法边界。不得改变任何字节布局。

完成情况：

- 统一了自研头文件的非保留 Include Guard；
- 补全 Formal Header、BCR、OTA 记录字段和密码学 API 的契约说明；
- 统一密码学入口的 Include 顺序与局部命名；
- terra medium 只读审阅发现 `magic_header.c` 缺少直接包含 `<string.h>`，已修正；
- GCC 零错误零警告，完整安全测试通过；
- Bootloader 和 WeatherClock BIN 的长度与 SHA-256 均和基线一致。

### 阶段 1B：A/B 核心状态机

计划文件：

- `STM32F4_Bootloader/app/boot_state.c/.h`；
- `STM32F4_Bootloader/app/firmware_slot.c/.h`；
- `STM32F4_Bootloader/app/bootloader.c/.h`；
- `STM32F4_Bootloader/app/ota_request.c/.h`。

重点：掉电恢复、Header 最后提交、Trial 次数、确认槽保护、迁移采用和人工救援。

完成情况：

- 统一 BCR 双副本变量、校验结果和安装结果命名；
- 补齐 A/B 核心头文件契约和掉电安全代码段说明；
- 把按键采样周期集中为一个模块常量，数值与行为保持不变；
- terra medium 只读审阅未发现 P0～P2，指出 `boot_state.c` 缺少直接
  `<stdint.h>`，已修正；
- GCC 零错误零警告，完整安全测试通过；
- Bootloader 仍为 34,800 B，WeatherClock BIN 与基线完全一致。

### 阶段 2：协议与 Bootloader 驱动

重点文件：`boot_protocol`、`bl_usart`、`w25q`、`bl24c512`、`stm32_flash`、
`watchdog`、board、key、timer 和 console。

重点：帧边界、DMA、槽写保护、外设超时、Flash 擦写和跳转前反初始化。

完成情况：

- 保持串口帧、Opcode、错误码、波特率、DMA容量、SPI频率和硬件超时不变；
- 将协议字节序辅助函数收敛为实际使用的 U8/U16/U32 小端接口，改为逐字节读写，
  消除未对齐宽指针访问和无用的宏生成接口；
- 统一板级资源、USART3、EEPROM、W25Q、内部Flash、TIM6、LED、按键和日志驱动的
  Include、命名与注释，补齐跳转前资源边界和关键硬件时序说明；
- terra medium 只读审阅未发现 P0～P2，指出 USART3 头文件把循环DMA误写成
  RXNE中断，已修正文档；
- GCC 零错误零警告，完整安全测试通过；Bootloader 增加 96 B，WeatherClock
  长度和 SHA-256 与基线一致。

### 阶段 3：WeatherClock 应用

重点文件：`main`、`app`、`health_monitor`、`ab_confirm`、`ota`、`wifi` 及页面入口。

重点：任务生命周期、初始化顺序、Wi-Fi 恢复、天气与 OTA 时序、Trial 健康语义。

完成情况：

- 保持任务优先级、栈大小、看门狗周期、联网周期和OTA参数不变，补齐关键 Task、
  Queue 与 Timer 创建结果检查；
- 删除 A/B 确认模块的重复 CRC32 算法，统一使用项目现有实现；
- 将 SNTP 和天气工作改为明确的 `void (*)(void *)` 回调，移除函数指针与
  `void *` 之间的非标准转换，同时维持单工作队列串行访问ESP；
- 统一主程序、Wi-Fi、OTA、UI、页面和天气解析的模块说明、命名、Include 与
  控制流，修复天气路径字符串 `%128[...]` 对128字节数组的一字节越界；
- 把两份CP936页面源码转为UTF-8，并通过 `-fexec-charset=GBK` 保持固件内字节
  与现有GB2312点阵字库兼容，二进制抽查确认只存在GBK文本；
- 新增天气路径最大长度测试；terra medium 只读审阅未发现 P0～P3；
- GCC 零错误零警告，天气解析和完整安全测试通过；WeatherClock 增加172 B，
  Bootloader长度和SHA-256与阶段2一致。

### 阶段 4：WeatherClock 驱动

按 console、key、ESP-AT、存储、RTC、传感器和显示的依赖顺序分批处理。重点是硬件
时序、缓冲区边界、ISR 上下文和共享总线所有权，不重新设计稳定驱动框架。

完成情况：

- 统一 AHT20、BL24C512、RTC、TIM6、LED、按键、USART1、ESP-AT 和 ST7789
  驱动的文件说明、公开接口、命名、Include Guard 与关键硬件时序注释；
- 将 AHT20 的返回式事件宏改为普通函数，所有 I2C 失败出口统一产生 STOP，并修复
  状态读取失败被误判为测量完成的问题；
- RTC 按 STM32 Calendar Shadow Registers 要求改为先读 TR、再读 DR，一次读取即
  获得一致快照，删除依赖结构体比较的重复读取；
- 删除没有调用者的 USART1 TX DMA、信号量和中断路径，保留实际使用的轮询日志
  输出；同步移除闲置 DMA2 时钟，并将 Wi-Fi 状态接口统一为 ESP-AT 模块前缀；
- 整理 ST7789 的 8 位寄存器和 16 位像素 DMA 边界，空数据不再启动零长度 DMA，
  修复图像底边判断多拒绝两行的问题；
- terra medium 只读审阅发现 USART1 在未注册回调时不读取 DR、会令 RXNE 持续触发
  的 P2 问题；已改为 ISR 始终先消费字节，再按需调用回调；其余无 P0/P1/P3；
- GCC 零错误零警告，天气解析和完整安全测试通过；WeatherClock 减少 516 B 至
  157,680 B，Bootloader 保持 34,896 B。

### 阶段 5：主机工具与说明

整理 C# 上位机和 Python 工具，使字段命名、协议术语和 Formal 文档一致；同步更新
`CODE_GUIDE.md`，加入模块调用关系和适合面试讲解的设计取舍。

完成情况：

- 将 C# 串口帧、Formal Header、BCR、断点 CRC 和槽命令改为显式小端读写，统一
  类型、枚举、事件和异步控制流，删除已经退出主线的旧 Bootloader 回退分支；
- 串口接收只把 `SerialPort.Read()` 实际返回的字节数写入 Channel；协议解析和 CRC
  使用 Span，避免逐阶段重复复制完整帧；UI 在升级期间禁用按钮并集中处理日志；
- 统一 Python 工具说明和生成头文件 Include Guard，重写旧字体转换脚本的入口和
  数据流；补齐主机天气解析测试的文件说明；
- 更新 README 当前空间占用与发布冲突提示，并在 `CODE_GUIDE.md` 增加三条主要调用
  链和七项适合面试讲解的设计取舍；
- terra medium 只读审阅未发现 P0～P3；GCC、全部主机测试、Python 语法检查和
  .NET Release 零警告构建通过；固件长度与阶段 4 一致。

### 阶段 6：完整验收

- [x] GCC Release 构建和 Formal 镜像验证；
- [x] 密码学、篡改、策略、OTA 契约、断点续传和天气解析测试；
- [x] .NET Release、Python 语法和 C# 固定帧协议测试；
- [x] 固件空间和 SHA-256 对比；
- [x] J-Link 连接、写入和整片回读验证；
- [x] USART3 完整升级、差异传输和全镜像断点复用；
- [x] Trial 确认、尝试次数耗尽回退和回退安装中再次复位；
- [x] 旧应用经 ESP-AT/COS 暂存 OTA，复位安装并由新应用确认；
- [x] 创建并再次验证新的整片救援镜像；
- [x] COS 按“XBIN 上传并回读、Manifest 最后更新”发布 Formal 1.0.1；
- [x] 更新 README、安全基线、发布说明和重构报告。

验收中发现 C# 重构误把响应帧偏移用于请求帧构造，导致请求多出一个字节。该问题在
真正连接 COM29 时被固定字节序列定位；现已分离请求/响应偏移，并新增无外部测试框架
依赖的 `SerialUpgrader.ProtocolTests`。修正后上位机状态查询、2 Mbaud完整升级和槽提交
均通过。

最终 Bootloader 为34,896 B，WeatherClock为157,680 B，XBIN为161,776 B。最终BCR为
`Normal / active=A / pending=none / attempts=0 / sequence=363`，A、B两槽和内部应用均为
`formal-1.0.1 / release 5 / security 2`。完整证据见`SAFETY_BASELINE.md`和
`RELEASE_NOTES_FORMAL_1.0.1.md`。

## 7. 每批验证记录

| 批次 | 修改范围 | 构建/测试 | 空间变化 | 实板 | Commit |
|---|---|---|---|---|---|
| Baseline | Formal 1.0.0 | GCC、.NET、完整安全测试已通过 | BL 34,800 B；App 158,024 B | A/B、串口、断点复用已通过 | `5b64b2a` |
| 0 | 指导文件和计划 | `git diff --check` 通过；未修改固件 | 不涉及固件 | 不需要 | `docs: establish refactor rules and baseline` |
| 1A | 共享契约、Header、签名和解密入口 | GCC、完整安全测试、terra 审阅通过 | 0 B；目标 BIN 哈希不变 | 不需要 | `refactor(formal): unify contracts and crypto comments` |
| 1B | BCR、固件槽和 Bootloader 状态机 | GCC、完整安全测试、terra 审阅通过 | 0 B；Bootloader 哈希变化 | 阶段 6 统一验证 | `refactor(boot): clarify A/B state and install flow` |
| 2 | 串口协议和 Bootloader 自研驱动 | GCC、完整安全测试、terra 审阅通过 | BL +96 B；App 0 B且哈希不变 | 阶段 6 统一验证 | `refactor(boot): unify protocol and driver style` |
| 3 | WeatherClock任务、Wi-Fi、OTA、UI和页面 | GCC、天气解析、完整安全测试、terra 审阅通过 | App +172 B；BL不变 | 阶段 6 统一验证 | `refactor(app): clarify tasks networking and UI flow` |
| 4 | WeatherClock自研底层驱动 | GCC、天气解析、完整安全测试、terra 审阅通过 | App -516 B；BL不变 | 阶段 6 统一验证 | `refactor(drivers): clarify hardware and DMA flows` |
| 5 | C#上位机、Python工具和代码导读 | GCC、完整主机测试、Python语法、.NET、terra审阅通过 | 固件长度不变 | 阶段 6 统一验证 | `refactor(tools): align updater and project documentation` |
| 6 | Formal 1.0.1发布与完整回归 | GCC、完整主机测试、.NET、协议测试、terra审阅通过 | BL 34,896 B；App 157,680 B | J-Link、USART3、Trial、回退、OTA、整片Verify通过 | `release: validate formal 1.0.1 refactor` |

## 8. 待讨论问题

只记录重构中发现、但不应混入当前批次的事项。当前为空。

## 9. 最终发布规则

最终 WeatherClock BIN 的 SHA-256 与基线不同，因此发布版本递增为 `formal-1.0.1`，
`release_sequence` 递增为5，`security_version` 保持2。公共发布已采用“上传并回读
验证XBIN，最后更新Manifest”的顺序完成；release 4对象仍保留用于回归测试。
