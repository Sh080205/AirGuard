# FW-5b: ZE07-CO. All pure C (host-tested): frame parser, CO decision logic
# (co_safety) and its LED/buzzer policy (co_alert). The UART2 task that uses
# them is main/tasks_co.c.
idf_component_register(
    SRCS "ze07_parse.c" "co_safety.c" "co_alert.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)
