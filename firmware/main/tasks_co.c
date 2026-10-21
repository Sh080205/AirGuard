#include "tasks_co.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "co_safety.h"
#include "task_config.h"
#include "tasks_io.h"
#include "ze07.h"

static const char *TAG = "co";

#if ZE07_ENABLE

#define CO_UART UART_NUM_2
/* Manual V1.7 p.4 Table 3: 9600 baud, 8 data bits, 1 stop bit, no parity. */
#define CO_BAUD 9600
/* Driver RX ring: must exceed the 128-byte hardware FIFO (ESP-IDF rule, same
 * check as pms7003_uart.c). 256 B = 28 frames, far more than can arrive
 * between two reads (a read returns at most CO_READ_WAIT_MS apart). */
#define CO_RX_RING_BYTES 256
_Static_assert(CO_RX_RING_BYTES > UART_HW_FIFO_LEN(CO_UART),
               "ESP-IDF UART RX ring buffer must exceed hardware FIFO");
/* Local parse buffer: the parser keeps at most 8 bytes of a partial frame,
 * so 64 B always leaves room for a new read. */
#define CO_BUF_BYTES 64
/* Longest block on the UART: also the period of the silence check, the
 * snapshot refresh and the task watchdog reset (5 s TWDT). Firmware policy. */
#define CO_READ_WAIT_MS 200
#define CO_SUMMARY_US (60LL * 1000 * 1000)
/* First frames are logged in hex to confirm the frame format on the real
 * module (the parser only knows manual V1.7). */
#define CO_HEX_LOG_FRAMES 5U
#define CO_BAD_LOG_FRAMES 3U

/* Created by co_mailbox_init() before the io/sampler tasks exist and never
 * changed afterwards, so they read the handle without a lock. */
static QueueHandle_t s_mailbox;

/* Owned by the CO task only (after co_start()). Static, not on the stack:
 * co_safety_t holds 480 one-minute buckets (~4.3 KB). */
static co_safety_t s_safety;
static uint8_t s_buf[CO_BUF_BYTES];
static size_t s_used;
static bool s_uart_ok;
static uint32_t s_seq;
static uint32_t s_uart_err, s_dbg_err, s_bytes, s_crc_err, s_field_err, s_hex_logged, s_bad_logged;
static co_status_t s_last;   /* last published, to spot changes */

static void log_hex(const char *what, const uint8_t *f, size_t n)
{
    char hex[3 * ZE07_FRAME_SIZE + 1];
    size_t o = 0;
    for (size_t i = 0; i < n && i < ZE07_FRAME_SIZE; ++i) {
        static const char digits[] = "0123456789ABCDEF";
        hex[o++] = digits[f[i] >> 4];
        hex[o++] = digits[f[i] & 0x0F];
        hex[o++] = ' ';
    }
    hex[o] = '\0';
    ESP_LOGI(TAG, "%s: %s", what, hex);
}

static void parse_buffer(int64_t now_us, bool *got_frame)
{
    for (;;) {
        ze07_reading_t r;
        size_t consumed = 0;
        const esp_err_t err = ze07_parse(s_buf, s_used, &r, &consumed);
        if (err == ESP_OK) {
            if (s_hex_logged < CO_HEX_LOG_FRAMES) {
                ++s_hex_logged;
                log_hex("frame", s_buf + consumed - ZE07_FRAME_SIZE, ZE07_FRAME_SIZE);
            }
            co_safety_on_frame(&s_safety, now_us, r.conc_x10);
            *got_frame = true;
        } else if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_RESPONSE) {
            if (err == ESP_ERR_INVALID_CRC) {
                ++s_crc_err;
            } else {
                ++s_field_err;
            }
            if (s_bad_logged < CO_BAD_LOG_FRAMES) {
                ++s_bad_logged;
                /* consumed - 1 = start byte of the rejected 9 bytes */
                log_hex(err == ESP_ERR_INVALID_CRC ? "rejected (checksum)" : "rejected (fields)",
                        s_buf + consumed - 1, ZE07_FRAME_SIZE);
            }
            co_safety_on_bad_frame(&s_safety);
        }
        memmove(s_buf, s_buf + consumed, s_used - consumed);
        s_used -= consumed;
        if (err != ESP_OK && err != ESP_ERR_INVALID_CRC && err != ESP_ERR_INVALID_RESPONSE) {
            return;   /* NOT_FOUND / INVALID_SIZE: wait for more bytes */
        }
    }
}

