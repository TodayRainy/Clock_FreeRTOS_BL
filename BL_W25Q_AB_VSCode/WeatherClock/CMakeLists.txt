set(STM32_SPL_SOURCES
    firmware/driver/src/misc.c
    firmware/driver/src/stm32f4xx_dma.c
    firmware/driver/src/stm32f4xx_exti.c
    firmware/driver/src/stm32f4xx_gpio.c
    firmware/driver/src/stm32f4xx_i2c.c
    firmware/driver/src/stm32f4xx_pwr.c
    firmware/driver/src/stm32f4xx_rcc.c
    firmware/driver/src/stm32f4xx_rtc.c
    firmware/driver/src/stm32f4xx_spi.c
    firmware/driver/src/stm32f4xx_syscfg.c
    firmware/driver/src/stm32f4xx_tim.c
    firmware/driver/src/stm32f4xx_usart.c
)

set(WEATHERCLOCK_SOURCES
    app/main.c
    app/ab_confirm.c
    app/health_monitor.c
    app/ota.c
    app/board.c
    app/wifi.c
    app/weather.c
    app/app.c
    app/ui.c
    app/workqueue.c
    app/font/font16_maple.c
    app/font/font20_maple_bold.c
    app/font/font24_maple_semibold.c
    app/font/font24_maple_bold.c
    app/font/font32_maple_bold.c
    app/font/font54_maple_bold.c
    app/font/font54_maple_semibold.c
    app/font/font64_maple_extrabold.c
    app/font/font76_maple_extrabold.c
    app/image/img_meihua_rle.c
    app/image/img_error_rle.c
    app/image/img_wifi_rle.c
    app/image/icon_wifi.c
    app/image/icon_wenduji.c
    app/image/icon_duoyun.c
    app/image/icon_leizhenyu.c
    app/image/icon_qing.c
    app/image/icon_yintian.c
    app/image/icon_yueliang.c
    app/image/icon_zhongxue.c
    app/image/icon_zhongyu.c
    app/image/icon_na.c
    app/page/welcome_page.c
    app/page/error_page.c
    app/page/wifi_page.c
    app/page/main_page.c
    driver/bl24c512/bl24c512.c
    driver/rtc/rtc.c
    driver/console/console.c
    driver/aht20/aht20.c
    driver/st7789/st7789.c
    driver/esp_at/esp_at.c
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/driver/w25q.c
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/app/magic_header.c
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/third_lib/crc/crc32.c
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/third_lib/sha256/sha256.c
    driver/tim_delay/tim_delay.c
    driver/led/led.c
    firmware/cmsis/device/gcc/startup_stm32f407xx.s
    firmware/cmsis/device/system_stm32f4xx.c
    third_lib/freertos/list.c
    third_lib/freertos/queue.c
    third_lib/freertos/tasks.c
    third_lib/freertos/timers.c
    ${CMAKE_SOURCE_DIR}/shared/watchdog.c
    third_lib/freertos/portable/heap_4.c
    third_lib/freertos/portable/GCC/ARM_CM4F/port.c
    ${CMAKE_SOURCE_DIR}/cmake/runtime/syscalls.c
    ${STM32_SPL_SOURCES}
)

# 这些 GBK 编码的遗留文件保持原样，逐文件屏蔽 GCC 对已知写法的噪声告警。
set_source_files_properties(
    firmware/cmsis/device/system_stm32f4xx.c
    driver/st7789/st7789.c
    PROPERTIES COMPILE_OPTIONS "-Wno-misleading-indentation"
)
set_source_files_properties(
    app/page/main_page.c
    app/page/welcome_page.c
    app/page/wifi_page.c
    # 页面源码统一为UTF-8；执行字符集仍保持GBK，
    # 因为现有点阵字库和LCD字符串解析器使用两个字节的GB2312编码。
    PROPERTIES COMPILE_OPTIONS "-fexec-charset=GBK"
)
set_source_files_properties(
    driver/esp_at/esp_at.c
    PROPERTIES COMPILE_OPTIONS "-Wno-sign-compare"
)

