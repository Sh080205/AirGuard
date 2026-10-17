# FW-5b: ZE07-CO. ze07_parse.c is pure C (host-tested). The UART2 task and
# the CO safety logic are added in FW-5b step 3.
idf_component_register(
    SRCS "ze07_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
)
