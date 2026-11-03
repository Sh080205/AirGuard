#pragma once
/* FW-8 step 2: JSON text for the on-device dashboard (docs/12-DASHBOARD-
 * DESIGN.md §3) and the 10-minute history ring (§2.4). Pure C: no
 * FreeRTOS, no sockets, no malloc, so firmware/test_host builds it with gcc.
 *
 * Read-only mirror: everything here is a copy of what the sampler, the CO
 * task and the io task (FSM) already decided. Nothing is decided again,
 * and the dashboard is NOT a CO alarm (CLAUDE.md §8).
 *
 * Not the CSV contract (CLAUDE.md §7): this JSON never reaches a data file.
 * It follows the same rules for values, though: an empty field is JSON
 * null (never 0 or -1), and temp_c / humidity_pct / co_ppm use
 * csv_round_tenths, so the page shows the same digits as the CSV row. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "csv_row.h"

#ifdef __cplusplus
extern "C" {
#endif

/* "v" field of every live message; bump when the format changes. */
#define WEB_JSON_VERSION 1

/* §2.4, owner approved Q2 (3 Nov 2026): 10 min of 1 Hz samples. */
#define WEB_HIST_LEN 600U

/* CO snapshot fields the page shows (co_safety.h co_status_t). */
typedef struct {
    bool present;            /* false: no CO path in this build -> "co": null */
    bool alarm;
    bool fault;
    bool warmup;
    bool stale;              /* co_status_stale(): CO task stopped publishing */
    bool has_mean_1h;
    uint16_t mean_1h_x10;    /* 0.1 ppm, like co_status_t */
} web_co_t;

/* Mahalanobis result (ag_mahal.h ag_ml_result_t) plus model constants. */
typedef struct {
    bool present;            /* false: no ML in this build -> "ml": null */
    const char *status;      /* ag_ml_status_name(); NULL -> null */
    float d2;                /* NAN or infinite -> null */
    float threshold_d2;
    uint16_t streak;
    const char *model;       /* ag_mahal_model_version(); NULL -> null */
} web_ml_t;

/* One live message: the newest row + what the LEDs show + ML + CO. */
typedef struct {
    uint32_t seq;            /* +1 per message, so the page sees a gap */
    int64_t uptime_s;        /* esp_timer seconds of the row */
    bool time_valid;         /* false (or an invalid local_time) -> "t": null */
    csv_sample_t row;        /* has_* == false -> null */
    const char *shown;       /* ag_fsm_show_name(); NULL -> null */
    bool demo;
    bool muted;              /* ML ALARM sound muted by ACK */
    uint8_t fault_reasons;   /* AG_FSM_FAULT_* bits (ag_fsm.h) */
    web_ml_t ml;
    web_co_t co;
} web_snapshot_t;

/* The live JSON object, one line, no trailing newline. Returns its length;
 * 0 (and buf[0] = '\0' when cap > 0) when s or buf is NULL or cap is too
 * small, never half an object. Strings longer than WEB_JSON_STR_MAX are
 * refused (0); with that, WEB_JSON_LIVE_CAP always fits (worst case checked
 * in test_web_json.c: every field at its widest, every string character
 * escaped as \u00XX). */
#define WEB_JSON_LIVE_CAP 1280U
#define WEB_JSON_STR_MAX  32U
size_t web_json_live(const web_snapshot_t *s, char *buf, size_t cap);

/* ---- 10-minute history (§2.4) ----
 * One entry per live message. Kept small (20 B on the ESP32-S3, so the
 * ring is ~12 KB of internal RAM; ค่าคาดหมาย, printed at boot by step 3).
 * The ring has no lock: the web task wraps every call in its mutex. */
#define WEB_HIST_PM   (1U << 0)
#define WEB_HIST_TEMP (1U << 1)
#define WEB_HIST_RH   (1U << 2)

typedef struct {
    uint32_t uptime_s;
    uint16_t pm2_5;
    int16_t temp_x10;        /* csv_round_tenths, valid if WEB_HIST_TEMP */
    int16_t rh_x10;
    uint8_t flags;           /* WEB_HIST_* */
    float d2;                /* NAN = missing (or no ML) */
    const char *shown;       /* static string from ag_fsm_show_name() */
} web_hist_entry_t;

typedef struct {
    web_hist_entry_t e[WEB_HIST_LEN];
    unsigned head;           /* next slot to write */
    unsigned count;          /* <= WEB_HIST_LEN */
} web_hist_t;

void web_hist_init(web_hist_t *h);

/* Append the plot fields of s, overwriting the oldest entry when full. A
 * temp/RH value that csv_round_tenths refuses or that does not fit int16
 * is stored as empty. NULL arguments are ignored. */
void web_hist_push(web_hist_t *h, const web_snapshot_t *s);

unsigned web_hist_count(const web_hist_t *h);

/* i = 0 is the oldest entry; NULL when out of range. */
const web_hist_entry_t *web_hist_at(const web_hist_t *h, unsigned i);

/* One history entry as a JSON array, e.g. [5021,12,33.8,57.0,3.2100,"NORMAL"]
 * (uptime_s, pm2_5, temp_c, humidity_pct, d2, shown; null where empty).
 * The /history.json handler joins these with ',' inside [ ] chunk by chunk.
 * Same return rule as web_json_live; WEB_JSON_HIST_CAP always fits. */
#define WEB_JSON_HIST_CAP 320U
size_t web_json_hist_entry(const web_hist_entry_t *e, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
