#pragma once

/* Pure C formatting for the logger CSV contract in CLAUDE.md §7 and the
 * events file in data/README.md. No file or SD access here (FW-4).
 *
 * Every function writes a NUL-terminated string, returns its length without
 * the NUL, and returns 0 when buf is too small or an argument is invalid
 * (NULL pointer, empty prefix, struct tm field out of range). It never writes
 * past buf[cap - 1]; on failure buf[0] is set to '\0' when cap > 0.
 * No malloc.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    struct tm local_time;      /* Already UTC+7; the row always says +07:00. */
    bool has_pm;      uint16_t pm1_0, pm2_5, pm10;     /* atmospheric set (D3) */
    bool has_th;      float temp_c, humidity_pct;
    bool has_voc;     uint16_t tvoc_ppb, eco2_ppm;
    bool has_co;      float co_ppm;
    bool marked;                                       /* D2 */
} csv_sample_t;

/* "timestamp_iso,pm1_0,...,co_ppm,label\n" exactly as CLAUDE.md §7. */
size_t csv_header(char *buf, size_t cap);

/* One data row ending in '\n'.
 * - has_* == false leaves those fields empty (never 0 or -1).
 * - temp_c, humidity_pct, co_ppm: one decimal, rounded half away from zero
 *   on the stored float value (28.45f is 28.4500008 -> "28.5";
 *   28.25f is exact -> "28.3"; -0.04f -> "0.0", never "-0.0").
 *   NaN, infinity or |value| >= 1e8 is written as an empty field.
 * - label is "marked" when marked, otherwise "normal".
 * The longest possible row is 99 characters, so 100 bytes always fit. */
size_t csv_format_row(const csv_sample_t *s, char *buf, size_t cap);

/* "<prefix>_YYYYMMDD_HHMMSS.csv", e.g. "AG" -> AG_20260923_143000.csv,
 * "EV" -> EV_20260923_143000.csv (data/README.md). */
size_t csv_filename(const struct tm *t, const char *prefix,
                    char *buf, size_t cap);

/* "start_iso,end_iso,label,note\n" exactly as data/README.md, written once
 * at the top of each EV_*.csv file (FW-4). */
size_t csv_event_header(char *buf, size_t cap);

/* One events row "start_iso,end_iso,marked,\n": label is always "marked"
 * and note is left empty for a person to fill in (data/README.md, D2). */
size_t csv_event_row(const struct tm *start, const struct tm *end,
                     char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
