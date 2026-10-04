#ifndef TEST_UART_H
#define TEST_UART_H
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
enum { UART_NUM_1 = 1, UART_DATA_8_BITS, UART_PARITY_DISABLE,
       UART_STOP_BITS_1, UART_HW_FLOWCTRL_DISABLE, UART_SCLK_DEFAULT };
#define UART_PIN_NO_CHANGE (-1)
/* ESP-IDF v5.5.5 soc/esp32s3/include/soc/soc_caps.h SOC_UART_FIFO_LEN. */
#define UART_HW_FIFO_LEN(port) 128
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, source_clk;
} uart_config_t;
bool uart_is_driver_installed(int port);
esp_err_t uart_param_config(int port, const uart_config_t *config);
esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts);
esp_err_t uart_driver_install(int port, int rx, int tx, int queue_size,
                              void *queue, int flags);
int uart_read_bytes(int port, void *buf, uint32_t length, TickType_t wait);
#endif