/* Block for the first byte (<= CO_READ_WAIT_MS), then take what is already
 * buffered without waiting. Reading a fixed length would make
 * uart_read_bytes() wait the whole timeout for bytes that never come. */
static void read_uart(void)
{
    if (s_used >= CO_BUF_BYTES) {
        s_used = 0;   /* cannot happen: the parser leaves <= 8 bytes */
    }
    int n = uart_read_bytes(CO_UART, s_buf + s_used, 1, pdMS_TO_TICKS(CO_READ_WAIT_MS));
    if (n < 0) {
        ++s_uart_err;
        return;
    }
    s_used += (size_t)n;
    s_bytes += (uint32_t)n;
    if (n == 0) {
        return;
    }
    size_t buffered = 0;
    if (uart_get_buffered_data_len(CO_UART, &buffered) != ESP_OK) {
        ++s_uart_err;
        return;
    }
    size_t room = CO_BUF_BYTES - s_used;
    if (buffered > room) {
        buffered = room;
    }
    if (buffered == 0) {
        return;
    }
    n = uart_read_bytes(CO_UART, s_buf + s_used, buffered, 0);
    if (n < 0) {
        ++s_uart_err;
        return;
    }
    s_used += (size_t)n;
    s_bytes += (uint32_t)n;
}

static void log_transitions(const co_status_t *now, const co_status_t *old)
{
    if (now->alarm != old->alarm) {
        if (now->alarm) {
            ESP_LOGW(TAG, "CO ALARM: reading %u.%u ppm, 1-h mean %u.%u ppm (reasons 0x%02x)",
                     now->conc_x10 / 10U, now->conc_x10 % 10U,
                     now->mean_1h_x10 / 10U, now->mean_1h_x10 % 10U, now->alarm_reasons);
        } else {
            ESP_LOGI(TAG, "CO ALARM cleared (reading %u.%u ppm, 1-h mean %u.%u ppm)",
                     now->conc_x10 / 10U, now->conc_x10 % 10U,
                     now->mean_1h_x10 / 10U, now->mean_1h_x10 % 10U);
        }
    }
    if (now->fault != old->fault) {
        if (now->fault) {
            ESP_LOGE(TAG, "CO FAULT (%s): no valid ZE07 frame for > %lld ms",
                     (now->fault_reasons & CO_FAULT_HW) ? "UART2 setup failed" : "sensor silent",
                     (long long)(CO_SILENCE_US / 1000));
        } else {
            ESP_LOGI(TAG, "CO FAULT cleared (%u valid frames in a row)", CO_FAULT_EXIT_FRAMES);
        }
    }
    if (now->elevated_8h != old->elevated_8h) {
        ESP_LOGI(TAG, "CO 8-h mean %s 9 ppm (WHO 8 h) - flag only: %u.%u ppm",
                 now->elevated_8h ? "reached" : "back under", now->mean_8h_x10 / 10U,
                 now->mean_8h_x10 % 10U);
    }
    if (now->warmup != old->warmup && !now->warmup) {
        ESP_LOGI(TAG, "CO warm-up done (%lld s): co_ppm is written from now on",
                 (long long)(CO_WARMUP_US / 1000000));
    }
}

