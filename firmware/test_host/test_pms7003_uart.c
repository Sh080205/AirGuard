#include <stdio.h>
#include "test_frames.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"

static bool installed, read_error, endless_noise;
static unsigned init_step, fail_step;
static TickType_t ticks, arrival_delay;
static uint8_t input[256]; /* Test fixture capacity, not a sensor parameter. */
static size_t input_size, input_pos, max_chunk;
static TickType_t read_cost;

static esp_err_t config_result(void)
{
    ++init_step;
    return init_step == fail_step ? ESP_FAIL : ESP_OK;
}

bool uart_is_driver_installed(int port)
{
    assert(port == UART_NUM_1);
    return installed;
}

esp_err_t uart_param_config(int port, const uart_config_t *config)
{
    assert(port == UART_NUM_1);
    /* Manual V2.5 Appendix I: 9600 bps, 8-bit bytes, no parity, 1 stop. */
    assert(config->baud_rate == 9600 && config->data_bits == UART_DATA_8_BITS);
    assert(config->parity == UART_PARITY_DISABLE);
    assert(config->stop_bits == UART_STOP_BITS_1);
    assert(config->flow_ctrl == UART_HW_FLOWCTRL_DISABLE);
    return config_result();
}

esp_err_t uart_set_pin(int port, int tx, int rx, int rts, int cts)
{
    assert(port == UART_NUM_1);
    assert(tx == BOARD_PIN_PMS_RX && rx == BOARD_PIN_PMS_TX);
    assert(rts == UART_PIN_NO_CHANGE && cts == UART_PIN_NO_CHANGE);
    return config_result();
}

esp_err_t gpio_set_level(int pin, uint32_t level)
{
    assert(pin == BOARD_PIN_PMS_SET || pin == BOARD_PIN_PMS_RST);
    assert(level == 1); /* Manual V2.5 Pin Definition: both high for normal. */
    return config_result();
}

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config->pin_bit_mask == ((1ULL << BOARD_PIN_PMS_SET) |
                                    (1ULL << BOARD_PIN_PMS_RST)));
    assert(config->mode == GPIO_MODE_OUTPUT);
    assert(config->intr_type == GPIO_INTR_DISABLE);
    return config_result();
}

esp_err_t uart_driver_install(int port, int rx, int tx, int queue_size,
                              void *queue, int flags)
{
    assert(port == UART_NUM_1 && rx > UART_HW_FIFO_LEN(port));
    assert(tx == 0 && queue_size == 0 && queue == NULL && flags == 0);
    const esp_err_t err = config_result();
    installed = err == ESP_OK;
    return err;
}

TickType_t xTaskGetTickCount(void)
{
    return ticks;
}

int uart_read_bytes(int port, void *buf, uint32_t length, TickType_t wait)
{
    assert(port == UART_NUM_1 && installed && length > 0);
    assert(wait != portMAX_DELAY);
    /* The real driver's repeated per-chunk wait must never be reachable. */
    if (wait != 0) {
        assert(length == 1);
    }
    if (read_error) {
        return -1;
    }
    if (endless_noise) {
        memset(buf, 0xa5, length);
        ++ticks;
        return (int)length;
    }
    if (arrival_delay > 0) {
        const TickType_t advance = wait < arrival_delay ? wait : arrival_delay;
        ticks += advance;
        arrival_delay -= advance;
        if (arrival_delay > 0) {
            return 0;
        }
    }
    if (input_pos == input_size) {
        ticks += wait;
        return 0;
    }
    size_t n = input_size - input_pos;
    if (n > length) {
        n = length;
    }
    if (max_chunk > 0 && n > max_chunk) {
        n = max_chunk;
    }
    memcpy(buf, input + input_pos, n);
    input_pos += n;
    ticks += read_cost;
    return (int)n;
}

static void feed(const uint8_t *bytes, size_t size)
{
    assert(size <= sizeof(input));
    if (size > 0) {
        memcpy(input, bytes, size);
    }
    input_pos = 0;
    input_size = size;
    arrival_delay = 0;
    max_chunk = 0;
    read_cost = 0;
}

static void expect_read_error(esp_err_t expected, TickType_t timeout)
{
    pms7003_data_t out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    assert(pms7003_read(&out, timeout) == expected);
    assert(memcmp(saved, &out, sizeof(out)) == 0);
}

