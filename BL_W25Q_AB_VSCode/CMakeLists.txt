cmake_minimum_required(VERSION 3.24)

project(STM32F407_WeatherClock_AB LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

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

# 对最终可烧录产物做边界、向量表和 XBIN 双 CRC 校验。
add_custom_target(validate_images
    COMMAND ${Python3_EXECUTABLE} ../../tools/validate_images.py
            STM32F4_Bootloader/stm32f407.bin
            WeatherClock/stm32f407.bin
            WeatherClock/stm32f407_upgrade.xbin
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    DEPENDS bootloader weatherclock
    COMMENT "Validating STM32 vectors, Flash ranges and XBIN header"
)
