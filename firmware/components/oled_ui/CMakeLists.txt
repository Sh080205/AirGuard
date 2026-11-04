# OLED status screen: text layout, font, frame buffer, controller command
# bytes (pure C, host-tested). The I2C traffic and the task are in
# main/tasks_oled.c.
idf_component_register(
    SRCS "oled_ui.c" "oled_font.c"
    INCLUDE_DIRS "include"
)
