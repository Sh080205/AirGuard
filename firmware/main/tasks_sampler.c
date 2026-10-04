#include "tasks_sampler.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "aht21.h"
#include "board_pins.h"
#include "csv_row.h"
#include "logutil.h"
#include "pms7003.h"
#include "task_config.h"
#include "tasks_io.h"
#include "tasks_storage.h"
#include "time_sync.h"

/* CLAUDE.md §7: one row per second. */
#define PERIOD_MS 1000U
#define PERIOD_US 1000000LL

/* Row timestamps are the system clock rounded to the nearest second, so the
 * loop is phase-locked to wake just after each second boundary: rounding is
 * then far from the ambiguous .5 point and jitter of a few ms cannot produce
 * a duplicate or skipped second. When the phase drifts more than
 * ALIGN_TOLERANCE_US from the boundary (first sync, SNTP step, settime) the
 * loop re-aligns once and logs it.
 * ALIGN_AFTER_BOUNDARY_US: wake 5 ms after the boundary so tick rounding
 * (1 ms, CONFIG_FREERTOS_HZ=1000) never lands just before it.
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด (jitter ที่ GPIO4) */
#define ALIGN_TOLERANCE_US      200000LL
#define ALIGN_AFTER_BOUNDARY_US   5000LL

/* FW-4 §1: warn every 10 s while there is no real time. */
#define NO_TIME_WARN_US (10LL * 1000 * 1000)
/* FW-4 §7: summary every 60 s, not every row. */
#define SUMMARY_US      (60LL * 1000 * 1000)

/* AHT21 datasheet §7.4 step 1 / CLAUDE.md §5: wait >= 100 ms after power-on
 * before the first status read; then 10 ms before the first trigger. */
#define AHT21_POWER_ON_WAIT_US  (100LL * 1000)
#define AHT21_AFTER_STATUS_MS   10U
/* I2C clock for the bus: 100 kHz (standard mode). Datasheet §4.4 allows
 * 10-400 kHz; we move ~10 bytes/s so speed buys nothing, while the slower
 * edge rate tolerates the 4.7 kΩ pull-ups (CLAUDE.md §4) plus JST cables
 * and up to four devices on the bus (§5 addresses). */
#define I2C_SCL_HZ 100000U
/* i2c_master_bus_config_t doc in v5.5.5: "typically value is 7"
 * [ขึ้นกับเวอร์ชัน ESP-IDF] */
#define I2C_GLITCH_IGNORE_CNT 7
/* FW-4 §8: print the first 5 raw frames + CRC result to confirm the CRC
 * assumption (6 bytes incl. state, MSB-first, no final XOR) on real data. */
#define AHT21_HEX_LOG_FRAMES 5

/* PMS7003 manual V2.5, Circuit Attentions item 4 (p. 10): data is stable
 * >= 30 s after the sensor wakes. Rows in that window get empty PM. */
#define PMS_WARMUP_US (30LL * 1000 * 1000)
/* Upper bound for one drain: the driver RX ring holds 16 frames
 * (pms7003_uart.c); 32 can never be reached unless the driver misbehaves. */
#define PMS_DRAIN_MAX 32
/* PMS_MAX_AGE_MS / PMS_MAX_AGE_US (2500 ms) live in logutil.h so the host
 * test uses the same value; the reasoning is written there. Without holding,
 * ~57 % of 1 Hz rows would have no PM (sensor sends every ~2.3 s in stable
 * air, 200-800 ms when concentration changes fast; manual V2.5 p. 4
 * [ต้องเปิด datasheet ยืนยัน]). */
_Static_assert(PMS_MAX_AGE_MS > 2 * PERIOD_MS && PMS_MAX_AGE_MS < 3 * PERIOD_MS,
               "PMS_MAX_AGE_MS must reuse a frame in exactly 2 later cycles");

/* Where the PM columns of the current row came from (summary counters). */
typedef enum {
    PM_SOURCE_EMPTY,    /* no frame, frame older than PMS_MAX_AGE_MS, or warm-up */
    PM_SOURCE_NEW,      /* frame drained in this cycle */
    PM_SOURCE_REUSED,   /* earlier frame, still <= PMS_MAX_AGE_MS old */
} pm_source_t;

