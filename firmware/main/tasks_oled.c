#include "tasks_oled.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ag_fsm.h"
#include "ag_mahal.h"
#include "co_safety.h"
#include "logutil.h"
#include "ml_mailbox.h"
#include "oled_ui.h"
#include "task_config.h"
#include "tasks_co.h"
#include "tasks_io.h"

/* ── The one place to change for the real panel (OLED.md step 3) ──────────
 * Controller not confirmed (docs/bringup/2026-10-23.md O0). If the test
 * pattern's left edge is missing and the right 2 columns show noise, the
 * chip is an SH1106: set OLED_CHIP to OLED_CHIP_SH1106. If the picture is
 * upside down in the lid, flip OLED_ROTATE_180. Address 0x3C: CLAUDE.md §5. */
#define OLED_CHIP         OLED_CHIP_SSD1306
#define OLED_ROTATE_180   false
#define OLED_I2C_ADDR     0x3C

/* Same clock as the sampler's devices (tasks_sampler.c I2C_SCL_HZ): the
 * chunk timing in tasks_oled.h is computed at 100 kHz. */
#define OLED_SCL_HZ       100000U
/* Includes the wait for the bus lock (i2c_master v5.5.5), like the
 * sampler's 20 ms; a chunk takes ≈ 6 ms. */
#define OLED_XFER_TIMEOUT_MS 20

/* Send window: from the sampler's publish (its I2C work done) until 700 ms
 * after its cycle start; the next cycle starts ≈ 1000 ms after the last.
 * A chunk is started only if OLED_CHUNK_BUDGET_US still fits before the
 * end (≈ 6.4 ms per chunk + margin for being preempted). */
#define OLED_WINDOW_END_US      700000LL
#define OLED_CHUNK_BUDGET_US     15000LL
/* No row for 1.5 s = the sampler is late or stopped. The screen still
 * updates (it must show SAMPLER STALE and the CO state) in a short window;
 * layer 1 (one chunk at a time) still bounds what the sampler can wait. */
#define OLED_NO_ROW_WAIT_MS       1500U
#define OLED_NO_ROW_WINDOW_US   300000LL
/* A window that opens this late after the cycle start is counted ("late"):
 * the screen task was held off by higher-priority work. */
#define OLED_LATE_US            200000LL

/* Freshness. Row: 3 s like the FSM's ML_STALE (the sampler stopped). io
 * passes are <= 100 ms apart, so 1 s old means the io task stopped. */
#define OLED_ROW_FRESH_US      3000000LL
#define OLED_IO_FRESH_US       1000000LL
#define OLED_ML_FRESH_US       AG_FSM_ML_STALE_US
/* co_ppm hold rule of the CSV (tasks_sampler.c CO_MAX_AGE_US), so the
 * screen shows the value the row gets */
#define OLED_CO_MAX_AGE_US     PMS_MAX_AGE_US

/* Not answering: probe again every 30 s (one NACKed address byte
 * ≈ 0.1 ms). 3 errors in a row = the screen is gone. Firmware policy. */
#define OLED_PROBE_RETRY_US   (30LL * 1000 * 1000)
#define OLED_ERRORS_TO_DROP      3U
/* Boot: test pattern (offset/orientation check) then CLAUDE.md §8 */
#define OLED_PATTERN_US       (3LL * 1000 * 1000)
#define OLED_BOOT_TEXT_US     (8LL * 1000 * 1000)
#define OLED_SUMMARY_US       (60LL * 1000 * 1000)

_Static_assert(OLED_FAULT_AHT21 == AG_FSM_FAULT_AHT21 &&
               OLED_FAULT_ENS160 == AG_FSM_FAULT_ENS160 &&
               OLED_FAULT_PMS7003 == AG_FSM_FAULT_PMS7003 &&
               OLED_FAULT_ML_MODEL == AG_FSM_FAULT_ML_MODEL &&
               OLED_FAULT_ML_STALE == AG_FSM_FAULT_ML_STALE,
               "OLED fault bits must match AG_FSM_FAULT_*");

typedef struct {
    csv_sample_t row;
    bool time_valid;
    int64_t t0_us;
} oled_row_t;

