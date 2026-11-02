# I2C bus recovery at boot: line sampling, 9-clock bus clear + STOP through
# callbacks, case classification (pure C, host-tested). The GPIO/probe side
# is in main/i2c_boot.c.
idf_component_register(
    SRCS "i2c_recover.c"
    INCLUDE_DIRS "include"
)
