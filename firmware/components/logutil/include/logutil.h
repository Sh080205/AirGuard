#pragma once

/* FW-4: pure C helpers for the 1 Hz logger. No FreeRTOS, no drivers, no
 * malloc, so firmware/test_host can compile them with gcc on a PC. */

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- settime console command (D1 fallback) ----
 * Parses exactly "YYYY-MM-DDTHH:MM:SS+HH:MM" (or -HH:MM), 25 characters,
 * the same shape as timestamp_iso in CLAUDE.md §7. Writes Unix epoch seconds
 * (UTC) to *epoch on success; *epoch is untouched on failure.
 * Rejects: wrong length or separators, non-digits, month/day out of range for
 * that year (leap years per Gregorian rule), hour > 23, minute > 59,
 * second > 59 (no leap second), offset hour > 14 or offset minute > 59,
 * year < 1970. */
bool logutil_parse_iso8601(const char *s, int64_t *epoch);

/* Days since 1970-01-01 for a proleptic Gregorian civil date.
 * Algorithm: H. Hinnant, "chrono-Compatible Low-Level Date Algorithms",
 * days_from_civil (public domain). m is 1..12. */
int64_t logutil_days_from_civil(int64_t y, unsigned m, unsigned d);

/* ---- timestamp of a row ----
 * CLAUDE.md §7 has one row per second; FW-4 §5 says the timestamp comes from
 * the system clock rounded to the nearest second (0.5 s rounds up). */
int64_t logutil_round_to_second(int64_t sec, int32_t usec);

typedef enum {
    LOGUTIL_GAP_FIRST,      /* no previous row */
    LOGUTIL_GAP_OK,         /* cur == prev + 1 */
    LOGUTIL_GAP_DUPLICATE,  /* cur == prev: same second written twice */
    LOGUTIL_GAP_SKIP,       /* cur > prev + 1: *skipped seconds missing */
    LOGUTIL_GAP_BACKWARD,   /* cur < prev: clock stepped back */
} logutil_gap_t;

/* Classify consecutive row timestamps (epoch seconds). *skipped is always
 * written: number of missing seconds for SKIP, otherwise 0. */
logutil_gap_t logutil_classify_gap(bool has_prev, int64_t prev, int64_t cur,
                                   int64_t *skipped);

/* ---- hold the last PMS7003 frame (CLAUDE.md §7 note, 4 Oct) ----
 * A row reuses the newest PMS7003 frame while it is at most PMS_MAX_AGE_MS
 * old; older → PM empty. Shared by tasks_sampler.c and the host test so both
 * always use the same bound.
 *
 * The age is counted from when the 1 Hz sampler drained the frame from the
 * driver's buffer, which is up to one sampler period (1 s) after the frame
 * arrived on the UART. With 2500 ms:
 *   - frame drained in cycle n → reused in cycles n+1 (age ≈ 1000 ms) and
 *     n+2 (≈ 2000 ms), never in n+3 (≈ 3000 ms). The bound sits mid-period,
 *     so microsecond scheduling jitter cannot flip the decision (3000 ms sat
 *     exactly on n+3 and made that row random; FW-4 review 2, F1).
 *   - real age since UART arrival ≤ 2 periods + ≤ 1 period lag ≈ 3 s, which
 *     is what CLAUDE.md §7 means by "อายุ ≤ 3 วินาที".
 *   - stable air: the sensor sends every ~2.3 s (manual V2.5, Output result
 *     p. 4 [ต้องเปิด datasheet ยืนยัน]), so drains are 2 or 3 cycles apart
 *     and at most 2 rows in between need a reused frame → no empty PM while
 *     frames keep coming; a missed frame shows up as empty PM, not as a stale
 *     value. ค่าคาดหมาย ต้องยืนยันด้วยการวัด (ตัวนับ new/reused/empty ใน log)
 * Only if the sampler itself overruns can the drain lag exceed one period. */
#define PMS_MAX_AGE_MS 2500
#define PMS_MAX_AGE_US ((int64_t)PMS_MAX_AGE_MS * 1000)

/* True when a frame received at frame_us may still fill the PM columns at
 * now_us: 0 <= now_us - frame_us <= max_age_us (the bound is inclusive).
 * Times are esp_timer_get_time() microseconds since boot: a signed 64-bit
 * count that does not wrap in practice (2^63 us ≈ 292,000 years), so there
 * is no wrap handling; instead a frame "from the future" (now_us < frame_us)
 * or a negative timestamp is rejected, which also keeps the subtraction
 * from overflowing. have_frame == false (nothing received yet) → false. */
bool logutil_frame_fresh(bool have_frame, int64_t frame_us, int64_t now_us,
                         int64_t max_age_us);

/* True when both struct tm fall on the same local calendar day; used to
 * start a new CSV file at midnight. */
bool logutil_same_day(const struct tm *a, const struct tm *b);

/* ---- min / max / average over a reporting window ---- */
typedef struct {
    uint32_t count;
    int64_t min;
    int64_t max;
    int64_t sum;
} logutil_stats_t;

void logutil_stats_reset(logutil_stats_t *s);
void logutil_stats_add(logutil_stats_t *s, int64_t v);
/* 0 when count == 0; integer division truncates toward zero. */
int64_t logutil_stats_avg(const logutil_stats_t *s);
/* Largest |v - nominal| seen: max(max - nominal, nominal - min).
 * 0 when count == 0. Used as the period jitter of the sampler. */
int64_t logutil_stats_jitter(const logutil_stats_t *s, int64_t nominal);

#ifdef __cplusplus
}
#endif
