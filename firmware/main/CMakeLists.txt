# [ขึ้นกับเวอร์ชัน ESP-IDF] ชื่อ component สำหรับ ESP-IDF v5.5.x
idf_component_register(
    SRCS "main.c" "tasks_io.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio spi_flash esp_psram esp_common freertos log
)
