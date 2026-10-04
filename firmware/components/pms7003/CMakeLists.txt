idf_component_register(
    SRCS "pms7003_parse.c" "pms7003_uart.c"
    INCLUDE_DIRS "include"
    # Reuse the existing board pin contract without depending on main.
    PRIV_INCLUDE_DIRS "../../main"
    REQUIRES esp_common freertos
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart
)