/* First few errors of each kind are logged; the rest only in the summary. */
#define ERRORS_LOGGED 3U

static const char *TAG = "sampler";

/* Owned by the sampler task only (after sampler_start() returns). */
typedef struct {
    sampler_config_t cfg;
    uint32_t cycle;

    /* AHT21: trigger in one cycle, read in the next (>= 80 ms later, §7.4
     * step 3) → no blocking wait inside the 1 Hz loop. One measurement per
     * 2 cycles = every 2 s, satisfying "> 1 s per measurement" (§4.4). */
    bool aht_pending;
    bool th_valid;
    float temp_c;
    float rh_pct;
    uint32_t th_cycle;
    uint32_t aht_frames_logged;
    uint32_t aht_trigger_err, aht_read_err, aht_crc_err, aht_busy;

    int64_t pms_ready_us;
    uint32_t pms_frames, pms_err;
    /* newest frame received after warm-up, held for PMS_MAX_AGE_MS */
    bool pms_have_frame;
    pms7003_data_t pms_frame;
    int64_t pms_frame_us;
    pm_source_t pm_source;      /* of the current cycle's row */

    bool has_prev_row;
    int64_t prev_row_sec;
    uint32_t rows_queued, rows_dropped;
    uint32_t dup_rows, skip_events, backward_steps, realigns, overruns;
    int64_t skipped_seconds;
    unsigned queue_max;
    /* queued rows by PM source; new + reused + empty == rows_queued */
    uint32_t pm_new, pm_reused, pm_missing;
    uint32_t th_missing;

    /* D2: MARK state. Lives here (not in io) because only the sampler knows
     * which row a press applies to. */
    bool marked;
    bool mark_has_rows;
    struct tm mark_start;
    struct tm mark_last;
    uint32_t marks_ignored, events_lost;

    bool indication_known;
    io_indication_t indication;

    logutil_stats_t period_us;   /* window since the last summary */
    int64_t jitter_boot_max_us;
    int64_t prev_cycle_us;
    bool skip_period_sample;
    int64_t next_summary_us;
    int64_t next_no_time_warn_us;
} sampler_state_t;

static sampler_state_t s_st;

static void format_iso(const struct tm *t, char *buf, size_t cap)
{
    snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d+07:00",
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);
}

static void aht21_boot(void)
{
    const int64_t since_boot = esp_timer_get_time();
    /* esp_timer starts after power-on, so this waits at least 100 ms
     * since the sensor got power */
    if (since_boot < AHT21_POWER_ON_WAIT_US) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((AHT21_POWER_ON_WAIT_US - since_boot) / 1000) + 1U));
    }
    uint8_t status = 0;
    const esp_err_t err = aht21_read_status(&status);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AHT21 status read failed: %s; T/RH stay empty until it answers",
                 esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "AHT21 status 0x%02X", status);
    if (aht21_needs_init(status)) {
        /* TODO [ต้องดู routine ทางการ]: datasheet §7.4 step 1 says registers
         * 0x1B, 0x1C, 0x1E must be initialised when (status & 0x18) != 0x18,
         * but the values/sequence are NOT in the datasheet (CLAUDE.md §5).
         * Do not guess them. Until the official ASAIR routine is obtained we
         * only warn and keep measuring; CRC still guards every frame. */
        ESP_LOGW(TAG, "AHT21 needs register init ((status & 0x18) != 0x18); "
                      "init routine not implemented (TODO official routine) - readings may be invalid");
    }
    vTaskDelay(pdMS_TO_TICKS(AHT21_AFTER_STATUS_MS));
}

static void log_aht_frame(sampler_state_t *st, const uint8_t raw[AHT21_FRAME_LEN],
                          esp_err_t parse_err, float t, float rh)
{
    if (st->aht_frames_logged >= AHT21_HEX_LOG_FRAMES) {
        return;
    }
    ++st->aht_frames_logged;
    const uint8_t crc = aht21_crc8(raw, AHT21_FRAME_LEN - 1);
    ESP_LOGI(TAG, "AHT21 frame %" PRIu32 "/%d: %02X %02X %02X %02X %02X %02X | %02X "
             "crc_calc=%02X %s -> %s",
             st->aht_frames_logged, AHT21_HEX_LOG_FRAMES,
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], crc,
             crc == raw[6] ? "CRC OK" : "CRC MISMATCH", esp_err_to_name(parse_err));
    if (parse_err == ESP_OK) {
        /* float formatting only in these first 5 log lines (not in the row) */
        ESP_LOGI(TAG, "AHT21 frame %" PRIu32 ": T=%.2f C RH=%.2f %%",
                 st->aht_frames_logged, (double)t, (double)rh);
    }
}

