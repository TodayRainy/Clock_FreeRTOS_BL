set(STM32_SPL_SOURCES
    firmware/driver/src/stm32f4xx_dma.c
    firmware/driver/src/misc.c
    firmware/driver/src/stm32f4xx_flash.c
    firmware/driver/src/stm32f4xx_gpio.c
    firmware/driver/src/stm32f4xx_rcc.c
    firmware/driver/src/stm32f4xx_spi.c
    firmware/driver/src/stm32f4xx_tim.c
    firmware/driver/src/stm32f4xx_usart.c
)

set(BOOTLOADER_SOURCES
    app/main.c
    app/board.c
    app/boot_state.c
    app/bootloader.c
    app/boot_protocol.c
    app/firmware_auth.c
    app/firmware_slot.c
    app/magic_header.c
    app/ota_request.c
    app/gcc/jumpapp.s
    driver/bl24c512.c
    driver/bl_usart.c
    driver/console.c
    driver/key.c
    driver/led.c
    driver/stm32_flash.c
    driver/tim_delay.c
    driver/w25q.c
    firmware/cmsis/device/gcc/startup_stm32f407xx.s
    firmware/cmsis/device/system_stm32f4xx.c
    third_lib/crc/crc16.c
    third_lib/crc/crc32.c
    third_lib/sha256/sha256.c
    third_lib/micro-ecc/uECC.c
    ${CMAKE_SOURCE_DIR}/shared/watchdog.c
    third_lib/easylogger/elog.c
    third_lib/easylogger/elog_port.c
    third_lib/easylogger/elog_utils.c
    ${CMAKE_SOURCE_DIR}/cmake/runtime/syscalls.c
    ${STM32_SPL_SOURCES}
)

# ST 旧版系统文件用空循环等待 PLL 就绪；关闭这一处格式告警即可。
set_source_files_properties(
    firmware/cmsis/device/system_stm32f4xx.c
    PROPERTIES COMPILE_OPTIONS "-Wno-misleading-indentation"
)

add_executable(bootloader ${BOOTLOADER_SOURCES})
set_target_properties(bootloader PROPERTIES
    OUTPUT_NAME stm32f407
    SUFFIX .elf
)

target_compile_definitions(bootloader PRIVATE
    STM32F40_41xxx
    USE_STDPERIPH_DRIVER
    HSE_VALUE=8000000
    ${FIRMWARE_PROFILE_DEFINITION}
    uECC_SUPPORTS_secp160r1=0
    uECC_SUPPORTS_secp192r1=0
    uECC_SUPPORTS_secp224r1=0
    uECC_SUPPORTS_secp256r1=1
    uECC_SUPPORTS_secp256k1=0
    uECC_OPTIMIZATION_LEVEL=2
    uECC_SQUARE_FUNC=0
)

target_include_directories(bootloader PRIVATE
    app
    driver
    third_lib/crc
    third_lib/sha256
    third_lib/micro-ecc
    third_lib/easylogger
    ${FIRMWARE_GENERATED_DIR}
    ${CMAKE_SOURCE_DIR}/shared
    firmware/cmsis/core
    firmware/cmsis/device
    firmware/driver/inc
)

target_link_libraries(bootloader PRIVATE
    stm32f407_compile_options
    m
)

target_link_options(bootloader PRIVATE
    "-T${CMAKE_SOURCE_DIR}/cmake/bootloader.ld"
    "-L${CMAKE_SOURCE_DIR}/cmake"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/stm32f407.map,--cref"
)

add_custom_command(TARGET bootloader POST_BUILD
    COMMAND ${CMAKE_SIZE} stm32f407.elf
    COMMAND ${CMAKE_OBJCOPY} -O ihex stm32f407.elf stm32f407.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary -S stm32f407.elf stm32f407.bin
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating Bootloader HEX/BIN"
)