/* Row mailbox (sampler → screen). Length 1, written with xQueueOverwrite
 * (the sampler never blocks or waits for the screen) and read with
 * xQueueReceive by the screen task only: receiving empties it, so the
 * screen task wakes exactly once per new row — that wake-up is the "bus
 * free" signal of layer 2. A copy of the whole struct under the queue's
 * lock: never half of one row and half of the next (volatile would not give
 * that across cores, CLAUDE.md §2). Static storage; the handle is written
 * before the sampler task exists. */
static QueueHandle_t s_row_q;   /* stays NULL with OLED_ENABLE=0 */
#if OLED_ENABLE
static StaticQueue_t s_row_q_struct;
static uint8_t s_row_q_storage[sizeof(oled_row_t)];
#endif

esp_err_t oled_mailbox_init(void)
{
#if !OLED_ENABLE
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (s_row_q != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_row_q = xQueueCreateStatic(1, sizeof(oled_row_t), s_row_q_storage, &s_row_q_struct);
    return s_row_q != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
#endif
}

void oled_publish_row(const csv_sample_t *row, bool time_valid, int64_t t0_us)
{
    if (s_row_q == NULL || row == NULL) {
        return;
    }
    const oled_row_t r = { .row = *row, .time_valid = time_valid, .t0_us = t0_us };
    (void)xQueueOverwrite(s_row_q, &r);   /* length 1: always pdPASS */
}

/* Everything below runs only in a build with the screen. */
#if OLED_ENABLE

static const char *TAG = "oled";

/* Set in oled_start() before the task exists, never changed. */
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

typedef struct {
    bool present;              /* answered the probe and was initialised */
    bool display_on_pending;   /* 0xAF after the first full clear */
    bool boot_started;
    int64_t boot_us;           /* first successful init */
    int64_t next_probe_us;
    unsigned consecutive_errors;

    oled_fb_t want;
    oled_fb_t sent;
    uint16_t valid;            /* chunks of `sent` known to be on the screen */

    bool row_seen;
    oled_row_t row;

    /* since the last summary */
    uint32_t windows, chunks, errors, deferred, late, no_row, probes_failed;
    int64_t chunk_max_us, chunk_sum_us;
    /* since boot */
    uint32_t drops, inits;
} oled_state_t;

/* static: two 1 KB frame buffers stay off the task stack */
static oled_state_t s_st;

/* ---- bus traffic ---- */

static esp_err_t send_bytes(const uint8_t *buf, size_t len, int64_t *took_us)
{
    const int64_t t = esp_timer_get_time();
    const esp_err_t err = i2c_master_transmit(s_dev, buf, len, OLED_XFER_TIMEOUT_MS);
    if (took_us != NULL) {
        *took_us = esp_timer_get_time() - t;
    }
    return err;
}

static void on_error(oled_state_t *st, esp_err_t err, const char *what, int64_t now_us)
{
    ++st->errors;
    ++st->consecutive_errors;
    if (st->consecutive_errors == 1U) {
        ESP_LOGW(TAG, "%s: %s", what, esp_err_to_name(err));
    }
    if (st->consecutive_errors >= OLED_ERRORS_TO_DROP) {
        ++st->drops;
        st->present = false;
        st->next_probe_us = now_us + OLED_PROBE_RETRY_US;
        ESP_LOGE(TAG, "screen dropped after %u errors in a row (last %s); probing every %lld s",
                 st->consecutive_errors, esp_err_to_name(err),
                 (long long)(OLED_PROBE_RETRY_US / 1000000));
    }
}

/* Probe, then the init list (display stays off until the RAM is cleared). */
static void try_attach(oled_state_t *st, int64_t now_us)
{
    st->next_probe_us = now_us + OLED_PROBE_RETRY_US;
    esp_err_t err = i2c_master_probe(s_bus, OLED_I2C_ADDR, OLED_XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        if (st->probes_failed++ == 0U && st->inits == 0U) {
            ESP_LOGW(TAG, "no answer at 0x%02X (%s) - no screen; retry every %lld s. "
                          "Sampler, CO path and logging are not affected",
                     OLED_I2C_ADDR, esp_err_to_name(err), (long long)(OLED_PROBE_RETRY_US / 1000000));
        }
        return;
    }
    uint8_t cmds[16];
    const size_t n = oled_init_cmds(OLED_CHIP, OLED_ROTATE_180, cmds, sizeof(cmds));
    if (n == 0) {
        ESP_LOGE(TAG, "init list does not fit");   /* programming error */
        return;
    }
    err = send_bytes(cmds, n, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "init: %s", esp_err_to_name(err));
        return;
    }
    ++st->inits;
    st->present = true;
    st->consecutive_errors = 0;
    st->display_on_pending = true;
    st->valid = 0;             /* RAM is random after power-on: resend all */
    if (!st->boot_started) {
        st->boot_started = true;
        st->boot_us = now_us;
    }
    ESP_LOGI(TAG, "screen at 0x%02X initialised (%s settings, column offset %u, rotate180=%d)"
                  " - controller not confirmed, see OLED.md step 3",
             OLED_I2C_ADDR, OLED_CHIP == OLED_CHIP_SH1106 ? "SH1106" : "SSD1306",
             oled_col_offset(OLED_CHIP), (int)OLED_ROTATE_180);
}

