# FW-5a: ENS160. ens160_parse.c is pure C (host-tested, uses logutil for
# the hold rule); ens160.c is bus access via the v5.x i2c_master driver.
idf_component_register(
    SRCS "ens160_parse.c" "ens160.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_driver_i2c logutil
)
