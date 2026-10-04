# FW-3: frame parsing (pure C, host-tested).
# FW-4: aht21.c adds bus access via the v5.x i2c_master driver.
idf_component_register(
    SRCS "aht21_parse.c" "aht21.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c
)