static void aht21_step(sampler_state_t *st, csv_sample_t *row)
{
    if (!st->aht_pending) {
        const esp_err_t err = aht21_trigger();
        if (err == ESP_OK) {
            st->aht_pending = true;
        } else if (++st->aht_trigger_err <= ERRORS_LOGGED) {
            ESP_LOGW(TAG, "AHT21 trigger: %s", esp_err_to_name(err));
        }
    } else {
        st->aht_pending = false;
        uint8_t raw[AHT21_FRAME_LEN];
        const esp_err_t err = aht21_read_frame(raw);
        if (err != ESP_OK) {
            st->th_valid = false;
            if (++st->aht_read_err <= ERRORS_LOGGED) {
                ESP_LOGW(TAG, "AHT21 read: %s", esp_err_to_name(err));
            }
        } else {
            float t = 0.0f;
            float rh = 0.0f;
            const esp_err_t perr = aht21_parse(raw, &t, &rh);
            log_aht_frame(st, raw, perr, t, rh);
            if (perr == ESP_OK) {
                st->th_valid = true;
                st->temp_c = t;
                st->rh_pct = rh;
                st->th_cycle = st->cycle;
            } else {
                /* a failed read must not leave an older value in use */
                st->th_valid = false;
                if (perr == ESP_ERR_INVALID_CRC) {
                    ++st->aht_crc_err;
                } else {
                    ++st->aht_busy;
                }
            }
        }
    }
    /* value read in cycle k is used for rows k and k+1 (measured every 2 s,
     * CLAUDE.md §7 note), never older: k+2 has a new read or stays empty */
    row->has_th = st->th_valid && (uint32_t)(st->cycle - st->th_cycle) <= 1U;
    if (row->has_th) {
        row->temp_c = st->temp_c;
        row->humidity_pct = st->rh_pct;
    }
}

static void pms_step(sampler_state_t *st, csv_sample_t *row)
{
    /* pms7003_read returns the OLDEST buffered frame (pms7003.h): drain with
     * timeout 0 and keep the last one = newest frame of this cycle */
    pms7003_data_t d;
    bool got_new = false;
    for (int i = 0; i < PMS_DRAIN_MAX; ++i) {
        const esp_err_t err = pms7003_read(&d, 0);
        if (err == ESP_OK) {
            ++st->pms_frames;
            const int64_t received_us = esp_timer_get_time();
            /* frames from the 30 s warm-up are drained but never used */
            if (received_us >= st->pms_ready_us) {
                st->pms_frame = d;
                st->pms_frame_us = received_us;
                st->pms_have_frame = true;
                got_new = true;
            }
            continue;
        }
        if (err != ESP_ERR_TIMEOUT && ++st->pms_err <= ERRORS_LOGGED) {
            ESP_LOGW(TAG, "PMS7003 read: %s", esp_err_to_name(err));
        }
        break;
    }
    /* FW-4 §4 (4 Oct): newest frame if <= PMS_MAX_AGE_MS old, else empty.
     * D3: atmospheric set. */
    row->has_pm = logutil_frame_fresh(st->pms_have_frame, st->pms_frame_us,
                                      esp_timer_get_time(), PMS_MAX_AGE_US);
    if (row->has_pm) {
        row->pm1_0 = st->pms_frame.pm1_0_atmospheric;
        row->pm2_5 = st->pms_frame.pm2_5_atmospheric;
        row->pm10 = st->pms_frame.pm10_atmospheric;
        st->pm_source = got_new ? PM_SOURCE_NEW : PM_SOURCE_REUSED;
    } else {
        st->pm_source = PM_SOURCE_EMPTY;
    }
}

