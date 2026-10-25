#include "tasks_sampler.h"

#include <inttypes.h>
#include <math.h>
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

#include "ag_mahal.h"
#include "aht21.h"
#include "board_pins.h"
#include "csv_row.h"
#include "ens160.h"
#include "logutil.h"
#include "ml_mailbox.h"
#include "pms7003.h"
#include "task_config.h"
#include "tasks_co.h"
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

/* FW-5a: ENS160 (ScioSense datasheet v1.3, "DS"). Read in the sampler
 * itself: per cycle one 6-byte burst + one MISR byte (+ a 4-byte
 * compensation write every 2 s) ≈ 1-2 ms at 100 kHz and no waiting, so a
 * separate task + queue would add stack and latency without taking any
 * blocking work out of the 1 Hz loop. Measured every 60 s ("ens160 i2c per
 * cycle") — ค่าคาดหมาย ต้องยืนยันด้วยการวัด.
 * -DENS160_ENABLE=0 puts the sensor in DEEP SLEEP (DS Table 3: 0.01 mA,
 * "not a gas sensing mode" vs 29 mA in STANDARD, so the heater is off)
 * and leaves tvoc/eco2 empty: the "ENS160 off" half of the heater test
 * (FW-5a step 3). */
#ifndef ENS160_ENABLE
#define ENS160_ENABLE 1
#endif
/* Retry start-up after a failure / sensor reset at most every 10 s so a
 * missing sensor costs one short NACKed transfer per 10 s, not per cycle.
 * Firmware policy, not a datasheet value. */
#define ENS160_RETRY_US (10LL * 1000 * 1000)
/* Status changes (warm-up -> normal ...) are logged with uptime so the
 * warm-up length can be read from the log; capped in case it flaps. */
#define ENS160_TRANSITIONS_LOGGED 16U

/* FW-5b: co_ppm comes from the CO task's snapshot (tasks_co.h). The
 * sampler only peeks the mailbox (never blocks) and the CO task never waits
 * for the sampler, so the CO decision does not depend on this loop. Same
 * hold rule as PM and ENS160 (owner approved 15 Oct): the newest frame may
 * fill rows while <= 2500 ms old, counted from when the CO task read it
 * (<= ~10 ms after the last UART byte, ค่าคาดหมาย). Empty during the CO
 * warm-up and FAULT (co_status_csv_value). */
#define CO_MAX_AGE_US PMS_MAX_AGE_US

/* FW-7 step 3: Mahalanobis score of every cycle's row (components/mahal ·
 * design approved 18 Oct, docs/tasks/reports/FW-7.md D1-D5). Scored here,
 * not in a task of its own: per row the pm_th set is one log1pf + a 3x3
 * quadratic form — no I/O, no wait, no malloc, constant time (ค่าคาดหมาย
 * a few µs, reported every 60 s as "ml compute"). Every cycle, also before
 * real time exists: the model needs no timestamp. The result goes to
 * ml_mailbox (FW-6 reads it; xQueueOverwrite never blocks) and with the row
 * to the storage task, which prints the AGML line; nothing here prints per
 * row. 🔴 The CO path neither reads nor waits for any of this (§2): co_step
 * runs before ml_step and does not use its result. */
