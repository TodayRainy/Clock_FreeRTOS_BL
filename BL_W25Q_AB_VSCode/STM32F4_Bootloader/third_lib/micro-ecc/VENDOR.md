# micro-ecc vendor information

- Upstream: https://github.com/kmackay/micro-ecc
- Commit: `541b3a78026420a3e369c4c9281c396b5e531113`
- License: BSD 2-Clause，见 `LICENSE.txt`

本项目只调用`uECC_verify()`和`secp256r1`公钥验签。其他曲线由CMake编译宏
关闭；密钥生成、ECDH和设备端签名没有调用点，会由函数级链接回收从最终固件中
移除，以控制Bootloader体积和实际可达接口范围。
