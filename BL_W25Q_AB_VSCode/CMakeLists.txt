cmake_minimum_required(VERSION 3.24)

project(STM32F407_WeatherClock_AB LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(HOST_GCC_EXECUTABLE NAMES gcc REQUIRED)

set(_HOST_EXECUTABLE_SUFFIX "")
if(CMAKE_HOST_WIN32)
    set(_HOST_EXECUTABLE_SUFFIX ".exe")
endif()

# DEV/PROD仍用于区分维护接口权限，但两者都使用项目已确认的Formal密钥。
# 本项目是私人学习仓库，密钥按项目要求纳入版本管理；不得复用于商业产品。
set(BOOT_SECURITY_PROFILE "DEV" CACHE STRING "Bootloader security profile: DEV or PROD")
set_property(CACHE BOOT_SECURITY_PROFILE PROPERTY STRINGS DEV PROD)
string(TOUPPER "${BOOT_SECURITY_PROFILE}" BOOT_SECURITY_PROFILE)
if(NOT BOOT_SECURITY_PROFILE STREQUAL "DEV" AND
   NOT BOOT_SECURITY_PROFILE STREQUAL "PROD")
    message(FATAL_ERROR "BOOT_SECURITY_PROFILE must be DEV or PROD")
endif()

# 面向用户的版本字符串可以回退；这个独立计数只在修复安全问题时递增。
set(FIRMWARE_SECURITY_VERSION "2" CACHE STRING
    "Monotonic firmware security version embedded in signed XBIN images")
if(NOT FIRMWARE_SECURITY_VERSION MATCHES "^[0-9]+$" OR
   FIRMWARE_SECURITY_VERSION GREATER 4294967295)
    message(FATAL_ERROR "FIRMWARE_SECURITY_VERSION must be a uint32 decimal value")
endif()

# 普通发布序号只决定自动OTA先后顺序。Formal首版承接现有release 3，从4开始。
set(FIRMWARE_RELEASE_SEQUENCE "4" CACHE STRING
    "Monotonic release sequence used by automatic OTA")
if(NOT FIRMWARE_RELEASE_SEQUENCE MATCHES "^[0-9]+$" OR
   FIRMWARE_RELEASE_SEQUENCE LESS 1 OR
   FIRMWARE_RELEASE_SEQUENCE GREATER 4294967295)
    message(FATAL_ERROR "FIRMWARE_RELEASE_SEQUENCE must be in uint32 range 1..4294967295")
endif()

# APP_VERSION 与 XBIN Header 共用同一字符串，串口日志和固件头不会漂移。
set(WEATHERCLOCK_VERSION "formal-1.0.0" CACHE STRING
    "WeatherClock display version stored in the signed XBIN header")
if(WEATHERCLOCK_VERSION STREQUAL "" OR WEATHERCLOCK_VERSION MATCHES "[^ -~]")
    message(FATAL_ERROR "WEATHERCLOCK_VERSION must be non-empty printable ASCII")
endif()

find_program(FIRMWARE_OPENSSL_EXECUTABLE
    NAMES openssl
    HINTS
        "C:/Program Files/Git/usr/bin"
        "C:/Program Files/Git/mingw64/bin"
    REQUIRED
)

set(FIRMWARE_SIGNING_KEY
    "${CMAKE_SOURCE_DIR}/keys/formal_firmware_private.pem"
    CACHE FILEPATH "Formal P-256 private signing key" FORCE)
set(FIRMWARE_VERIFY_KEY
    "${CMAKE_SOURCE_DIR}/keys/formal_firmware_public.pem"
    CACHE FILEPATH "Formal P-256 public verification key" FORCE)
set(FIRMWARE_ENCRYPTION_KEY
    "${CMAKE_SOURCE_DIR}/keys/formal_firmware_aes256.key"
    CACHE FILEPATH "Formal raw 32-byte AES key" FORCE)

if(BOOT_SECURITY_PROFILE STREQUAL "DEV")
    set(FIRMWARE_PROFILE_DEFINITION FIRMWARE_BUILD_PROFILE_DEV=1)
else()
    set(FIRMWARE_PROFILE_DEFINITION FIRMWARE_BUILD_PROFILE_PROD=1)
endif()

set(FIRMWARE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(FIRMWARE_PUBLIC_KEY_HEADER "${FIRMWARE_GENERATED_DIR}/firmware_public_key.h")
set(FIRMWARE_ENCRYPTION_KEY_HEADER
    "${FIRMWARE_GENERATED_DIR}/firmware_encryption_key.h")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/firmware_crypto.py
            --openssl ${FIRMWARE_OPENSSL_EXECUTABLE}
            --private-key ${FIRMWARE_SIGNING_KEY}
            --public-key ${FIRMWARE_VERIFY_KEY}
            --output-header ${FIRMWARE_PUBLIC_KEY_HEADER}
            --encryption-key ${FIRMWARE_ENCRYPTION_KEY}
            --output-encryption-header ${FIRMWARE_ENCRYPTION_KEY_HEADER}
    RESULT_VARIABLE _KEY_PREPARE_RESULT
    OUTPUT_VARIABLE _KEY_PREPARE_OUTPUT
    ERROR_VARIABLE _KEY_PREPARE_ERROR
)
if(NOT _KEY_PREPARE_RESULT EQUAL 0)
    message(FATAL_ERROR "Firmware key preparation failed:\n${_KEY_PREPARE_ERROR}")
endif()
string(STRIP "${_KEY_PREPARE_OUTPUT}" _KEY_PREPARE_OUTPUT)
message(STATUS "Security profile: ${BOOT_SECURITY_PROFILE}")
message(STATUS "${_KEY_PREPARE_OUTPUT}")

# 主机打包器与 STM32 端共用同一份 Mbed TLS AES/GCM 源码。工具只接受文件参数，
# 不把密钥写入命令行或构建日志。
set(FIRMWARE_MBEDTLS_DIR
    "${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/third_lib/mbedtls")
# 当前 MinGW 链接器不能可靠创建中文绝对路径下的 EXE。Windows 上把纯主机
# 工具放到 LOCALAPPDATA；固件产物仍全部保留在工程 build 目录。
if(CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(FIRMWARE_HOST_TOOL_DIR "$ENV{LOCALAPPDATA}/ClockF407Tools")
else()
    set(FIRMWARE_HOST_TOOL_DIR "${CMAKE_BINARY_DIR}/host")
endif()
set(FIRMWARE_AES_TOOL
    "${FIRMWARE_HOST_TOOL_DIR}/firmware_aes_gcm_tool${_HOST_EXECUTABLE_SUFFIX}")
set(FIRMWARE_AES_TOOL_SOURCES
    ${CMAKE_SOURCE_DIR}/tools/firmware_aes_gcm_tool.c
    ${FIRMWARE_MBEDTLS_DIR}/library/aes.c
    ${FIRMWARE_MBEDTLS_DIR}/library/gcm.c
    ${FIRMWARE_MBEDTLS_DIR}/library/block_cipher.c
    ${FIRMWARE_MBEDTLS_DIR}/library/constant_time.c
    ${FIRMWARE_MBEDTLS_DIR}/library/platform_util.c
    ${FIRMWARE_MBEDTLS_DIR}/library/platform.c
)
add_custom_command(
    OUTPUT ${FIRMWARE_AES_TOOL}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${FIRMWARE_HOST_TOOL_DIR}
    COMMAND ${HOST_GCC_EXECUTABLE}
            -std=c11 -O2 -Wall -Wextra -DMBEDTLS_PLATFORM_C
            -I${FIRMWARE_MBEDTLS_DIR}/include
            -I${FIRMWARE_MBEDTLS_DIR}/library
            ${FIRMWARE_AES_TOOL_SOURCES}
            -o ${FIRMWARE_AES_TOOL}
    DEPENDS ${FIRMWARE_AES_TOOL_SOURCES}
            ${FIRMWARE_MBEDTLS_DIR}/include/mbedtls/mbedtls_config.h
    COMMENT "Building host AES-256-GCM packaging tool"
    VERBATIM
)
add_custom_target(firmware_aes_gcm_tool DEPENDS ${FIRMWARE_AES_TOOL})

# 两个固件使用同一颗 Cortex-M4F。把 ABI 参数集中在这里，避免子工程漂移。
add_library(stm32f407_compile_options INTERFACE)
target_compile_options(stm32f407_compile_options INTERFACE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -ffunction-sections
    -fdata-sections
    -fno-common
    # Windows GDB 不能可靠打开含中文的绝对 DWARF 路径，记录为工作区相对路径。
    "-fdebug-prefix-map=${CMAKE_SOURCE_DIR}=."
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-Og;-g3>
    $<$<CONFIG:Release>:-Os;-g1>
)
target_link_options(stm32f407_compile_options INTERFACE
    -mcpu=cortex-m4
    -mthumb
    -mfpu=fpv4-sp-d16
    -mfloat-abi=hard
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    --specs=nano.specs
    --specs=nosys.specs
)

add_subdirectory(STM32F4_Bootloader)
add_subdirectory(WeatherClock)

# 对最终可烧录产物做边界、向量表、摘要和固件签名校验。
add_custom_target(validate_images
    COMMAND ${Python3_EXECUTABLE} ../../tools/validate_images.py
            STM32F4_Bootloader/stm32f407.bin
            WeatherClock/stm32f407.bin
            WeatherClock/stm32f407_upgrade.xbin
            ${FIRMWARE_VERIFY_KEY}
            --openssl ${FIRMWARE_OPENSSL_EXECUTABLE}
            --aes-tool ${FIRMWARE_AES_TOOL}
            --encryption-key ${FIRMWARE_ENCRYPTION_KEY}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS bootloader weatherclock
    COMMENT "Validating STM32 vectors, Flash ranges and XBIN header"
)

# 若开发机有本地GCC，再把Bootloader实际使用的SHA-256和micro-ecc源码编译为
# 宿主机程序。这样不仅验证OpenSSL工具链，还能及早发现两端字节序或实现漂移。
if(HOST_GCC_EXECUTABLE)
    file(RELATIVE_PATH _FIRMWARE_GENERATED_RELATIVE
         "${CMAKE_SOURCE_DIR}" "${FIRMWARE_GENERATED_DIR}")
    file(RELATIVE_PATH _FIRMWARE_BUILD_RELATIVE
         "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}")
    set(_NATIVE_CRYPTO_TEST ".cache/native_crypto_selftest${_HOST_EXECUTABLE_SUFFIX}")
    add_custom_target(test_firmware_crypto_c
        COMMAND ${CMAKE_COMMAND} -E make_directory .cache
        COMMAND ${HOST_GCC_EXECUTABLE}
                -std=c11 -O2
                -DuECC_SUPPORTS_secp160r1=0
                -DuECC_SUPPORTS_secp192r1=0
                -DuECC_SUPPORTS_secp224r1=0
                -DuECC_SUPPORTS_secp256r1=1
                -DuECC_SUPPORTS_secp256k1=0
                -DuECC_OPTIMIZATION_LEVEL=2
                -DuECC_SQUARE_FUNC=0
                -Ishared
                -ISTM32F4_Bootloader/app
                -ISTM32F4_Bootloader/third_lib/sha256
                -ISTM32F4_Bootloader/third_lib/micro-ecc
                -I${_FIRMWARE_GENERATED_RELATIVE}
                tools/native_crypto_selftest.c
                STM32F4_Bootloader/app/firmware_auth.c
                STM32F4_Bootloader/third_lib/sha256/sha256.c
                STM32F4_Bootloader/third_lib/micro-ecc/uECC.c
                -o ${_NATIVE_CRYPTO_TEST}
        COMMAND ${_NATIVE_CRYPTO_TEST}
                ${_FIRMWARE_BUILD_RELATIVE}/WeatherClock/stm32f407_upgrade.xbin
                ${_FIRMWARE_BUILD_RELATIVE}/WeatherClock/stm32f407.bin
        DEPENDS validate_images
        COMMENT "Running Bootloader SHA-256/ECDSA code on the host"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    set(_NATIVE_ENCRYPTION_TEST
        ".cache/native_encryption_selftest${_HOST_EXECUTABLE_SUFFIX}")
    add_custom_target(test_firmware_encryption_c
        COMMAND ${CMAKE_COMMAND} -E make_directory .cache
        COMMAND ${HOST_GCC_EXECUTABLE}
                -std=c11 -O2 -Wall -Wextra -DMBEDTLS_PLATFORM_C
                -Ishared
                -ISTM32F4_Bootloader/app
                -ISTM32F4_Bootloader/third_lib/mbedtls/include
                -ISTM32F4_Bootloader/third_lib/mbedtls/library
                -I${_FIRMWARE_GENERATED_RELATIVE}
                tools/native_encryption_selftest.c
                STM32F4_Bootloader/app/firmware_decryption.c
                STM32F4_Bootloader/third_lib/mbedtls/library/aes.c
                STM32F4_Bootloader/third_lib/mbedtls/library/gcm.c
                STM32F4_Bootloader/third_lib/mbedtls/library/block_cipher.c
                STM32F4_Bootloader/third_lib/mbedtls/library/constant_time.c
                STM32F4_Bootloader/third_lib/mbedtls/library/platform_util.c
                STM32F4_Bootloader/third_lib/mbedtls/library/platform.c
                -o ${_NATIVE_ENCRYPTION_TEST}
        COMMAND ${_NATIVE_ENCRYPTION_TEST}
                ${_FIRMWARE_BUILD_RELATIVE}/WeatherClock/stm32f407_upgrade.xbin
                ${_FIRMWARE_BUILD_RELATIVE}/WeatherClock/stm32f407.bin
        DEPENDS validate_images
        COMMENT "Running Bootloader streaming AES-256-GCM code on the host"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(test_firmware_security_policy
        COMMAND ${CMAKE_COMMAND} -E make_directory .cache
        COMMAND ${HOST_GCC_EXECUTABLE}
                -std=c11 -O2 -Wall -Wextra -Ishared
                tools/test_firmware_security_policy.c
                -o .cache/test_firmware_security_policy${_HOST_EXECUTABLE_SUFFIX}
        COMMAND .cache/test_firmware_security_policy${_HOST_EXECUTABLE_SUFFIX}
        COMMENT "Testing security and automatic OTA release policies with native C"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(test_ota_contract
        COMMAND ${CMAKE_COMMAND} -E make_directory .cache
        COMMAND ${HOST_GCC_EXECUTABLE}
                -std=c11 -O2 -Wall -Wextra
                -Ishared -ISTM32F4_Bootloader/third_lib/crc
                tools/test_ota_contract.c
                STM32F4_Bootloader/third_lib/crc/crc32.c
                -o .cache/test_ota_contract${_HOST_EXECUTABLE_SUFFIX}
        COMMAND .cache/test_ota_contract${_HOST_EXECUTABLE_SUFFIX}
        COMMENT "Testing OTA EEPROM binary contract with native C"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
    add_custom_target(test_weather_parser
        COMMAND ${CMAKE_COMMAND} -E make_directory .cache
        COMMAND ${HOST_GCC_EXECUTABLE}
                -std=c11 -Wall -Wextra
                -IWeatherClock/app
                tools/test_weather_parser.c
                WeatherClock/app/weather.c
                -o .cache/test_weather_parser${_HOST_EXECUTABLE_SUFFIX}
        COMMAND .cache/test_weather_parser${_HOST_EXECUTABLE_SUFFIX}
        COMMENT "Testing fixed-point weather response parsing"
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        VERBATIM
    )
else()
    # 固件本身仍可正常构建；只有额外的同源C密码学自测需要宿主机GCC。
    add_custom_target(test_firmware_crypto_c
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native firmware crypto self-test skipped"
        DEPENDS validate_images
    )
    add_custom_target(test_firmware_encryption_c
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native firmware encryption test skipped"
        DEPENDS validate_images
    )
    add_custom_target(test_firmware_security_policy
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native firmware policy test skipped"
    )
    add_custom_target(test_ota_contract
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native OTA contract test skipped"
    )
    add_custom_target(test_weather_parser
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native weather parser test skipped"
    )
endif()

# 断点续传布局不依赖板卡：对生成的真实XBIN及合成的10%/50%/95%中断镜像
# 检查4 KiB分块、首个差异定位、32 KiB擦除回退和协议请求上限。
add_custom_target(test_resume_algorithm
    COMMAND ${Python3_EXECUTABLE} ../../tools/test_resume_algorithm.py
            WeatherClock/stm32f407_upgrade.xbin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS weatherclock
    COMMENT "Testing resumable-upgrade block planning"
)

# 构造CRC、SHA、元数据和签名被篡改的镜像，确认它们均无法通过信任边界。
add_custom_target(test_firmware_security
    COMMAND ${Python3_EXECUTABLE} ../../tools/test_firmware_security.py
            WeatherClock/stm32f407_upgrade.xbin
            ${FIRMWARE_VERIFY_KEY}
            --openssl ${FIRMWARE_OPENSSL_EXECUTABLE}
            --aes-tool ${FIRMWARE_AES_TOOL}
            --encryption-key ${FIRMWARE_ENCRYPTION_KEY}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_firmware_crypto_c test_firmware_encryption_c
            test_firmware_security_policy
            test_resume_algorithm test_ota_contract
    COMMENT "Testing rejection of forged signed/encrypted firmware"
)
