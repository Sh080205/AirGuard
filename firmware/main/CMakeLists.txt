# [ขึ้นกับเวอร์ชัน ESP-IDF] ชื่อ component สำหรับ ESP-IDF v5.5.x
idf_component_register(
    SRCS "main.c" "tasks_io.c" "tasks_sampler.c" "tasks_storage.c"
         "time_sync.c" "console.c" "tasks_co.c" "ml_mailbox.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio spi_flash esp_psram esp_common freertos log
             esp_driver_i2c esp_driver_uart esp_driver_spi esp_driver_sdspi
             fatfs sdmmc esp_wifi esp_netif esp_event nvs_flash esp_timer
             pms7003 aht21 ens160 storage logutil ze07 mahal
)

# FW-5a: firmware/sdkconfig that predates partitions.csv keeps the default
# 1 MB single-app table, because sdkconfig.defaults only seeds a NEW
# sdkconfig. Stop at configure time instead of building an image that
# overflows the old 1 MB partition later or flashes the wrong table.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    if(NOT CONFIG_PARTITION_TABLE_CUSTOM
       OR NOT CONFIG_PARTITION_TABLE_CUSTOM_FILENAME STREQUAL "partitions.csv")
        message(FATAL_ERROR
            "sdkconfig does not use firmware/partitions.csv (factory 4 MB). "
            "Delete firmware/sdkconfig and rebuild so sdkconfig.defaults apply "
            "(see firmware/README.md, partition table).")
    endif()
endif()

# FW-5a heater test: -DENS160_ENABLE=0 keeps the ENS160 in DEEP SLEEP
# (tasks_sampler.c). idf.py -D only sets a CMake cache variable, so it is
# forwarded to the compiler here.
# Review 17 Oct item 2: an OFF value left in the normal build/ would make a
# later plain `idf.py build flash` log a training run with the heater off.
# Removing it from the cache is not enough: a plain `idf.py build` does not
# re-run CMake, so build.ninja keeps -DENS160_ENABLE=0 (tested). Instead an
# OFF build is refused in build/ and must use its own build directory:
#   idf.py -B build/heater_off -DENS160_ENABLE=0 build
#   idf.py -B build/heater_off -p COM7 flash
# (inside build/, so git already ignores it). There the value stays cached,
# so that directory is always OFF and build/ is always ON.
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND DEFINED ENS160_ENABLE)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" airguard_bin_dir)
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/build" airguard_default_bin_dir)
    if(NOT ENS160_ENABLE AND airguard_bin_dir STREQUAL airguard_default_bin_dir)
        unset(ENS160_ENABLE CACHE)
        message(FATAL_ERROR
            "ENS160_ENABLE=0 is not allowed in firmware/build (it would stick to "
            "later normal builds). Use a separate directory: "
            "idf.py -B build/heater_off -DENS160_ENABLE=0 build")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ENS160_ENABLE=${ENS160_ENABLE})
    message(STATUS "ENS160_ENABLE=${ENS160_ENABLE} (build dir ${airguard_bin_dir})")
endif()

# FW-4: Wi-Fi/SNTP only when main/secrets.h exists (git-ignored).
# CONFIGURE_DEPENDS re-checks the glob on every build, so creating or
# deleting secrets.h after the first build re-runs CMake and recompiles
# time_sync.c; a plain __has_include would keep the old object file.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    file(GLOB AIRGUARD_SECRETS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/secrets.h")
    if(AIRGUARD_SECRETS)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE AIRGUARD_HAVE_SECRETS=1)
    else()
        message(STATUS "main/secrets.h not found: building without Wi-Fi/SNTP (settime only)")
    endif()
endif()

# FW-5b: -DZE07_ENABLE=0 builds without the CO path (no UART2/co task,
# co_ppm empty) for a board without the ZE07 wired. Same rule as
# ENS160_ENABLE above: an OFF value must not stick to the normal build/,
# because a later plain `idf.py build flash` would then silently run without
# the CO path. Use its own directory:
#   idf.py -B build/co_off -DZE07_ENABLE=0 build
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND DEFINED ZE07_ENABLE)
    file(REAL_PATH "${CMAKE_BINARY_DIR}" airguard_bin_dir)
    file(REAL_PATH "${CMAKE_SOURCE_DIR}/build" airguard_default_bin_dir)
    if(NOT ZE07_ENABLE AND airguard_bin_dir STREQUAL airguard_default_bin_dir)
        unset(ZE07_ENABLE CACHE)
        message(FATAL_ERROR
            "ZE07_ENABLE=0 is not allowed in firmware/build (it would stick to "
            "later normal builds). Use a separate directory: "
            "idf.py -B build/co_off -DZE07_ENABLE=0 build")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ZE07_ENABLE=${ZE07_ENABLE})
    message(STATUS "ZE07_ENABLE=${ZE07_ENABLE} (build dir ${airguard_bin_dir})")
endif()
