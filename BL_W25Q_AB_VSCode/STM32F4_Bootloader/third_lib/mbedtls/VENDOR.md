# Mbed TLS AES-GCM subset

This directory contains the AES/GCM subset of Mbed TLS 3.6.5, copied from the
`WeatherClock2026/components/mbedtls` reference project that accompanies this
workspace.

Only the files required by `aes.c`, `gcm.c`, `block_cipher.c`,
`constant_time.c` and `platform_util.c` are retained. The local
`mbedtls_config.h` enables software AES, the lightweight block-cipher adapter
and GCM. ECDSA and SHA-256 remain provided by the existing micro-ecc and small
SHA-256 modules.

The imported sources are otherwise unchanged. They are available under the
Apache-2.0 or GPL-2.0-or-later choice stated in each source file and in
`LICENSE`; this project uses the Apache-2.0 option.