/* Golden vectors printed one by one when the boot self-test fails. */
#define ML_SELF_TEST_FAILS_LOGGED 3U

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

    /* ENS160 (FW-5a) */
    bool ens_ready;             /* start-up sequence done */
    int64_t ens_next_try_us;
    int ens_last_verdict;       /* -1 = no reading yet */
    uint32_t ens_transitions_logged;
    uint32_t ens_init_err, ens_read_err, ens_misr_err, ens_restarts;
    uint32_t ens_notrun_wait;   /* NOT_RUNNING with OPMODE still STANDARD (P7) */
    uint32_t ens_wait_streak;   /* consecutive WAITs, see ENS160_WAIT_MAX_CYCLES */
    uint32_t ens_wait_cap;      /* restarts forced by that cap */
    uint32_t ens_comp_writes, ens_comp_err, ens_comp_skipped;
    uint32_t ens_verdicts[ENS160_VERDICT_RANGE + 1];   /* reads by verdict */
    uint32_t ens_newdat, ens_no_newdat;
    int64_t ens_last_newdat_us;
    ens160_hold_t ens_hold;
    ens160_fill_t voc_fill;     /* of the current cycle's row */
    /* queued rows by VOC source; new + reused + empty == rows_queued */
    uint32_t voc_new, voc_reused, voc_missing;
    logutil_stats_t ens_newdat_gap_us;  /* last 60 s */
    logutil_stats_t ens_xfer_us;        /* last 60 s: ENS160 time per cycle */
    logutil_stats_t work_us;            /* last 60 s: whole cycle work time */

    bool has_prev_row;
    int64_t prev_row_sec;
    uint32_t rows_queued, rows_dropped;
    uint32_t dup_rows, skip_events, backward_steps, realigns, overruns;
    int64_t skipped_seconds;
    unsigned queue_max;
    /* queued rows by PM source; new + reused + empty == rows_queued */
    uint32_t pm_new, pm_reused, pm_missing;
    uint32_t th_missing;
    /* queued rows with / without co_ppm; co_filled + co_missing == rows_queued */
    uint32_t co_filled, co_missing;

    /* FW-7 (Mahalanobis) */
    bool ml_ok;                         /* boot self-test passed */
    ag_mahal_persist_t ml_persist;
    ag_ml_result_t ml_last;             /* of the current cycle's row */
    uint32_t ml_cycles[AG_ML_PERSISTENT + 1];   /* scored cycles by status, since boot */
    uint32_t ml_episodes;               /* entries into PERSISTENT, since boot */
    uint32_t ml_gaps;                   /* > AG_MAHAL_GAP_RESET_US between cycles */
    float ml_d2_max;                    /* last 60 s, NAN = none */
    logutil_stats_t ml_us;              /* last 60 s: scoring time per cycle */

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

static const char *ens160_verdict_name(int v)
{
    switch (v) {
    case ENS160_VERDICT_OK: return "normal";
    case ENS160_VERDICT_ERROR: return "STATER";
    case ENS160_VERDICT_NOT_RUNNING: return "not running";
    case ENS160_VERDICT_WARMUP: return "warm-up";
    case ENS160_VERDICT_STARTUP: return "initial start-up";
    case ENS160_VERDICT_INVALID: return "invalid output";
    case ENS160_VERDICT_RANGE: return "out of range";
    default: return "none";
    }
}

/* PART_ID check, then OPMODE IDLE -> STANDARD (DS Figure 13: OPERATIONAL
 * and DEEP SLEEP are both left through IDLE; after power-on the sensor is
 * already in IDLE, Table 18 default 0x01). No waits: none are given in the
 * DS, so each OPMODE write is verified by reading it back (ens160.h).
 * Writing STANDARD restarts the mode (Table 18); that this starts a new
 * warm-up is expected, not stated: read it from the status log. */
static bool ens160_start_sensor(sampler_state_t *st)
{
    uint16_t id = 0;
    esp_err_t err = ens160_read_part_id(&id);
    if (err == ESP_OK && id != ENS160_PART_ID) {
        if (st->ens_init_err < ERRORS_LOGGED) {
            ESP_LOGE(TAG, "ENS160 PART_ID 0x%04X, expected 0x%04X (DS Table 17)",
                     id, ENS160_PART_ID);
        }
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK) {
        err = ens160_set_opmode(ENS160_OPMODE_IDLE);
    }
#if ENS160_ENABLE
    if (err == ESP_OK) {
        err = ens160_set_opmode(ENS160_OPMODE_STANDARD);
    }
    if (err == ESP_OK) {
        err = ens160_sync_misr();
    }
#else
    if (err == ESP_OK) {
        err = ens160_set_opmode(ENS160_OPMODE_DEEP_SLEEP);
    }
#endif
    if (err != ESP_OK) {
        if (++st->ens_init_err <= ERRORS_LOGGED) {
            ESP_LOGE(TAG, "ENS160 start failed: %s; retry every %lld s, tvoc/eco2 stay empty",
                     esp_err_to_name(err), ENS160_RETRY_US / 1000000);
        }
        return false;
    }
#if ENS160_ENABLE
    ESP_LOGI(TAG, "ENS160 PART_ID 0x%04X, OPMODE STANDARD at uptime %" PRId64 " s",
             id, esp_timer_get_time() / 1000000);
#else
    ESP_LOGW(TAG, "ENS160 PART_ID 0x%04X, DEEP SLEEP (ENS160_ENABLE=0): heater off, "
                  "tvoc/eco2 stay empty", id);
#endif
    return true;
}

