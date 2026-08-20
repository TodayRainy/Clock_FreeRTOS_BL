# BL_W25Q_AB_VSCode 工作约定

本文件只定义进入本目录后必须遵守的工作边界。具体编码规则、当前重构进度和
硬件救援信息分别以 `CODING_STYLE.md`、`REFACTOR_PLAN.md` 和
`SAFETY_BASELINE.md` 为准，不在此复制易变化的参数。

## 开始工作前

1. 阅读 `CODING_STYLE.md` 和 `REFACTOR_PLAN.md`。
2. 涉及烧录、Flash、EEPROM、W25Q 或启动流程时，再阅读
   `SAFETY_BASELINE.md`。
3. 从当前源码、CMake 和构建产物确认地址、容量、版本、波特率等动态参数，
   不把历史报告中的数值当作当前事实。

## 修改范围

- 当前主线仅为本目录。
- `BL/`、`Boot2026/`、`BL_W25Q_AB/` 和 `STM32F4_WT_FreeRTOS/` 是历史或
  参考项目，除非用户明确指定，否则不修改。
- CMSIS、STM32 标准外设库、FreeRTOS、Mbed TLS、micro-ecc 和启动文件等
  第三方代码不参与风格重构。

## 重构原则

- 注释和风格整理不得顺带改变协议、持久化布局、Flash 映射或安全策略。
- 风格重构与功能修改分开提交；发现功能问题时先记录，再单独讨论和验证。
- 注释解释设计原因、状态不变量、硬件时序和失败结果，不逐行翻译代码。
- 保持代码直接、清晰，不过度封装，不加入预见性防御层。
- 每批只处理一组强相关模块，并同步更新 `REFACTOR_PLAN.md`。

## 验证与安全

- C 代码修改后至少执行 GCC Release 构建、镜像校验和相关主机测试。
- 上位机修改后执行 .NET Release 构建。
- 关键启动、存储或通信模块完成后安排 J-Link 与串口实板回归。
- 只在 J-Link/SWD 和救援镜像可用时进行写入测试。
- 不修改 RDP、WRP、Option Bytes，不关闭 SWD，也不升级当前 J-Link 探针固件。
- 公共 OTA Manifest 只在最终发布验证完成后更新，并保持“先 XBIN、后
  Manifest”的发布顺序。
