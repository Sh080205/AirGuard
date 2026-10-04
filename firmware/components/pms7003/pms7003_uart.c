#include <stdbool.h>
#include <string.h>

#include "pms7003.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"

/* UART1 and pin net directions: CLAUDE.md section 4 / board_pins.h. */
#define PMS_UART UART_NUM_1
/* Application buffer choices, not sensor limits.
 * Stream buffer: two frames, so one read may hold a partial frame and its
 * successor.
 * Driver RX ring: Manual V2.5, Output result (p. 4): active mode sends every
 * 2.3 s when stable and every 200~800 ms in fast mode (big concentration
 * change). 16 frames x 200 ms = 3.2 s of fastest output may wait unread.
 * Frames are consumed oldest first, so a caller reading slower than the sensor
 * sends gets values that grow stale (up to ~3.2 s, then input may be lost when
 * the ring is full) unless it drains as described in pms7003.h.
 * [ค่าคาดหมาย ต้องยืนยันด้วยการวัด] real frame interval and overflow behavior.
 */
#define STREAM_BUFFER_SIZE (2U * PMS7003_FRAME_SIZE)
#define DRIVER_RX_BUFFER_SIZE (16U * PMS7003_FRAME_SIZE)
_Static_assert(DRIVER_RX_BUFFER_SIZE > UART_HW_FIFO_LEN(PMS_UART),
               "ESP-IDF UART RX ring buffer must exceed hardware FIFO");

static bool initialized;
static uint8_t stream_buffer[STREAM_BUFFER_SIZE];
static size_t stream_used;

esp_err_t pms7003_init(void)
{
    if (initialized) {
        return ESP_OK;
    }
    if (uart_is_driver_installed(PMS_UART)) {
        return ESP_ERR_INVALID_STATE; /* Do not take over another owner's port. */
    }
    const uart_config_t config = {
        /* Manual V2.5, Appendix I (p. 13): 9600 bps, high/low 8-bit bytes,
         * no check/parity bit, one stop bit. Pin Definition (p. 4) describes
         * RX/TX only; hardware flow control is not used.
         */
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(PMS_UART, &config);
    if (err != ESP_OK) {
        return err;
    }
    err = uart_set_pin(PMS_UART, BOARD_PIN_PMS_RX, BOARD_PIN_PMS_TX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }

    /* Manual V2.5, Pin Definition (p. 4): SET high = normal; RESET low
     * resets, so high releases reset. Preload high before enabling outputs.
     */
    err = gpio_set_level(BOARD_PIN_PMS_SET, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(BOARD_PIN_PMS_RST, 1);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t control = {
        .pin_bit_mask = (1ULL << BOARD_PIN_PMS_SET) | (1ULL << BOARD_PIN_PMS_RST),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&control);
    if (err != ESP_OK) {
        return err;
    }
    /* Install last: failures above cannot leak an installed driver. No TX
     * ring, event queue, or custom interrupt flags are needed for active RX.
     */
    err = uart_driver_install(PMS_UART, DRIVER_RX_BUFFER_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    stream_used = 0;
    initialized = true;
    return ESP_OK;
}

static void discard_prefix(size_t consumed)
{
    stream_used -= consumed;
    memmove(stream_buffer, stream_buffer + consumed, stream_used);
}

esp_err_t pms7003_read(pms7003_data_t *out, TickType_t timeout)
{
    if (out == NULL || timeout == portMAX_DELAY) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    const TickType_t started = xTaskGetTickCount();
    bool polled = false;

    for (;;) {
        /* Each retry discards at least one byte, so recovery is bounded by
         * the local buffer size even when the input contains many headers.
         */
        while (stream_used > 0) {
            size_t consumed;
            const esp_err_t err = pms7003_parse(stream_buffer, stream_used,
                                               out, &consumed);
            discard_prefix(consumed);
            if (err == ESP_OK) {
                return ESP_OK;
            }
            if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_FOUND) {
                break;
            }
            if (err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_RESPONSE) {
                return err;
            }
        }

        /* Unsigned tick subtraction tolerates a tick counter wrap. */
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if ((timeout == 0 && polled) || (timeout != 0 && elapsed >= timeout)) {
            return ESP_ERR_TIMEOUT;
        }
        int received = uart_read_bytes(PMS_UART, stream_buffer + stream_used,
                                        sizeof(stream_buffer) - stream_used, 0);
        polled = true;
        if (received < 0) {
            return ESP_FAIL;
        }
        if (received == 0) {
            if (timeout == 0) {
                return ESP_ERR_TIMEOUT;
            }
            const TickType_t spent = xTaskGetTickCount() - started;
            if (spent >= timeout) {
                return ESP_ERR_TIMEOUT;
            }
            /* ESP-IDF v5.5.x uart_read_bytes can wait again for each chunk.
             * Wait for ONE byte only, then drain nonblocking on the next loop,
             * so fragmented input cannot restart the caller's timeout budget.
             */
            received = uart_read_bytes(PMS_UART, stream_buffer + stream_used,
                                        1, timeout - spent);
            if (received < 0) {
                return ESP_FAIL;
            }
            if (received == 0) {
                return ESP_ERR_TIMEOUT;
            }
        }
        stream_used += (size_t)received;
    }
}
