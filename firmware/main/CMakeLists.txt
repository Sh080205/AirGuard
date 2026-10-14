# [ขึ้นกับเวอร์ชัน ESP-IDF] ชื่อ component สำหรับ ESP-IDF v5.5.x
idf_component_register(
    SRCS "main.c" "tasks_io.c" "tasks_sampler.c" "tasks_storage.c"
         "time_sync.c" "console.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio spi_flash esp_psram esp_common freertos log
             esp_driver_i2c esp_driver_uart esp_driver_spi esp_driver_sdspi
             fatfs sdmmc esp_wifi esp_netif esp_event nvs_flash esp_timer
             pms7003 aht21 storage logutil
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