static void handle_marks(sampler_state_t *st, bool time_valid)
{
    io_mark_event_t ev;
    while (st->cfg.mark_queue != NULL &&
           xQueueReceive(st->cfg.mark_queue, &ev, 0) == pdTRUE) {
        if (!time_valid) {
            ++st->marks_ignored;
            ESP_LOGW(TAG, "MARK ignored: no real time yet, nothing is being recorded");
            continue;
        }
        if (!st->marked) {
            st->marked = true;
            st->mark_has_rows = false;
            ESP_LOGI(TAG, "MARK: event started (label=marked from this row)");
            continue;
        }
        st->marked = false;
        if (!st->mark_has_rows) {
            ESP_LOGW(TAG, "MARK: event closed before any marked row; not recorded");
            continue;
        }
        storage_event_t sev = { .start = st->mark_start, .end = st->mark_last };
        char line[96];
        if (csv_event_row(&sev.start, &sev.end, line, sizeof(line)) != 0) {
            line[strcspn(line, "\n")] = '\0';
            /* console copy = backup ground truth if the SD write fails */
            ESP_LOGI(TAG, "MARK: event closed, EV row: %s", line);
        }
        if (!storage_submit_event(&sev)) {
            ++st->events_lost;
            ESP_LOGE(TAG, "MARK: event queue full - EV row NOT saved to SD, copy it from this log");
        }
    }
}

static void update_indication(sampler_state_t *st, bool time_valid)
{
    const io_indication_t want = !time_valid ? IO_IND_WAIT_TIME :
                                 st->marked ? IO_IND_MARKED : IO_IND_RUNNING;
    if (st->indication_known && st->indication == want) {
        return;
    }
    const esp_err_t err = io_set_indication(want);
    if (err == ESP_OK) {
        st->indication = want;
        st->indication_known = true;
    } else {
        /* io queue full: try again next cycle */
        ESP_LOGW(TAG, "io_set_indication: %s", esp_err_to_name(err));
    }
}

/* Returns true when the wake phase drifted and the loop must re-align. */
static bool record_row(sampler_state_t *st, const struct timeval *tv, csv_sample_t *row)
{
    const int64_t sec = logutil_round_to_second((int64_t)tv->tv_sec, (int32_t)tv->tv_usec);
    int64_t skipped = 0;
    const logutil_gap_t gap = logutil_classify_gap(st->has_prev_row, st->prev_row_sec,
                                                   sec, &skipped);
    const time_t t = (time_t)sec;
    if (localtime_r(&t, &row->local_time) == NULL) {
        ESP_LOGE(TAG, "localtime_r failed; row dropped");
        ++st->rows_dropped;
        return false;
    }
    char iso[32];
    switch (gap) {
    case LOGUTIL_GAP_DUPLICATE:
        /* keep timestamps unique (FW-4 board test 3): drop the second row */
        ++st->dup_rows;
        format_iso(&row->local_time, iso, sizeof(iso));
        ESP_LOGW(TAG, "duplicate second %s; row dropped", iso);
        return true;
    case LOGUTIL_GAP_SKIP:
        ++st->skip_events;
        st->skipped_seconds += skipped;
        format_iso(&row->local_time, iso, sizeof(iso));
        ESP_LOGW(TAG, "%" PRId64 " second(s) skipped before %s", skipped, iso);
        break;
    case LOGUTIL_GAP_BACKWARD:
        /* clock stepped back (SNTP correction / settime): keep writing,
         * dropping rows could lose hours after a large step */
        ++st->backward_steps;
        format_iso(&row->local_time, iso, sizeof(iso));
        ESP_LOGW(TAG, "clock went back %" PRId64 " s to %s", st->prev_row_sec - sec, iso);
        break;
    case LOGUTIL_GAP_FIRST:
    case LOGUTIL_GAP_OK:
    default:
        break;
    }
    st->has_prev_row = true;
    st->prev_row_sec = sec;

    row->marked = st->marked;
    if (st->marked) {
        if (!st->mark_has_rows) {
            st->mark_start = row->local_time;
            st->mark_has_rows = true;
        }
        st->mark_last = row->local_time;
    }

    if (storage_submit_row(row)) {
        ++st->rows_queued;
        /* counted only for rows that entered the queue, so
         * pm_new + pm_reused + pm_missing == rows_queued exactly; rows that
         * never reach the queue (localtime_r failure, duplicate second, full
         * queue) are in rows_dropped / dup_rows instead */
        switch (st->pm_source) {
        case PM_SOURCE_NEW: ++st->pm_new; break;
        case PM_SOURCE_REUSED: ++st->pm_reused; break;
        case PM_SOURCE_EMPTY:
        default: ++st->pm_missing; break;
        }
        if (!row->has_th) {
            ++st->th_missing;
        }
        const unsigned waiting = storage_rows_waiting();
        if (waiting > st->queue_max) {
            st->queue_max = waiting;
        }
    } else if (++st->rows_dropped <= ERRORS_LOGGED) {
        /* never block: the sampler must keep its period (FW-4 §7) */
        ESP_LOGW(TAG, "row queue full; row dropped (total %" PRIu32 ")", st->rows_dropped);
    }

    const int64_t frac = tv->tv_usec;
    return frac > ALIGN_TOLERANCE_US && frac < PERIOD_US - ALIGN_TOLERANCE_US;
}