int main(void)
{
    pms7003_data_t out;
    expect_read_error(ESP_ERR_INVALID_STATE, 10);
    assert(pms7003_read(NULL, 10) == ESP_ERR_INVALID_ARG);
    expect_read_error(ESP_ERR_INVALID_ARG, portMAX_DELAY);
    installed = true;
    assert(pms7003_init() == ESP_ERR_INVALID_STATE);
    installed = false;
    for (fail_step = 1; fail_step <= 6; ++fail_step) {
        init_step = 0;
        assert(pms7003_init() == ESP_FAIL);
        assert(init_step == fail_step && !installed);
    }
    fail_step = 0;
    init_step = 0;
    assert(pms7003_init() == ESP_OK);
    assert(init_step == 6 && installed);
    assert(pms7003_init() == ESP_OK && init_step == 6);
    puts("PASS UART configuration, pin direction, all init failures and retry");

    uint8_t pair[64]; /* Two Appendix I frames, each 32 bytes. */
    make_frame(pair, sample);
    uint16_t other[13];
    memcpy(other, sample, sizeof(other));
    other[1] = 0xfedc;
    make_frame(pair + 32, other);
    feed(pair, sizeof(pair));
    assert(pms7003_read(&out, 0) == ESP_OK);
    check_data(&out, sample);
    assert(pms7003_read(&out, 0) == ESP_OK);
    check_data(&out, other);
    expect_read_error(ESP_ERR_TIMEOUT, 0);
    puts("PASS UART consecutive frames and zero-timeout polling");

    uint8_t three[96]; /* Three Appendix I frames, each 32 bytes. */
    uint16_t values[3][13];
    for (size_t i = 0; i < 3; ++i) {
        memcpy(values[i], sample, sizeof(values[i]));
        values[i][4] = (uint16_t)(0x1000U + i); /* Distinct PM2.5 atmospheric. */
        make_frame(three + 32 * i, values[i]);
    }
    feed(three, sizeof(three));
    pms7003_data_t newest;
    memset(&newest, 0, sizeof(newest));
    unsigned frames = 0;
    esp_err_t drain_err;
    while ((drain_err = pms7003_read(&out, 0)) == ESP_OK) {
        newest = out;
        ++frames;
    }
    assert(drain_err == ESP_ERR_TIMEOUT && frames == 3);
    check_data(&newest, values[2]);
    puts("PASS drain with zero timeout returns newest frame");

    feed(pair, 11); /* Synthetic chunk boundary inside a frame. */
    const TickType_t before_partial = ticks;
    expect_read_error(ESP_ERR_TIMEOUT, 7);
    assert((TickType_t)(ticks - before_partial) == 7);
    feed(pair + 11, 32 - 11);
    assert(pms7003_read(&out, 0) == ESP_OK);
    check_data(&out, sample);
    puts("PASS partial frame survives timeout and completes on next call");

    pair[4] ^= 1;
    feed(pair, sizeof(pair));
    assert(pms7003_read(&out, 10) == ESP_OK);
    check_data(&out, other);
    pair[4] ^= 1;
    pair[3] = 0; /* Wrong length, manual Appendix I requires 28. */
    feed(pair, sizeof(pair));
    assert(pms7003_read(&out, 10) == ESP_OK);
    check_data(&out, other);
    make_frame(pair, sample);
    puts("PASS UART resynchronizes after CRC and length errors");

    feed(pair, 32);
    max_chunk = 1;
    read_cost = 1;
    arrival_delay = 3;
    const TickType_t before_slow = ticks;
    expect_read_error(ESP_ERR_TIMEOUT, 9);
    assert((TickType_t)(ticks - before_slow) == 9);
    read_cost = 0;
    assert(pms7003_read(&out, 10) == ESP_OK);
    check_data(&out, sample);
    puts("PASS slow byte stream uses one total timeout budget");

    feed(NULL, 0);
    ticks = UINT32_MAX - 3;
    expect_read_error(ESP_ERR_TIMEOUT, 8);
    assert(ticks == 4);
    puts("PASS unplugged sensor timeout across tick counter wrap");

    endless_noise = true;
    const TickType_t before_noise = ticks;
    expect_read_error(ESP_ERR_TIMEOUT, 5);
    assert((TickType_t)(ticks - before_noise) == 5);
    endless_noise = false;
    read_error = true;
    expect_read_error(ESP_FAIL, 10);
    read_error = false;
    puts("PASS continuous noise deadline and UART read failure preserve output");
    puts("All UART mock tests passed (8 groups; no hardware validation).");
    return 0;
}
