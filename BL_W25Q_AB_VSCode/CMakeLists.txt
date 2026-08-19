cmake_minimum_required(VERSION 3.24)

project(STM32F407_WeatherClock_AB LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

# DEV构建在首次配置时生成仅供本机使用的签名密钥；密钥目录由Git忽略。
# PROD构建必须显式提供外部密钥，避免测试密钥被误用于正式固件。
set(BOOT_SECURITY_PROFILE "DEV" CACHE STRING "Bootloader security profile: DEV or PROD")
set_property(CACHE BOOT_SECURITY_PROFILE PROPERTY STRINGS DEV PROD)
string(TOUPPER "${BOOT_SECURITY_PROFILE}" BOOT_SECURITY_PROFILE)
if(NOT BOOT_SECURITY_PROFILE STREQUAL "DEV" AND
   NOT BOOT_SECURITY_PROFILE STREQUAL "PROD")
    message(FATAL_ERROR "BOOT_SECURITY_PROFILE must be DEV or PROD")
endif()

# 面向用户的版本字符串可以回退；这个独立计数只在修复安全问题时递增。
set(FIRMWARE_SECURITY_VERSION "1" CACHE STRING
    "Monotonic firmware security version embedded in signed XBIN images")
if(NOT FIRMWARE_SECURITY_VERSION MATCHES "^[0-9]+$" OR
   FIRMWARE_SECURITY_VERSION GREATER 4294967295)
    message(FATAL_ERROR "FIRMWARE_SECURITY_VERSION must be a uint32 decimal value")
endif()

find_program(FIRMWARE_OPENSSL_EXECUTABLE
    NAMES openssl
    HINTS
        "C:/Program Files/Git/usr/bin"
        "C:/Program Files/Git/mingw64/bin"
    REQUIRED
)

set(_DEV_SIGNING_KEY "${CMAKE_SOURCE_DIR}/.keys/dev_firmware_private.pem")
set(_DEV_VERIFY_KEY "${CMAKE_SOURCE_DIR}/.keys/dev_firmware_public.pem")
if(BOOT_SECURITY_PROFILE STREQUAL "DEV")
    set(FIRMWARE_SIGNING_KEY "${_DEV_SIGNING_KEY}"
        CACHE FILEPATH "PEM private key used to sign firmware")
    set(FIRMWARE_VERIFY_KEY "${_DEV_VERIFY_KEY}"
        CACHE FILEPATH "PEM public key compiled into the Bootloader")
    set(_CREATE_DEV_KEY --create)
    set(FIRMWARE_PROFILE_DEFINITION FIRMWARE_BUILD_PROFILE_DEV=1)
else()
    set(FIRMWARE_SIGNING_KEY "" CACHE FILEPATH "External PROD firmware signing key")
    set(FIRMWARE_VERIFY_KEY "" CACHE FILEPATH "External PROD firmware verification key")
    if(NOT FIRMWARE_SIGNING_KEY OR NOT FIRMWARE_VERIFY_KEY)
        message(FATAL_ERROR
            "PROD requires -DFIRMWARE_SIGNING_KEY=<private.pem> and "
            "-DFIRMWARE_VERIFY_KEY=<public.pem>")
    endif()
    if("${FIRMWARE_SIGNING_KEY}" STREQUAL "${_DEV_SIGNING_KEY}" OR
       "${FIRMWARE_VERIFY_KEY}" STREQUAL "${_DEV_VERIFY_KEY}")
        message(FATAL_ERROR
            "PROD refuses the auto-generated DEV key paths; provide an external key pair")
    endif()
    set(_CREATE_DEV_KEY)
    set(FIRMWARE_PROFILE_DEFINITION FIRMWARE_BUILD_PROFILE_PROD=1)
endif()

set(FIRMWARE_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(FIRMWARE_PUBLIC_KEY_HEADER "${FIRMWARE_GENERATED_DIR}/firmware_public_key.h")
execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/firmware_crypto.py
            --openssl ${FIRMWARE_OPENSSL_EXECUTABLE}
            --private-key ${FIRMWARE_SIGNING_KEY}
            --public-key ${FIRMWARE_VERIFY_KEY}
            --output-header ${FIRMWARE_PUBLIC_KEY_HEADER}
            ${_CREATE_DEV_KEY}
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
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS bootloader weatherclock
    COMMENT "Validating STM32 vectors, Flash ranges and XBIN header"
)

# 若开发机有本地GCC，再把Bootloader实际使用的SHA-256和micro-ecc源码编译为
# 宿主机程序。这样不仅验证OpenSSL工具链，还能及早发现两端字节序或实现漂移。
find_program(HOST_GCC_EXECUTABLE NAMES gcc)
if(HOST_GCC_EXECUTABLE)
    set(_HOST_EXECUTABLE_SUFFIX "")
    if(CMAKE_HOST_WIN32)
        set(_HOST_EXECUTABLE_SUFFIX ".exe")
    endif()
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
        DEPENDS validate_images
        COMMENT "Running Bootloader SHA-256/ECDSA code on the host"
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
        COMMENT "Testing anti-rollback policy with native C"
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
    add_custom_target(test_firmware_security_policy
        COMMAND ${CMAKE_COMMAND} -E echo
                "Host GCC not found; native anti-rollback policy test skipped"
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
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS test_firmware_crypto_c test_firmware_security_policy
            test_resume_algorithm
    COMMENT "Testing rejection of forged signed firmware"
)