static void summary(sampler_state_t *st, int64_t now_us)
{
    if (now_us < st->next_summary_us) {
        return;
    }
    st->next_summary_us = now_us + SUMMARY_US;
    const int64_t jitter = logutil_stats_jitter(&st->period_us, PERIOD_US);
    /* "last 60s" = reset after every summary (period stats only);
     * "since boot" = cumulative counters / minimum marks, never reset;
     * "now" = current state */
    ESP_LOGI(TAG, "last 60s: period min=%" PRId64 " max=%" PRId64 " jitter=%" PRId64
             " us (n=%" PRIu32 ") | since boot: max jitter=%" PRId64 " us realign=%" PRIu32
             " overrun=%" PRIu32,
             st->period_us.min, st->period_us.max, jitter, st->period_us.count,
             st->jitter_boot_max_us, st->realigns, st->overruns);
    ESP_LOGI(TAG, "since boot: rows queued=%" PRIu32 " dropped=%" PRIu32
             " dup=%" PRIu32 " skipped_sec=%" PRId64 " (events %" PRIu32 ") back=%" PRIu32
             " | queue max %u/%u | ignored_marks=%" PRIu32 " events_lost=%" PRIu32
             " | now: time=%s marked=%d",
             st->rows_queued, st->rows_dropped,
             st->dup_rows, st->skipped_seconds, st->skip_events, st->backward_steps,
             st->queue_max, (unsigned)STORAGE_ROW_QUEUE_LEN,
             st->marks_ignored, st->events_lost,
             time_sync_is_valid() ? "valid" : "NOT SET", st->marked);
    ESP_LOGI(TAG, "since boot: rows PM new=%" PRIu32 " reused(<=%d ms)=%" PRIu32 " empty=%" PRIu32
             " | rows w/o T/RH=%" PRIu32 " | pms frames=%" PRIu32 " err=%" PRIu32
             " | aht trig_err=%" PRIu32 " read_err=%" PRIu32 " crc_err=%" PRIu32 " busy=%" PRIu32,
             st->pm_new, PMS_MAX_AGE_MS, st->pm_reused, st->pm_missing,
             st->th_missing, st->pms_frames, st->pms_err,
             st->aht_trigger_err, st->aht_read_err, st->aht_crc_err, st->aht_busy);
    ESP_LOGI(TAG, "since boot: stack HWM sampler=%u storage=%u console=%u B | heap free now=%u min=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             st->cfg.storage_task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(st->cfg.storage_task) : 0U,
             st->cfg.console_task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(st->cfg.console_task) : 0U,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    logutil_stats_reset(&st->period_us);
}

static void sampler_task(void *arg)
{
    (void)arg;
    sampler_state_t *st = &s_st;
    aht21_boot();

    st->next_summary_us = esp_timer_get_time() + SUMMARY_US;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        /* GPIO4 HIGH = cycle start (rising edges give the real period) */
        const esp_err_t dbg_err = gpio_set_level(BOARD_PIN_DBG_SAMPLER, 1);
        const int64_t t0 = esp_timer_get_time();
        ++st->cycle;
        if (st->prev_cycle_us != 0 && !st->skip_period_sample) {
            logutil_stats_add(&st->period_us, t0 - st->prev_cycle_us);
            const int64_t dev = t0 - st->prev_cycle_us - PERIOD_US;
            const int64_t adev = dev < 0 ? -dev : dev;
            if (adev > st->jitter_boot_max_us) {
                st->jitter_boot_max_us = adev;
            }
        }
        st->skip_period_sample = false;
        st->prev_cycle_us = t0;

        struct timeval tv = { 0 };
        /* a clock read failure is treated like "no real time" for this row */
        const bool time_valid = gettimeofday(&tv, NULL) == 0 && time_sync_is_valid();

        handle_marks(st, time_valid);

        csv_sample_t row;
        memset(&row, 0, sizeof(row));
        pms_step(st, &row);
        aht21_step(st, &row);
        /* tvoc/eco2/co stay empty until FW-5 (has_voc/has_co = false) */

        bool realign = false;
        if (time_valid) {
            realign = record_row(st, &tv, &row);
        } else if (t0 >= st->next_no_time_warn_us) {
            st->next_no_time_warn_us = t0 + NO_TIME_WARN_US;
            ESP_LOGW(TAG, "no real time yet - CSV not written. Wait for SNTP or type: "
                          "settime 2026-10-06T09:00:00+07:00");
        }
        update_indication(st, time_valid);
        summary(st, t0);

        if (dbg_err != ESP_OK || gpio_set_level(BOARD_PIN_DBG_SAMPLER, 0) != ESP_OK) {
            ESP_LOGW(TAG, "debug pin GPIO%d write failed", BOARD_PIN_DBG_SAMPLER);
        }

        if (realign) {
            /* next wake = boundary of (last row second + 1) + 5 ms, so the
             * next timestamp is exactly prev + 1 whatever the old phase */
            struct timeval now;
            if (gettimeofday(&now, NULL) != 0) {
                now = tv;   /* fall back to the cycle-start reading */
            }
            const int64_t now_us = (int64_t)now.tv_sec * PERIOD_US + now.tv_usec;
            const int64_t target_us = (st->prev_row_sec + 1) * PERIOD_US + ALIGN_AFTER_BOUNDARY_US;
            int64_t wait_us = target_us - now_us;
            if (wait_us < 0) {
                wait_us = 0;
            } else if (wait_us > 2 * PERIOD_US) {
                wait_us = 2 * PERIOD_US;
            }
            ++st->realigns;
            ESP_LOGI(TAG, "re-aligning to the second boundary: phase was %06ld us, waiting %" PRId64 " us",
                     (long)tv.tv_usec, wait_us);
            vTaskDelay(pdMS_TO_TICKS((uint32_t)((wait_us + 999) / 1000)));
            last_wake = xTaskGetTickCount();
            st->skip_period_sample = true;   /* this period is not a 1 Hz sample */
        } else if (xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(PERIOD_MS)) == pdFALSE) {
            /* work took longer than the period: the cycle starts late */
            ++st->overruns;
        }
    }
}