/* AHT21 T/RH -> TEMP_IN/RH_IN, only in the cycle that produced a new AHT21
 * reading (every 2 s). DS §8.2: compensation input for all outputs. */
static void ens160_compensate(sampler_state_t *st)
{
    if (!st->th_valid || st->th_cycle != st->cycle) {
        return;
    }
    uint16_t t_raw = 0;
    uint16_t rh_raw = 0;
    if (ens160_encode_temp(st->temp_c, &t_raw) != ESP_OK ||
        ens160_encode_rh(st->rh_pct, &rh_raw) != ESP_OK) {
        /* outside -40..85 °C / 0..100 %: the sensor keeps the last value */
        ++st->ens_comp_skipped;
        return;
    }
    const esp_err_t err = ens160_write_compensation(t_raw, rh_raw);
    if (err == ESP_OK) {
        ++st->ens_comp_writes;
    } else if (++st->ens_comp_err <= ERRORS_LOGGED) {
        ESP_LOGW(TAG, "ENS160 compensation write: %s", esp_err_to_name(err));
    }
}

static void ens160_note_status(sampler_state_t *st, const ens160_data_t *d, int64_t now_us)
{
    const ens160_verdict_t v = ens160_verdict(d);
    ++st->ens_verdicts[v];
    if (d->new_data) {
        /* NEWDAT gaps measure the data rate the DS does not state; at a
         * 1 Hz poll a rate of 1 Hz or faster shows up as ~1000 ms gaps */
        ++st->ens_newdat;
        if (st->ens_last_newdat_us != 0) {
            logutil_stats_add(&st->ens_newdat_gap_us, now_us - st->ens_last_newdat_us);
        }
        st->ens_last_newdat_us = now_us;
    } else {
        ++st->ens_no_newdat;
    }
    if ((int)v != st->ens_last_verdict) {
        if (st->ens_transitions_logged < ENS160_TRANSITIONS_LOGGED) {
            ++st->ens_transitions_logged;
            ESP_LOGI(TAG, "ENS160 status %s -> %s at uptime %" PRId64 " s (DEVICE_STATUS 0x%02X)",
                     ens160_verdict_name(st->ens_last_verdict), ens160_verdict_name(v),
                     now_us / 1000000, d->status);
        }
        st->ens_last_verdict = (int)v;
    }
    /* P7 (bring-up 16 Oct): STATAS can still be 0 one second after
     * STANDARD was written, so NOT_RUNNING asks OPMODE before restarting
     * (ens160_status_action). The extra read happens only in that case. */
    uint8_t opmode = 0;
    const bool opmode_ok = v == ENS160_VERDICT_NOT_RUNNING &&
                           ens160_read_opmode(&opmode) == ESP_OK;
    const ens160_action_t action = ens160_status_action(v, opmode_ok, opmode,
                                                        &st->ens_wait_streak);
    /* RESTART although OPMODE still reads STANDARD = the WAIT cap was hit */
    const bool wait_capped = action == ENS160_ACTION_RESTART && v == ENS160_VERDICT_NOT_RUNNING &&
                             opmode_ok && opmode == ENS160_OPMODE_STANDARD;
    if (action == ENS160_ACTION_WAIT) {
        if (++st->ens_notrun_wait <= ERRORS_LOGGED) {
            ESP_LOGI(TAG, "ENS160 STATAS=0 but OPMODE=0x%02X (STANDARD): starting, not restarting",
                     opmode);
        }
    } else if (action == ENS160_ACTION_RESTART) {
        /* sensor fell back to IDLE (e.g. its own power dip), the OPMODE
         * read failed, STATAS stayed 0 for ENS160_WAIT_MAX_CYCLES cycles,
         * or it flags an error: run the start-up sequence again */
        if (wait_capped) {
            if (++st->ens_wait_cap <= ERRORS_LOGGED) {
                ESP_LOGW(TAG, "ENS160 STATAS=0 for %u cycles with OPMODE=0x02: restarting",
                         (unsigned)ENS160_WAIT_MAX_CYCLES);
            }
        } else if (v == ENS160_VERDICT_NOT_RUNNING && st->ens_restarts < ERRORS_LOGGED) {
            if (opmode_ok) {
                ESP_LOGW(TAG, "ENS160 not running, OPMODE=0x%02X: restarting", opmode);
            } else {
                ESP_LOGW(TAG, "ENS160 not running and OPMODE read failed: restarting");
            }
        }
        st->ens_ready = false;
        st->ens_next_try_us = now_us + ENS160_RETRY_US;
        ++st->ens_restarts;
    }
}