static esp_err_t send_chunk(oled_state_t *st, unsigned chunk)
{
    unsigned page;
    unsigned col;
    oled_chunk_where(chunk, &page, &col);
    uint8_t pos[4];
    if (oled_pos_cmds(OLED_CHIP, page, col, pos) != sizeof(pos)) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t t_pos = 0;
    esp_err_t err = send_bytes(pos, sizeof(pos), &t_pos);
    if (err != ESP_OK) {
        return err;
    }
    uint8_t data[1 + OLED_CHUNK_COLS];
    data[0] = OLED_CTRL_DATA;
    memcpy(&data[1], &st->want.px[page][col], OLED_CHUNK_COLS);
    int64_t t_data = 0;
    err = send_bytes(data, sizeof(data), &t_data);
    if (err != ESP_OK) {
        return err;
    }
    memcpy(&st->sent.px[page][col], &st->want.px[page][col], OLED_CHUNK_COLS);
    st->valid |= (uint16_t)(1U << chunk);
    /* the longest single bus hold is the data write; both summed here */
    const int64_t took = t_pos + t_data;
    ++st->chunks;
    st->chunk_sum_us += took;
    if (took > st->chunk_max_us) {
        st->chunk_max_us = took;
    }
    return ESP_OK;
}

/* Changed chunks, one at a time, until the window closes. */
static void send_changes(oled_state_t *st, int64_t window_end_us)
{
    int c = 0;
    while ((c = oled_fb_next_dirty(&st->want, &st->sent, st->valid, (unsigned)c)) >= 0) {
        const int64_t now = esp_timer_get_time();
        if (now + OLED_CHUNK_BUDGET_US > window_end_us) {
            ++st->deferred;    /* the rest goes in the next window */
            return;
        }
        const esp_err_t err = send_chunk(st, (unsigned)c);
        if (err != ESP_OK) {
            st->valid &= (uint16_t)~(1U << (unsigned)c);
            on_error(st, err, "chunk write", now);
            return;
        }
        st->consecutive_errors = 0;
        ++c;
    }
    if (st->display_on_pending) {
        const uint8_t on[] = { OLED_CTRL_CMD, OLED_CMD_DISPLAY_ON };
        const esp_err_t err = send_bytes(on, sizeof(on), NULL);
        if (err != ESP_OK) {
            on_error(st, err, "display on", esp_timer_get_time());
            return;
        }
        st->display_on_pending = false;
    }
}

/* ---- what to show ---- */