# 大图的原始RGB565数组继续作为可追溯源文件；需要更新资源时显式运行本目标。
add_custom_target(generate_rle_images
    COMMAND ${Python3_EXECUTABLE} scripts/encode_rgb565_rle.py
            app/image/img_meihua.c app/image/img_meihua_rle.c
            --symbol img_meihua --width 180 --height 180
    COMMAND ${Python3_EXECUTABLE} scripts/encode_rgb565_rle.py
            app/image/img_error.c app/image/img_error_rle.c
            --symbol img_error --width 160 --height 160
    COMMAND ${Python3_EXECUTABLE} scripts/encode_rgb565_rle.py
            app/image/img_wifi.c app/image/img_wifi_rle.c
            --symbol img_wifi --width 180 --height 164
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Regenerating compressed RGB565 image sources"
)

add_executable(weatherclock ${WEATHERCLOCK_SOURCES})
set_target_properties(weatherclock PROPERTIES
    OUTPUT_NAME stm32f407
    SUFFIX .elf
)
add_dependencies(weatherclock firmware_aes_gcm_tool)

target_compile_definitions(weatherclock PRIVATE
    STM32F40_41xxx
    USE_STDPERIPH_DRIVER
    HSE_VALUE=8000000
    ${FIRMWARE_PROFILE_DEFINITION}
    APP_VERSION="${WEATHERCLOCK_VERSION}"
    # Debug保留完整AT收发日志；Release只保留驱动外层的状态和错误信息。
    $<$<CONFIG:Release>:ESP_AT_DEBUG=0>
)

target_include_directories(weatherclock PRIVATE
    app
    app/font
    app/image
    app/page
    driver/tim_delay
    driver/rtc
    driver/console
    driver/aht20
    driver/st7789
    driver/esp_at
    driver/bl24c512
    driver/led
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/driver
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/app
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/third_lib/crc
    ${CMAKE_SOURCE_DIR}/STM32F4_Bootloader/third_lib/sha256
    firmware/cmsis/core
    firmware/cmsis/device
    firmware/driver/inc
    third_lib/freertos/include
    third_lib/freertos/portable/GCC/ARM_CM4F
    third_lib/freertos/portable
    ${CMAKE_SOURCE_DIR}/shared
)

target_link_libraries(weatherclock PRIVATE
    stm32f407_compile_options
    m
)

target_link_options(weatherclock PRIVATE
    "-T${CMAKE_SOURCE_DIR}/cmake/weatherclock.ld"
    "-L${CMAKE_SOURCE_DIR}/cmake"
    "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/stm32f407.map,--cref"
)

add_custom_command(TARGET weatherclock POST_BUILD
    COMMAND ${CMAKE_SIZE} stm32f407.elf
    COMMAND ${CMAKE_OBJCOPY} -O ihex stm32f407.elf stm32f407.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary -S stm32f407.elf stm32f407.bin
    COMMAND ${Python3_EXECUTABLE}
            ../../../WeatherClock/scripts/gen_magic_header.py
            stm32f407.bin
            stm32f407_upgrade.xbin
            --private-key ${FIRMWARE_SIGNING_KEY}
            --public-key ${FIRMWARE_VERIFY_KEY}
            --openssl ${FIRMWARE_OPENSSL_EXECUTABLE}
            --encryption-key ${FIRMWARE_ENCRYPTION_KEY}
            --aes-tool ${FIRMWARE_AES_TOOL}
            --security-version ${FIRMWARE_SECURITY_VERSION}
            --release-sequence ${FIRMWARE_RELEASE_SEQUENCE}
            --version ${WEATHERCLOCK_VERSION}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    COMMENT "Generating WeatherClock HEX/BIN/encrypted XBIN"
)