esp_err_t sampler_start(const sampler_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_st.cfg = *config;

    esp_err_t err = gpio_set_level(BOARD_PIN_DBG_SAMPLER, 0);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t dbg = {
        .pin_bit_mask = 1ULL << BOARD_PIN_DBG_SAMPLER,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&dbg);
    if (err != ESP_OK) {
        return err;
    }

    /* [ขึ้นกับเวอร์ชัน ESP-IDF] new i2c_master driver (v5.2+) */
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_PIN_SDA,
        .scl_io_num = BOARD_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
        .intr_priority = 0,
        .trans_queue_depth = 0,     /* synchronous transfers only */
        /* board has 4.7 kΩ pull-ups on SDA/SCL (CLAUDE.md §4); the internal
         * ~45 kΩ would add little and must not be relied on */
        .flags.enable_internal_pullup = false,
    };
    i2c_master_bus_handle_t bus = NULL;
    err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        return err;
    }
    err = aht21_attach(bus, I2C_SCL_HZ);
    if (err != ESP_OK) {
        return err;
    }
    err = pms7003_init();
    if (err != ESP_OK) {
        return err;
    }
    s_st.pms_ready_us = esp_timer_get_time() + PMS_WARMUP_US;

    const BaseType_t ok = xTaskCreatePinnedToCore(sampler_task, "sampler", SAMPLER_STACK_BYTES,
                                                  NULL, SAMPLER_TASK_PRIORITY, NULL,
                                                  SAMPLER_TASK_CORE);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
