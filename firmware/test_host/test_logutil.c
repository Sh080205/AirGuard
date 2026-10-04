#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "logutil.h"

#define PASS(name) printf("PASS %s\n", name)

/* Independent reference: count days one year/month at a time from 1970.
 * Deliberately slow and simple; it shares no code with logutil.c. */
static int64_t naive_days(int y, int m, int d)
{
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t days = 0;
    for (int yy = 1970; yy < y; ++yy) {
        const int leap = (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0;
        days += 365 + leap;
    }
    const int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    for (int mm = 1; mm < m; ++mm) {
        days += mdays[mm - 1] + (mm == 2 ? leap : 0);
    }
    return days + d - 1;
}

static int64_t naive_epoch(int y, int mo, int d, int h, int mi, int s, int off_sec)
{
    return naive_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + s - off_sec;
}

static void expect_parse(const char *s, int64_t want)
{
    int64_t got = -12345;
    const bool ok = logutil_parse_iso8601(s, &got);
    if (!ok || got != want) {
        /* MinGW -pedantic rejects both %lld and PRId64 (I64); epoch values
         * here are < 2^53 so printing as double is exact. */
        fprintf(stderr, "parse \"%s\": ok=%d got=%.0f want=%.0f\n",
                s, ok, (double)got, (double)want);
        assert(0);
    }
}

static void expect_reject(const char *s)
{
    int64_t got = -12345;
    if (logutil_parse_iso8601(s, &got) || got != -12345) {
        fprintf(stderr, "should reject \"%s\"\n", s);
        assert(0);
    }
}

static void test_days_from_civil(void)
{
    /* Every day 1970..2105: covers 2000 (leap, /400) and 2100 (not leap). */
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int64_t expected = 0;
    for (int y = 1970; y <= 2105; ++y) {
        const int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        for (int m = 1; m <= 12; ++m) {
            const int n = mdays[m - 1] + (m == 2 ? leap : 0);
            for (int d = 1; d <= n; ++d) {
                assert(logutil_days_from_civil(y, (unsigned)m, (unsigned)d) == expected);
                ++expected;
            }
        }
    }
    assert(naive_days(2105, 12, 31) + 1 == expected);
    PASS("days_from_civil 1970-01-01..2105-12-31 contiguous");
}

static void test_parse_valid(void)
{
    expect_parse("1970-01-01T00:00:00+00:00", 0);
    expect_parse("1970-01-01T07:00:00+07:00", 0);
    /* Well-known anchor: 2000-03-01T00:00:00Z = 951868800. */
    expect_parse("2000-03-01T00:00:00+00:00", 951868800);
    /* FW-4 example command and the CLAUDE.md §7 example row. */
    expect_parse("2026-10-06T09:00:00+07:00", naive_epoch(2026, 10, 6, 9, 0, 0, 7 * 3600));
    expect_parse("2026-09-23T14:30:00+07:00", naive_epoch(2026, 9, 23, 14, 30, 0, 7 * 3600));
    /* Same instant written in three offsets. */
    const int64_t t = naive_epoch(2026, 10, 6, 2, 0, 0, 0);
    expect_parse("2026-10-06T02:00:00+00:00", t);
    expect_parse("2026-10-06T09:00:00+07:00", t);
    expect_parse("2026-10-05T21:30:00-04:30", t);
    /* Leap days and month ends. */
    expect_parse("2028-02-29T23:59:59+07:00", naive_epoch(2028, 2, 29, 23, 59, 59, 7 * 3600));
    expect_parse("2000-02-29T12:00:00+00:00", naive_epoch(2000, 2, 29, 12, 0, 0, 0));
    expect_parse("2026-12-31T23:59:59+14:00", naive_epoch(2026, 12, 31, 23, 59, 59, 14 * 3600));
    PASS("parse valid timestamps (offsets, leap days, anchors)");
}

static void test_parse_invalid(void)
{
    expect_reject("");
    expect_reject("2026-10-06T09:00:00");              /* no offset */
    expect_reject("2026-10-06T09:00:00Z");             /* Z not accepted */
    expect_reject("2026-10-06T09:00:00+0700");         /* wrong length */
    expect_reject("2026-10-06T09:00:00+07:00 ");       /* trailing space */
    expect_reject("2026-10-06 09:00:00+07:00");        /* space instead of T */
    expect_reject("2026/10/06T09:00:00+07:00");
    expect_reject("2026-10-06T09-00-00+07:00");
    expect_reject("2026-10-06T09:00:00*07:00");
    expect_reject("2026-10-06T09:00:00+07-00");
    expect_reject("2026-1a-06T09:00:00+07:00");        /* non-digit */
    expect_reject("2026-00-06T09:00:00+07:00");        /* month 0 */
    expect_reject("2026-13-06T09:00:00+07:00");        /* month 13 */
    expect_reject("2026-10-00T09:00:00+07:00");        /* day 0 */
    expect_reject("2026-09-31T09:00:00+07:00");        /* 30-day month */
    expect_reject("2026-02-29T09:00:00+07:00");        /* not a leap year */
    expect_reject("2100-02-29T09:00:00+07:00");        /* century, not leap */
    expect_reject("2026-10-06T24:00:00+07:00");
    expect_reject("2026-10-06T09:60:00+07:00");
    expect_reject("2026-10-06T09:00:60+07:00");        /* no leap second */
    expect_reject("2026-10-06T09:00:00+15:00");
    expect_reject("2026-10-06T09:00:00+07:60");
    expect_reject("1969-12-31T23:59:59+00:00");
    assert(!logutil_parse_iso8601(NULL, NULL));
    int64_t e = 0;
    assert(!logutil_parse_iso8601(NULL, &e));
    assert(!logutil_parse_iso8601("2026-10-06T09:00:00+07:00", NULL));
    PASS("parse rejects malformed / out-of-range input, output untouched");
}

static void test_round(void)
{
    assert(logutil_round_to_second(100, 0) == 100);
    assert(logutil_round_to_second(100, 499999) == 100);
    assert(logutil_round_to_second(100, 500000) == 101);
    assert(logutil_round_to_second(100, 999999) == 101);
    PASS("round_to_second at 0 / 0.499999 / 0.5 / 0.999999 s");
}

static void test_gap(void)
{
    int64_t skipped = -1;
    assert(logutil_classify_gap(false, 0, 50, &skipped) == LOGUTIL_GAP_FIRST && skipped == 0);
    assert(logutil_classify_gap(true, 50, 51, &skipped) == LOGUTIL_GAP_OK && skipped == 0);
    assert(logutil_classify_gap(true, 50, 50, &skipped) == LOGUTIL_GAP_DUPLICATE && skipped == 0);
    assert(logutil_classify_gap(true, 50, 52, &skipped) == LOGUTIL_GAP_SKIP && skipped == 1);
    assert(logutil_classify_gap(true, 50, 60, &skipped) == LOGUTIL_GAP_SKIP && skipped == 9);
    assert(logutil_classify_gap(true, 50, 49, &skipped) == LOGUTIL_GAP_BACKWARD && skipped == 0);
    assert(logutil_classify_gap(true, 50, 51, NULL) == LOGUTIL_GAP_OK);
    PASS("classify_gap first/ok/duplicate/skip/backward");
}

static void test_frame_fresh(void)
{
    /* PMS_MAX_AGE_US comes from logutil.h, the same header tasks_sampler.c
     * uses, so the test follows any change of the firmware bound */
    const int64_t max_us = PMS_MAX_AGE_US;
    const int64_t t = 5LL * 1000 * 1000;           /* frame received at 5 s */
    assert(logutil_frame_fresh(true, t, t + 0, max_us));               /* age 0 */
    assert(logutil_frame_fresh(true, t, t + max_us, max_us));          /* bound: inclusive */
    assert(!logutil_frame_fresh(true, t, t + max_us + 1, max_us));     /* 1 us over */

    /* 1 Hz sampler: frame drained at cycle 0, asked again 1/2/3 cycles later
     * → used / used / empty (reused in exactly 2 rows; FW-4 review 2, F1) */
    const int64_t f = 0;
    assert(logutil_frame_fresh(true, f, f + 1000LL * 1000, max_us));   /* +1000 ms */
    assert(logutil_frame_fresh(true, f, f + 2000LL * 1000, max_us));   /* +2000 ms */
    assert(!logutil_frame_fresh(true, f, f + 3000LL * 1000, max_us));  /* +3000 ms */
    /* the decision must not depend on scheduling jitter: same answers with
     * the later cycle a few ms early or late */
    for (int64_t j = -5000; j <= 5000; j += 1000) {                    /* ±5 ms */
        assert(logutil_frame_fresh(true, f, f + 2000LL * 1000 + j, max_us));
        assert(!logutil_frame_fresh(true, f, f + 3000LL * 1000 + j, max_us));
    }
    /* nothing received yet */
    assert(!logutil_frame_fresh(false, t, t, max_us));
    /* no wrap in int64 us: a frame "from the future" or a negative stamp is
     * rejected instead of turning into a huge or negative age */
    assert(!logutil_frame_fresh(true, t, t - 1, max_us));
    assert(!logutil_frame_fresh(true, -1, t, max_us));
    assert(!logutil_frame_fresh(true, 0, INT64_MAX, max_us));
    assert(logutil_frame_fresh(true, INT64_MAX - 10, INT64_MAX, max_us));
    assert(!logutil_frame_fresh(true, t, t, -1));
    PASS("frame_fresh 0/PMS_MAX_AGE kept, +1 us dropped, cycles +1/+2/+3 s = used/used/empty, no-wrap guards");
}

static void test_same_day(void)
{
    struct tm a, b;
    memset(&a, 0, sizeof(a));
    a.tm_year = 126; a.tm_mon = 9; a.tm_mday = 6; a.tm_hour = 23; a.tm_min = 59; a.tm_sec = 59;
    b = a;
    b.tm_hour = 0; b.tm_min = 0; b.tm_sec = 0;
    assert(logutil_same_day(&a, &b));
    b.tm_mday = 7;
    assert(!logutil_same_day(&a, &b));
    b = a; b.tm_mon = 10;
    assert(!logutil_same_day(&a, &b));
    b = a; b.tm_year = 127;
    assert(!logutil_same_day(&a, &b));
    assert(!logutil_same_day(NULL, &b));
    PASS("same_day: midnight rollover changes day/month/year");
}

static void test_stats(void)
{
    logutil_stats_t s;
    logutil_stats_reset(&s);
    assert(s.count == 0 && logutil_stats_avg(&s) == 0 &&
           logutil_stats_jitter(&s, 1000) == 0);
    logutil_stats_add(&s, 1000);
    assert(s.min == 1000 && s.max == 1000 && logutil_stats_jitter(&s, 1000) == 0);
    logutil_stats_add(&s, 997);
    logutil_stats_add(&s, 1002);
    assert(s.count == 3 && s.min == 997 && s.max == 1002);
    assert(logutil_stats_avg(&s) == 999);          /* 2999 / 3 */
    assert(logutil_stats_jitter(&s, 1000) == 3);   /* below by 3 > above by 2 */
    logutil_stats_add(&s, 1010);
    assert(logutil_stats_jitter(&s, 1000) == 10);
    /* Negative values (first sample must set min even when < 0). */
    logutil_stats_reset(&s);
    logutil_stats_add(&s, -5);
    assert(s.min == -5 && s.max == -5);
    /* Whole window above nominal still reports the larger side. */
    logutil_stats_reset(&s);
    logutil_stats_add(&s, 1004);
    logutil_stats_add(&s, 1006);
    assert(logutil_stats_jitter(&s, 1000) == 6);
    PASS("stats min/max/avg/jitter");
}

int main(void)
{
    test_days_from_civil();
    test_parse_valid();
    test_parse_invalid();
    test_round();
    test_gap();
    test_frame_fresh();
    test_same_day();
    test_stats();
    puts("ALL logutil tests passed");
    return 0;
}