static oled_show_t map_shown(const io_shown_t *s, int64_t now_us, bool *demo, bool *muted,
                             uint8_t *faults)
{
    *demo = false;
    *muted = false;
    *faults = 0;
    if (now_us - s->at_us > OLED_IO_FRESH_US) {
        return OLED_SHOW_UNKNOWN;
    }
    if (s->co_active) {
        *muted = s->co_muted;
        return s->co_alarm ? OLED_SHOW_CO_ALARM : OLED_SHOW_CO_FAULT;
    }
    *demo = s->demo;
    *muted = s->fsm_muted;
    *faults = s->fault_reasons;
    switch (s->shown) {
    case AG_FSM_SHOW_NORMAL:    return OLED_SHOW_NORMAL;
    case AG_FSM_SHOW_ML_WATCH:  return OLED_SHOW_WATCH;
    case AG_FSM_SHOW_SYS_FAULT: return OLED_SHOW_FAULT;
    case AG_FSM_SHOW_ML_ALARM:  return OLED_SHOW_ML_ALARM;
    case AG_FSM_SHOW_CO:        /* FSM stands aside but co_alert is not active:
                                   a pass in between, show nothing rather than guess */
    default:                    return OLED_SHOW_UNKNOWN;
    }
}

static void fill_co(oled_ui_in_t *in, int64_t now_us)
{
#if ZE07_ENABLE
    co_status_t cs;
    if (!co_peek_status(&cs)) {
        in->co = OLED_CO_NONE;
        return;
    }
    uint16_t x10 = 0;
    if (cs.fault || co_status_stale(&cs, now_us)) {
        in->co = OLED_CO_FAULT;
    } else if (cs.warmup) {
        in->co = OLED_CO_WARMUP;
    } else if (co_status_csv_value(&cs, now_us, OLED_CO_MAX_AGE_US, &x10)) {
        in->co = OLED_CO_VALUE;
        in->co_x10 = x10;
        in->co_mean1h_x10 = cs.mean_1h_x10;
    } else {
        in->co = OLED_CO_NO_VALUE;
    }
#else
    (void)now_us;
    in->co = OLED_CO_OFF;
#endif
}

static void fill_row(const oled_state_t *st, oled_ui_in_t *in, int64_t now_us)
{
    in->row_seen = st->row_seen;
    in->row_fresh = st->row_seen && now_us - st->row.t0_us <= OLED_ROW_FRESH_US;
    if (!in->row_fresh) {
        return;
    }
    const csv_sample_t *r = &st->row.row;
    in->has_pm = r->has_pm;
    in->pm2_5 = r->pm2_5;
    /* same rounding as the CSV, so the screen shows the row's digits */
    in->has_th = r->has_th && csv_round_tenths(r->temp_c, &in->temp_x10) &&
                 csv_round_tenths(r->humidity_pct, &in->rh_x10);
    in->has_voc = r->has_voc;
    in->tvoc_ppb = r->tvoc_ppb;
    in->eco2_ppm = r->eco2_ppm;
    in->time_valid = st->row.time_valid;
    if (in->time_valid) {
        in->hh = (uint8_t)r->local_time.tm_hour;
        in->mm = (uint8_t)r->local_time.tm_min;
        in->ss = (uint8_t)r->local_time.tm_sec;
        in->marked = r->marked;
    }
}

static void fill_ml(oled_ui_in_t *in, int64_t now_us)
{
    ag_ml_result_t r;
    if (!ml_mailbox_peek(&r)) {
        in->ml = OLED_ML_NONE;
        return;
    }
    if (now_us - r.at_us > OLED_ML_FRESH_US) {
        in->ml = OLED_ML_STALE;
        return;
    }
    switch (r.status) {
    case AG_ML_MODEL_FAULT: in->ml = OLED_ML_MODEL_FAULT; break;
    case AG_ML_MISSING:     in->ml = OLED_ML_MISSING; break;
    case AG_ML_BELOW:       in->ml = OLED_ML_BELOW; break;
    case AG_ML_ABOVE:       in->ml = OLED_ML_ABOVE; break;
    case AG_ML_PERSISTENT:  in->ml = OLED_ML_PERSISTENT; break;
    default:                in->ml = OLED_ML_NONE; break;
    }
    /* < 1e7 keeps D² x 100 inside a 32-bit long; larger shows as >9999 */
    if (isfinite(r.d2) && r.d2 >= 0.0f) {
        in->d2_valid = true;
        in->d2_x100 = r.d2 < 1.0e7f ? lroundf(r.d2 * 100.0f) : 1000000000L;
    }
}