static void publish(int64_t now_us, bool frame_this_round)
{
    co_status_t st;
    co_safety_status(&s_safety, now_us, &st);
    st.updated_us = now_us;
    st.seq = ++s_seq;
    /* length-1 queue: overwrite never blocks and never fails */
    (void)xQueueOverwrite(s_mailbox, &st);

    const bool shown_changed = st.alarm != s_last.alarm || st.fault != s_last.fault ||
                               st.alarm_reasons != s_last.alarm_reasons;
    if (frame_this_round) {
        /* GPIO7 HIGH from "frame decided and published" until the io task
         * has been told: rising edge vs the last UART byte on GPIO5 and vs
         * LED_R/BUZZ gives the latency split (FW-5b step 3). */
        if (gpio_set_level(BOARD_PIN_DBG_CO, 1) != ESP_OK) {
            ++s_dbg_err;
        }
    }
    if (shown_changed) {
        const esp_err_t err = io_notify_co();
        if (err != ESP_OK) {
            /* io queue full: the io task still re-reads the mailbox within
             * its 100 ms bound, so the change is late, not lost */
            ESP_LOGW(TAG, "io notify failed (%s); io picks it up on its next wake",
                     esp_err_to_name(err));
        }
    }
    if (frame_this_round && gpio_set_level(BOARD_PIN_DBG_CO, 0) != ESP_OK) {
        ++s_dbg_err;
    }
    log_transitions(&st, &s_last);
    s_last = st;
}

static void summary(int64_t now_us)
{
    const co_status_t *st = &s_last;
    const int64_t age_ms = st->have_frame ? (now_us - st->frame_us) / 1000 : -1;
    ESP_LOGI(TAG, "since boot: frames ok=%" PRIu32 " bad(crc=%" PRIu32 " fields=%" PRIu32
             ") bytes=%" PRIu32 " uart_err=%" PRIu32 " dbg_err=%" PRIu32 " | alarms=%" PRIu32 " faults=%" PRIu32
             " | now: %u.%u ppm (age %lld ms) 1h=%u.%u 8h=%u.%u warmup=%d alarm=%d fault=%d | "
             "stack HWM co=%u B",
             s_safety.frames_ok, s_crc_err, s_field_err, s_bytes, s_uart_err, s_dbg_err,
             s_safety.alarms_entered, s_safety.faults_entered,
             st->conc_x10 / 10U, st->conc_x10 % 10U, (long long)age_ms,
             st->mean_1h_x10 / 10U, st->mean_1h_x10 % 10U,
             st->mean_8h_x10 / 10U, st->mean_8h_x10 % 10U,
             st->warmup, st->alarm, st->fault,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static void co_task(void *arg)
{
    (void)arg;
    const esp_err_t wdt = esp_task_wdt_add(NULL);
    if (wdt != ESP_OK) {
        ESP_LOGW(TAG, "task watchdog not attached: %s", esp_err_to_name(wdt));
    }
    int64_t next_summary = esp_timer_get_time() + CO_SUMMARY_US;
    for (;;) {
        if (s_uart_ok) {
            read_uart();
        } else {
            vTaskDelay(pdMS_TO_TICKS(CO_READ_WAIT_MS));
        }
        const int64_t now = esp_timer_get_time();
        bool got_frame = false;
        parse_buffer(now, &got_frame);
        co_safety_tick(&s_safety, now);
        publish(now, got_frame);
        if (now >= next_summary) {
            next_summary = now + CO_SUMMARY_US;
            summary(now);
        }
        if (wdt == ESP_OK) {
            const esp_err_t err = esp_task_wdt_reset();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "task watchdog reset failed: %s", esp_err_to_name(err));
            }
        }
    }
}

