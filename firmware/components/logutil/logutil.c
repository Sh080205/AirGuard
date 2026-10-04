#include "logutil.h"

#include <stddef.h>
#include <string.h>

/* "YYYY-MM-DDTHH:MM:SS+HH:MM" */
#define ISO_LEN 25

static bool read_digits(const char *s, size_t pos, size_t n, unsigned *out)
{
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') {
            return false;
        }
        v = v * 10U + (unsigned)(c - '0');
    }
    *out = v;
    return true;
}

static bool is_leap(unsigned y)
{
    return (y % 4U == 0 && y % 100U != 0) || y % 400U == 0;
}

static unsigned days_in_month(unsigned y, unsigned m)
{
    static const unsigned days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    return (m == 2 && is_leap(y)) ? 29U : days[m - 1];
}

int64_t logutil_days_from_civil(int64_t y, unsigned m, unsigned d)
{
    /* Hinnant days_from_civil: shift the year to start in March so the leap
     * day is the last day of the "year"; era = 400-year cycle (146097 days). */
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                  /* [0, 399] */
    const int64_t mp = (int64_t)m + (m > 2 ? -3 : 9);                   /* [0, 11] */
    const int64_t doy = (153 * mp + 2) / 5 + (int64_t)d - 1;            /* [0, 365] */
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + doe - 719468;   /* 719468 = days 0000-03-01 .. 1970-01-01 */
}

bool logutil_parse_iso8601(const char *s, int64_t *epoch)
{
    if (s == NULL || epoch == NULL || strlen(s) != ISO_LEN) {
        return false;
    }
    if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
        s[16] != ':' || (s[19] != '+' && s[19] != '-') || s[22] != ':') {
        return false;
    }
    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, oh = 0, om = 0;
    if (!read_digits(s, 0, 4, &y) || !read_digits(s, 5, 2, &mo) ||
        !read_digits(s, 8, 2, &d) || !read_digits(s, 11, 2, &h) ||
        !read_digits(s, 14, 2, &mi) || !read_digits(s, 17, 2, &sec) ||
        !read_digits(s, 20, 2, &oh) || !read_digits(s, 23, 2, &om)) {
        return false;
    }
    /* UTC offsets in use span -12:00 .. +14:00; 14 is the bound for both
     * signs here because only the shape matters, not the zone list. */
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo) ||
        h > 23 || mi > 59 || sec > 59 || oh > 14 || om > 59) {
        return false;
    }
    const int64_t local = logutil_days_from_civil(y, mo, d) * 86400 +
                          (int64_t)h * 3600 + (int64_t)mi * 60 + (int64_t)sec;
    const int64_t offset = (int64_t)oh * 3600 + (int64_t)om * 60;
    /* local = UTC + offset  =>  UTC = local - offset */
    *epoch = s[19] == '+' ? local - offset : local + offset;
    return true;
}

int64_t logutil_round_to_second(int64_t sec, int32_t usec)
{
    return usec >= 500000 ? sec + 1 : sec;
}

logutil_gap_t logutil_classify_gap(bool has_prev, int64_t prev, int64_t cur,
                                   int64_t *skipped)
{
    int64_t missing = 0;
    logutil_gap_t gap;
    if (!has_prev) {
        gap = LOGUTIL_GAP_FIRST;
    } else if (cur == prev + 1) {
        gap = LOGUTIL_GAP_OK;
    } else if (cur == prev) {
        gap = LOGUTIL_GAP_DUPLICATE;
    } else if (cur > prev) {
        gap = LOGUTIL_GAP_SKIP;
        missing = cur - prev - 1;
    } else {
        gap = LOGUTIL_GAP_BACKWARD;
    }
    if (skipped != NULL) {
        *skipped = missing;
    }
    return gap;
}

bool logutil_frame_fresh(bool have_frame, int64_t frame_us, int64_t now_us,
                         int64_t max_age_us)
{
    if (!have_frame || frame_us < 0 || now_us < frame_us || max_age_us < 0) {
        return false;
    }
    /* now_us >= frame_us >= 0, so the difference cannot overflow */
    return now_us - frame_us <= max_age_us;
}

bool logutil_same_day(const struct tm *a, const struct tm *b)
{
    return a != NULL && b != NULL && a->tm_year == b->tm_year &&
           a->tm_mon == b->tm_mon && a->tm_mday == b->tm_mday;
}

void logutil_stats_reset(logutil_stats_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}

void logutil_stats_add(logutil_stats_t *s, int64_t v)
{
    if (s == NULL) {
        return;
    }
    if (s->count == 0 || v < s->min) {
        s->min = v;
    }
    if (s->count == 0 || v > s->max) {
        s->max = v;
    }
    s->sum += v;
    ++s->count;
}

int64_t logutil_stats_avg(const logutil_stats_t *s)
{
    return (s == NULL || s->count == 0) ? 0 : s->sum / (int64_t)s->count;
}

int64_t logutil_stats_jitter(const logutil_stats_t *s, int64_t nominal)
{
    if (s == NULL || s->count == 0) {
        return 0;
    }
    const int64_t above = s->max - nominal;
    const int64_t below = nominal - s->min;
    const int64_t worst = above > below ? above : below;
    return worst > 0 ? worst : 0;
}