static void compose(oled_state_t *st, int64_t now_us)
{
    const int64_t since_boot = now_us - st->boot_us;
    if (since_boot < OLED_PATTERN_US) {
        oled_fb_test_pattern(&st->want);
        return;
    }
    if (since_boot < OLED_PATTERN_US + OLED_BOOT_TEXT_US) {
        oled_ui_lines_t boot;
        oled_ui_boot_lines(ag_mahal_model_version(), &boot);
        oled_ui_draw_lines(&boot, &st->want);
        return;
    }
    oled_ui_in_t in;
    memset(&in, 0, sizeof(in));
    io_shown_t shown;
    if (io_peek_shown(&shown)) {
        in.show = map_shown(&shown, now_us, &in.demo, &in.muted, &in.fault_bits);
    } else {
        in.show = OLED_SHOW_UNKNOWN;
    }
    fill_co(&in, now_us);
    fill_row(st, &in, now_us);
    fill_ml(&in, now_us);
    oled_ui_text_t text;
    oled_ui_format(&in, &text);
    oled_ui_draw(&text, &st->want);
}

static void summary(oled_state_t *st, int64_t now_us, int64_t *next_us)
{
    if (now_us < *next_us) {
        return;
    }
    *next_us = now_us + OLED_SUMMARY_US;
    ESP_LOGI(TAG, "60 s: present=%d windows=%" PRIu32 " chunks=%" PRIu32
                  " chunk us avg=%" PRId64 " max=%" PRId64 " | err=%" PRIu32 " deferred=%" PRIu32
                  " late=%" PRIu32 " no_row=%" PRIu32 " | boot: inits=%" PRIu32 " drops=%" PRIu32
                  " | stack HWM=%u B",
             (int)st->present, st->windows, st->chunks,
             st->chunks ? st->chunk_sum_us / (int64_t)st->chunks : 0, st->chunk_max_us,
             st->errors, st->deferred, st->late, st->no_row, st->inits, st->drops,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    st->windows = st->chunks = st->errors = st->deferred = st->late = st->no_row = 0;
    st->chunk_max_us = st->chunk_sum_us = 0;
}

static void oled_task(void *arg)
{
    (void)arg;
    oled_state_t *st = &s_st;
    int64_t next_summary_us = esp_timer_get_time() + OLED_SUMMARY_US;
    for (;;) {
        oled_row_t r;
        const bool got = xQueueReceive(s_row_q, &r, pdMS_TO_TICKS(OLED_NO_ROW_WAIT_MS)) == pdTRUE;
        const int64_t now_us = esp_timer_get_time();
        int64_t window_end_us;
        if (got) {
            st->row = r;
            st->row_seen = true;
            window_end_us = r.t0_us + OLED_WINDOW_END_US;
            if (now_us - r.t0_us > OLED_LATE_US) {
                ++st->late;
            }
        } else {
            ++st->no_row;
            window_end_us = now_us + OLED_NO_ROW_WINDOW_US;
        }
        ++st->windows;

        if (!st->present && now_us >= st->next_probe_us) {
            try_attach(st, now_us);
        }
        if (st->present) {
            compose(st, now_us);
            send_changes(st, window_end_us);
        }
        summary(st, now_us, &next_summary_us);
    }
}

#endif /* OLED_ENABLE */

esp_err_t oled_start(i2c_master_bus_handle_t bus)
{
#if !OLED_ENABLE
    (void)bus;
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (bus == NULL || s_row_q == NULL || s_dev != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_SCL_HZ,
    };
    /* takes the bus lock briefly (list update only, no bus traffic) */
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &s_dev);
    if (err != ESP_OK) {
        s_dev = NULL;
        return err;
    }
    s_bus = bus;
    const BaseType_t ok = xTaskCreatePinnedToCore(oled_task, "oled", OLED_STACK_BYTES, NULL,
                                                  OLED_TASK_PRIORITY, NULL, OLED_TASK_CORE);
    if (ok != pdPASS) {
        err = i2c_master_bus_rm_device(s_dev);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "remove device after failed task create: %s", esp_err_to_name(err));
        }
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
#endif
}