static esp_err_t uart_setup(void)
{
    if (uart_is_driver_installed(CO_UART)) {
        return ESP_ERR_INVALID_STATE;   /* do not take over another owner's port */
    }
    const uart_config_t config = {
        .baud_rate = CO_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(CO_UART, &config);
    if (err != ESP_OK) {
        return err;
    }
    /* RX only. TX is left unrouted and GPIO6 (CO_RX net) is not touched:
     * the module RXD is rated 0-3.0 V (manual p.4) and the firmware sends no
     * commands (initiative upload mode); J2.4 stays unconnected (owner
     * decision 18 Oct 2026). */
    err = uart_set_pin(CO_UART, UART_PIN_NO_CHANGE, BOARD_PIN_CO_TX,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        return err;
    }
    /* The CO_TX net has no pull resistor on the board (docs/02) and
     * uart_set_pin() adds none [ขึ้นกับเวอร์ชัน ESP-IDF: v5.5.5 uart.c], so
     * with J2 unplugged or J2.3 broken GPIO5 floats and picks up noise.
     * Internal pull-down (owner decision 21 Oct 2026) holds it LOW instead:
     * it never raises the module TXD pin above what the module drives
     * (manual p.4: 0-3.0 V), unlike a pull-up to 3.3 V; the module only
     * sources about 3.0 V / R_PD when it sends HIGH (R_PD
     * [ต้องเปิด datasheet: ESP32-S3 Datasheet, DC characteristics]; tens of
     * uA, ค่าคาดหมาย). A line held LOW is a break, not valid frames, so
     * FAULT still comes from "no valid frame for 6 s". */
    err = gpio_set_pull_mode(BOARD_PIN_CO_TX, GPIO_PULLDOWN_ONLY);
    if (err != ESP_OK) {
        return err;
    }
    return uart_driver_install(CO_UART, CO_RX_RING_BYTES, 0, 0, NULL, 0);
}

esp_err_t co_mailbox_init(void)
{
    if (s_mailbox != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mailbox = xQueueCreate(1, sizeof(co_status_t));
    if (s_mailbox == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const int64_t now = esp_timer_get_time();
    co_safety_init(&s_safety, now);
    co_safety_status(&s_safety, now, &s_last);
    s_last.updated_us = now;
    (void)xQueueOverwrite(s_mailbox, &s_last);
    return ESP_OK;
}

esp_err_t co_start(void)
{
    if (s_mailbox == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t dbg = gpio_set_level(BOARD_PIN_DBG_CO, 0);
    if (dbg == ESP_OK) {
        const gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << BOARD_PIN_DBG_CO,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        dbg = gpio_config(&cfg);
    }
    if (dbg != ESP_OK) {
        ESP_LOGW(TAG, "debug pin GPIO%d setup failed: %s", BOARD_PIN_DBG_CO, esp_err_to_name(dbg));
    }

    const esp_err_t uart = uart_setup();
    s_uart_ok = uart == ESP_OK;
    if (!s_uart_ok) {
        co_safety_set_hw_fault(&s_safety);
        ESP_LOGE(TAG, "UART2 setup failed: %s - CO path in FAULT", esp_err_to_name(uart));
    } else {
        ESP_LOGI(TAG, "ZE07-CO on UART2 RX GPIO%d, %d 8N1, TX not used; warm-up %lld s, "
                      "FAULT after %lld ms silence. Research logging only - NOT a CO alarm.",
                 BOARD_PIN_CO_TX, CO_BAUD, (long long)(CO_WARMUP_US / 1000000),
                 (long long)(CO_SILENCE_US / 1000));
    }
    const BaseType_t ok = xTaskCreatePinnedToCore(co_task, "co", CO_STACK_BYTES, NULL,
                                                  CO_TASK_PRIORITY, NULL, CO_TASK_CORE);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return uart;
}

bool co_peek_status(co_status_t *out)
{
    return s_mailbox != NULL && xQueuePeek(s_mailbox, out, 0) == pdTRUE;
}

#else  /* ZE07_ENABLE == 0 */

esp_err_t co_mailbox_init(void)
{
    return ESP_OK;
}

esp_err_t co_start(void)
{
    ESP_LOGW(TAG, "CO path disabled at build time (ZE07_ENABLE=0): no UART2, co_ppm empty, "
                  "no CO ALARM/FAULT");
    return ESP_OK;
}

bool co_peek_status(co_status_t *out)
{
    (void)out;
    return false;
}

#endif
