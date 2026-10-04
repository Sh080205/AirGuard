# FW-3: frame parsing only. I2C access is added in FW-4.
idf_component_register(
    SRCS "aht21_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)