static void ens160_step(sampler_state_t *st, csv_sample_t *row)
{
    const int64_t t_start = esp_timer_get_time();
    st->voc_fill = ENS160_FILL_EMPTY;
    if (!st->ens_ready) {
        if (t_start >= st->ens_next_try_us) {
            st->ens_next_try_us = t_start + ENS160_RETRY_US;
            st->ens_ready = ens160_start_sensor(st);
        }
    } else if (ENS160_ENABLE) {
        ens160_compensate(st);

        uint8_t raw[ENS160_DATA_LEN];
        uint8_t misr_exp = 0;
        uint8_t misr_got = 0;
        ens160_data_t d;
        bool read_ok = false;
        const esp_err_t err = ens160_read_data(raw, &misr_exp, &misr_got);
        if (err == ESP_OK) {
            read_ok = ens160_parse_data(raw, &d) == ESP_OK;
        } else if (err == ESP_ERR_INVALID_CRC) {
            if (++st->ens_misr_err <= ERRORS_LOGGED) {
                ESP_LOGW(TAG, "ENS160 MISR mismatch: calc %02X read %02X | %02X %02X %02X %02X %02X %02X",
                         misr_exp, misr_got, raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
            }
        } else if (++st->ens_read_err <= ERRORS_LOGGED) {
            ESP_LOGW(TAG, "ENS160 read: %s", esp_err_to_name(err));
        }
        const int64_t now_us = esp_timer_get_time();
        if (read_ok) {
            ens160_note_status(st, &d, now_us);
        }
        st->voc_fill = ens160_hold_step(&st->ens_hold, read_ok, read_ok ? &d : NULL, now_us,
                                        ENS160_MAX_AGE_US, &row->tvoc_ppb, &row->eco2_ppm);
    }
    row->has_voc = st->voc_fill != ENS160_FILL_EMPTY;
    logutil_stats_add(&st->ens_xfer_us, esp_timer_get_time() - t_start);
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

    /* the row's ML result travels with it so the AGML line pairs with it */
    if (storage_submit_row(row, &st->ml_last)) {
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
        if (row->has_co) {
            ++st->co_filled;
        } else {
            ++st->co_missing;
        }
        switch (st->voc_fill) {
        case ENS160_FILL_NEW: ++st->voc_new; break;
        case ENS160_FILL_REUSED: ++st->voc_reused; break;
        case ENS160_FILL_EMPTY:
        default: ++st->voc_missing; break;
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

/* FW-7: once, before the 1 Hz loop starts (so it cannot move the period;
 * the sampler's stack HWM covers this path too). Runs the golden vectors
 * and the persistence fixture of the model header with the ESP32-S3's own
 * arithmetic (newlib log1pf, xtensa FPU): the board must reproduce
 * Python's D² within the header tolerance before any score is trusted.
 * A failure only turns scoring off (status model_fault until reboot): no
 * abort, no reboot, the logger and the CO path keep running. */
static void ml_self_test(sampler_state_t *st)
{
    ag_mahal_self_test_t r;
    const int64_t t0 = esp_timer_get_time();
    st->ml_ok = ag_mahal_self_test(&r);
    const int64_t dt = esp_timer_get_time() - t0;

    char names[64] = "";
    size_t used = 0;
    for (unsigned j = 0; j < ag_mahal_n_features(); ++j) {
        const int n = snprintf(names + used, sizeof(names) - used, "%s%s",
                               j > 0 ? "," : "", ag_mahal_feature_name(j));
        if (n < 0 || (size_t)n >= sizeof(names) - used) {
            break;   /* truncated, still NUL-terminated */
        }
        used += (size_t)n;
    }
    if (st->ml_ok) {
        ESP_LOGI(TAG, "mahal self-test PASS: model=%s features=%s threshold_d2=%.4f K=%u | "
                      "params ok golden %u/%u seq %u/%u max_abs_err=%.6f | %" PRId64 " us",
                 ag_mahal_model_version(), names, (double)ag_mahal_threshold_d2(),
                 ag_mahal_persistence_k(), r.golden_pass, r.golden_total, r.seq_pass,
                 r.seq_total, (double)r.max_abs_err, dt);
        return;
    }
    ESP_LOGE(TAG, "mahal self-test FAIL: model=%s features=%s | params %s golden %u/%u "
                  "seq %u/%u (first seq fail #%d) max_abs_err=%.6f | %" PRId64 " us",
             ag_mahal_model_version(), names, r.params_ok ? "ok" : "BAD",
             r.golden_pass, r.golden_total, r.seq_pass, r.seq_total, r.first_seq_fail,
             (double)r.max_abs_err, dt);
    unsigned logged = 0;
    for (unsigned g = 0; g < ag_mahal_golden_count() && logged < ML_SELF_TEST_FAILS_LOGGED; ++g) {
        float got = NAN;
        float ref = NAN;
        if (!ag_mahal_golden_check(g, &got, &ref)) {
            ++logged;
            ESP_LOGE(TAG, "mahal golden #%u: board D2=%.6f header D2=%.6f", g,
                     (double)got, (double)ref);
        }
    }
    ESP_LOGE(TAG, "ML scoring OFF until reboot (status model_fault); rows, CSV and the CO "
                  "path are not affected");
}

/* FW-7: score the finished row of this cycle and publish the result.
 * now_us = cycle start (t0), so the gap rule sees the regular 1 Hz grid. */
static void ml_step(sampler_state_t *st, const csv_sample_t *row, int64_t now_us)
{
    const int64_t t_start = esp_timer_get_time();
    ag_ml_result_t r = {
        .status = AG_ML_MODEL_FAULT,
        .d2 = NAN,
        .streak = 0,
        .at_us = now_us,
    };
    if (st->ml_ok) {
        float x[AG_MAHAL_ROW_LEN];
        ag_mahal_row_from_sample(row, x);
        r.d2 = ag_mahal_d2(x);
        const ag_mahal_persist_t *p = &st->ml_persist;
        /* same test as ag_mahal_persist_step: counted for the summary */
        if (p->has_prev &&
            (now_us < p->prev_us || now_us - p->prev_us > AG_MAHAL_GAP_RESET_US)) {
            ++st->ml_gaps;
        }
        r.status = ag_mahal_persist_step(&st->ml_persist, r.d2, now_us);
        r.streak = st->ml_persist.streak;
        if (r.status == AG_ML_PERSISTENT && st->ml_last.status != AG_ML_PERSISTENT) {
            ++st->ml_episodes;
        }
        if (isfinite(r.d2) && (isnan(st->ml_d2_max) || r.d2 > st->ml_d2_max)) {
            st->ml_d2_max = r.d2;
        }
    }
    ++st->ml_cycles[r.status];
    st->ml_last = r;
    ml_mailbox_publish(&r);
    logutil_stats_add(&st->ml_us, esp_timer_get_time() - t_start);
}

/* FW-7 D3-A: one line per 60 s (part of summary()). */
static void ml_summary(sampler_state_t *st)
{
    const uint32_t *n = st->ml_cycles;
    ESP_LOGI(TAG, "ml %s model=%s | since boot cycles missing=%" PRIu32 " below=%" PRIu32
             " above=%" PRIu32 " persistent=%" PRIu32 " fault=%" PRIu32 " episodes=%" PRIu32
             " gaps=%" PRIu32 " | last 60s D2 max=%.4f compute avg=%" PRId64 " max=%" PRId64
             " us | now %s D2=%.4f streak=%u",
             st->ml_ok ? "on" : "OFF (self-test failed)", ag_mahal_model_version(),
             n[AG_ML_MISSING], n[AG_ML_BELOW], n[AG_ML_ABOVE], n[AG_ML_PERSISTENT],
             n[AG_ML_MODEL_FAULT], st->ml_episodes, st->ml_gaps,
             (double)st->ml_d2_max, logutil_stats_avg(&st->ml_us), st->ml_us.max,
             ag_ml_status_name(st->ml_last.status), (double)st->ml_last.d2,
             (unsigned)st->ml_last.streak);
    st->ml_d2_max = NAN;
    logutil_stats_reset(&st->ml_us);
}

/* Part of the 60 s summary; also reads back DATA_T/DATA_RH so the log shows
 * that the AHT21 compensation reached the sensor (DS §16.2.12-13). */
static void ens160_summary(sampler_state_t *st)
{
    const uint32_t *n = st->ens_verdicts;
    ESP_LOGI(TAG, "since boot: rows VOC new=%" PRIu32 " reused(<=%d ms)=%" PRIu32 " empty=%" PRIu32
             " | ens160 %s now=%s | reads normal=%" PRIu32 " warmup=%" PRIu32 " startup=%" PRIu32
             " invalid=%" PRIu32 " range=%" PRIu32 " stater=%" PRIu32 " notrun=%" PRIu32
             " | err start=%" PRIu32 " i2c=%" PRIu32 " misr=%" PRIu32 " restarts=%" PRIu32 " notrun_wait=%" PRIu32 " wait_cap=%" PRIu32,
             st->voc_new, ENS160_MAX_AGE_MS, st->voc_reused, st->voc_missing,
             !ENS160_ENABLE ? "DISABLED (deep sleep)" : st->ens_ready ? "ready" : "NOT READY",
             ens160_verdict_name(st->ens_last_verdict),
             n[ENS160_VERDICT_OK], n[ENS160_VERDICT_WARMUP], n[ENS160_VERDICT_STARTUP],
             n[ENS160_VERDICT_INVALID], n[ENS160_VERDICT_RANGE], n[ENS160_VERDICT_ERROR],
             n[ENS160_VERDICT_NOT_RUNNING],
             st->ens_init_err, st->ens_read_err, st->ens_misr_err, st->ens_restarts, st->ens_notrun_wait,
             st->ens_wait_cap);
    uint16_t t_raw = 0;
    uint16_t rh_raw = 0;
    esp_err_t th_err = ESP_ERR_INVALID_STATE;
    if (ENS160_ENABLE && st->ens_ready) {
        th_err = ens160_read_used_th(&t_raw, &rh_raw);
        if (th_err == ESP_ERR_INVALID_CRC) {
            ++st->ens_misr_err;
        } else if (th_err != ESP_OK) {
            ++st->ens_read_err;
        }
    }
    char used[48] = "n/a";
    if (th_err == ESP_OK) {
        snprintf(used, sizeof(used), "T=%.2f C RH=%.2f %%",
                 (double)ens160_decode_temp(t_raw), (double)ens160_decode_rh(rh_raw));
    }
    ESP_LOGI(TAG, "ens160: newdat since boot=%" PRIu32 " (no newdat %" PRIu32 ") | last 60s gap min=%"
             PRId64 " max=%" PRId64 " ms (n=%" PRIu32 ") | comp writes=%" PRIu32 " err=%" PRIu32
             " skipped=%" PRIu32 " | sensor uses %s",
             st->ens_newdat, st->ens_no_newdat,
             st->ens_newdat_gap_us.min / 1000, st->ens_newdat_gap_us.max / 1000,
             st->ens_newdat_gap_us.count,
             st->ens_comp_writes, st->ens_comp_err, st->ens_comp_skipped, used);
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
             " | rows w/o T/RH=%" PRIu32 " | rows CO filled=%" PRIu32 " empty=%" PRIu32
             " | pms frames=%" PRIu32 " err=%" PRIu32
             " | aht trig_err=%" PRIu32 " read_err=%" PRIu32 " crc_err=%" PRIu32 " busy=%" PRIu32,
             st->pm_new, PMS_MAX_AGE_MS, st->pm_reused, st->pm_missing,
             st->th_missing, st->co_filled, st->co_missing, st->pms_frames, st->pms_err,
             st->aht_trigger_err, st->aht_read_err, st->aht_crc_err, st->aht_busy);
    ESP_LOGI(TAG, "since boot: stack HWM sampler=%u storage=%u console=%u B | heap free now=%u min=%u B",
             (unsigned)uxTaskGetStackHighWaterMark(NULL),
             st->cfg.storage_task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(st->cfg.storage_task) : 0U,
             st->cfg.console_task != NULL ? (unsigned)uxTaskGetStackHighWaterMark(st->cfg.console_task) : 0U,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    ens160_summary(st);
    ESP_LOGI(TAG, "last 60s: sampler work per cycle avg=%" PRId64 " max=%" PRId64
             " us | ens160 i2c per cycle avg=%" PRId64 " max=%" PRId64 " us",
             logutil_stats_avg(&st->work_us), st->work_us.max,
             logutil_stats_avg(&st->ens_xfer_us), st->ens_xfer_us.max);
    ml_summary(st);
    logutil_stats_reset(&st->period_us);
    logutil_stats_reset(&st->work_us);
    logutil_stats_reset(&st->ens_xfer_us);
    logutil_stats_reset(&st->ens_newdat_gap_us);
}

static void co_step(csv_sample_t *row)
{
    co_status_t cs;
    uint16_t x10 = 0;
    if (co_peek_status(&cs) &&
        co_status_csv_value(&cs, esp_timer_get_time(), CO_MAX_AGE_US, &x10)) {
        row->has_co = true;
        row->co_ppm = (float)x10 / 10.0f;   /* exact one decimal: host test_ze07_parse */
    }
}

static void sampler_task(void *arg)
{
    (void)arg;
    sampler_state_t *st = &s_st;
    ml_self_test(st);
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
        /* after the AHT21 so a T/RH read in this cycle is written as
         * compensation before the ENS160 data is read */
        ens160_step(st, &row);
        co_step(&row);
        /* the row is complete: score it before it is queued (FW-7) */
        ml_step(st, &row, t0);

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
        /* busy time of this cycle (the summary cycle includes its own log
         * output); the next summary reports it */
        logutil_stats_add(&st->work_us, esp_timer_get_time() - t0);

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
    /* same 100 kHz bus; ENS160 allows up to 1 MHz (DS Table 12), the
     * AHT21 (400 kHz max) sets the limit and 100 kHz is chosen above */
    err = ens160_attach(bus, I2C_SCL_HZ);
    if (err != ESP_OK) {
        return err;
    }
    s_st.ens_last_verdict = -1;
    s_st.ml_d2_max = NAN;
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
