#ifndef AIRGUARD_CO_SAFETY_H
#define AIRGUARD_CO_SAFETY_H

#include <stdbool.h>
#include <stdint.h>

/* FW-5b step 3: CO decision logic, pure C (host-tested, no FreeRTOS).
 *
 * This logic records and tracks the CO level reported by the sensor for
 * research. It is NOT a CO alarm and must not replace one (CLAUDE.md §8).
 * It is fully separate from the ML path: fixed ppm thresholds, no model,
 * no ML persistence, no clock, SD or Wi-Fi (CLAUDE.md §2).
 *
 * All values are integers in 0.1 ppm (ZE07 frame unit, ze07.h), so every
 * threshold comparison is exact. Times are esp_timer_get_time()
 * microseconds since boot (signed 64-bit, no wrap in practice).
 *
 * Thresholds approved by the owner on 18 Oct 2026 (FW-5b report, option B),
 * sources from docs/03-CO_SAFETY.md §2 (WHO indoor guidelines):
 *   instant reading >= 90 ppm -> ALARM. WHO 90 ppm is a 15-min average; one
 *     sample at that level is stricter than the 15-min average, so a
 *     separate 15-min mean adds nothing.
 *   1-h mean >= 30 ppm -> ALARM (WHO 1 h).
 *   8-h mean >= 9 ppm  -> "elevated" flag only, no light/sound (WHO 8 h).
 * The values are compared against what the sensor reports, which is not
 * calibrated (docs/03 §3). */
#define CO_INSTANT_ON_X10    900U
#define CO_MEAN_1H_ON_X10    300U
#define CO_MEAN_8H_ON_X10     90U
/* Leaving a state: every condition below its threshold minus 10 % for
 * CO_CLEAR_HOLD_US of valid samples. Firmware policy (not WHO/datasheet):
 * stops on/off chatter around the threshold; entering stays immediate. */
#define CO_INSTANT_OFF_X10   810U
#define CO_MEAN_1H_OFF_X10   270U
#define CO_MEAN_8H_OFF_X10    81U
#define CO_CLEAR_HOLD_US     (60LL * 1000 * 1000)

/* Manual V1.7: warm-up <= 3 min (p.3), "at least 5 minutes when first
 * using" (p.6 Caution 5). The longer value is used on every boot (owner
 * approved). During warm-up the instant threshold still trips (a wrong
 * reading may over-warn, never stay silent) but samples are not added to
 * the means and co_ppm stays empty in the CSV. */
#define CO_WARMUP_US         (5LL * 60 * 1000 * 1000)

/* FAULT when no valid frame for this long. Firmware policy: the manual
 * says the module sends "every other 1 s" (p.4, ambiguous: 1 or 2 s), so
 * 6 s = 3 times the slower reading. Measure the real period on the board. */
#define CO_SILENCE_US        (6LL * 1000 * 1000)
/* Leave FAULT after this many valid frames in a row (firmware policy). */
#define CO_FAULT_EXIT_FRAMES 3U

/* Rolling means use one bucket per minute since co_safety_init(). The 1-h
 * mean covers the current minute and the 59 before it (59-60 min of data),
 * the 8-h mean 480 buckets the same way. The denominator is the number of
 * valid samples actually in the window, so shortly after boot (or after a
 * gap) the mean can only be higher than or equal to a mean that counts
 * missing seconds as 0 ppm: it may over-warn, never under-warn. */
#define CO_MINUTE_US         (60LL * 1000 * 1000)
#define CO_WINDOW_1H_MIN     60U
#define CO_WINDOW_8H_MIN     480U

/* Bits in co_status_t.alarm_reasons: which ALARM condition is true now. */
#define CO_REASON_INSTANT    (1U << 0)
#define CO_REASON_MEAN_1H    (1U << 1)

/* Bits in co_status_t.fault_reasons. */
#define CO_FAULT_SILENT      (1U << 0)   /* no valid frame for CO_SILENCE_US */
#define CO_FAULT_HW          (1U << 1)   /* UART2 could not be set up */

/* Snapshot published by the CO task through a length-1 queue
 * (xQueueOverwrite / xQueuePeek): the whole struct is copied, so readers
 * never see half an update. Readers: io task (lights/sound), sampler
 * (co_ppm column), later FW-6. */
typedef struct {
    int64_t updated_us;       /* when the CO task published this copy */
    int64_t frame_us;         /* last valid frame; meaningful if have_frame */
    uint32_t seq;             /* +1 per publish */
    uint16_t conc_x10;        /* last valid reading */
    uint16_t mean_1h_x10;     /* 0 when no samples in the window */
    uint16_t mean_8h_x10;
    bool have_frame;
    bool warmup;
    bool alarm;
    bool fault;
    bool elevated_8h;
    uint8_t alarm_reasons;    /* CO_REASON_* true at the last sample */
    uint8_t fault_reasons;    /* CO_FAULT_* */
} co_status_t;

typedef struct {
    int32_t minute;           /* minutes since start; -1 = empty */
    uint32_t sum_x10;
    uint16_t count;
} co_bucket_t;

/* State of the decision logic, owned by the CO task only. */
typedef struct {
    int64_t start_us;
    int64_t last_valid_us;
    bool have_frame;
    uint16_t last_x10;
    bool alarm, elevated, fault, hw_fault;
    bool alarm_clearing, elevated_clearing;
    int64_t alarm_clear_since_us, elevated_clear_since_us;
    uint8_t reasons;
    uint32_t good_streak;
    uint32_t mean_1h_x10, mean_8h_x10;
    uint32_t frames_ok, frames_bad;
    uint32_t alarms_entered, faults_entered;
    co_bucket_t bucket[CO_WINDOW_8H_MIN];
} co_safety_t;

void co_safety_init(co_safety_t *s, int64_t now_us);

/* One valid frame (ze07_parse ESP_OK) received at now_us. Values at or
 * above full scale are passed as they are; they are above every threshold. */
void co_safety_on_frame(co_safety_t *s, int64_t now_us, uint16_t conc_x10);

/* A rejected frame (bad checksum / fields): counted, and breaks the run of
 * good frames needed to leave FAULT. */
void co_safety_on_bad_frame(co_safety_t *s);

/* UART2 setup failed: FAULT until reboot. */
void co_safety_set_hw_fault(co_safety_t *s);

/* Time-based checks (silence -> FAULT). Call at least every few hundred ms
 * and after every frame. */
void co_safety_tick(co_safety_t *s, int64_t now_us);

/* Fill everything except updated_us and seq (the publisher sets those). */
void co_safety_status(const co_safety_t *s, int64_t now_us, co_status_t *out);

/* Reader side: true when the snapshot is older than CO_SILENCE_US, i.e. the
 * CO task itself stopped publishing. Readers must then treat CO as FAULT. */
bool co_status_stale(const co_status_t *st, int64_t now_us);

/* Sampler side: true when st->conc_x10 may fill the co_ppm column at now_us
 * (have a frame, not warming up, not FAULT, frame age <= max_age_us). */
bool co_status_csv_value(const co_status_t *st, int64_t now_us,
                         int64_t max_age_us, uint16_t *conc_x10);

#endif
