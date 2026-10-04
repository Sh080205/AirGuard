#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "csv_row.h"

/* CLAUDE.md §7, typed by hand here on purpose: not taken from csv_row.c. */
static const char HEADER[] =
    "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,"
    "tvoc_ppb,eco2_ppm,co_ppm,label\n";

static struct tm make_tm(int y, int mo, int d, int h, int mi, int s)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    return t;
}

/* The CLAUDE.md §7 example row. */
static csv_sample_t example(void)
{
    csv_sample_t s;
    memset(&s, 0, sizeof(s));
    s.local_time = make_tm(2026, 9, 23, 14, 30, 0);
    s.has_pm = true;
    s.pm1_0 = 8;
    s.pm2_5 = 12;
    s.pm10 = 15;
    s.has_th = true;
    s.temp_c = 28.4f;
    s.humidity_pct = 62.1f;
    s.has_voc = true;
    s.tvoc_ppb = 145;
    s.eco2_ppm = 520;
    s.has_co = true;
    s.co_ppm = 0.4f;
    s.marked = false;
    return s;
}

static void expect_row(const csv_sample_t *s, const char *expected)
{
    char buf[128];
    const size_t n = csv_format_row(s, buf, sizeof(buf));
    if (n != strlen(expected) || strcmp(buf, expected) != 0) {
        fprintf(stderr, "row mismatch\n got: \"%s\"\nwant: \"%s\"\n",
                buf, expected);
        assert(0);
    }
}

static void expect_temp(float v, const char *expected_field)
{
    csv_sample_t s = example();
    s.temp_c = v;
    char want[128];
    snprintf(want, sizeof(want),
             "2026-09-23T14:30:00+07:00,8,12,15,%s,62.1,145,520,0.4,normal\n",
             expected_field);
    expect_row(&s, want);
}

/* Every cap below the needed size must return 0, leave an empty string
 * and never touch a byte at or past buf[cap]. */
static void check_all_caps(size_t (*call)(void *ctx, char *buf, size_t cap),
                           void *ctx, const char *expected)
{
    const size_t need = strlen(expected) + 1;
    char buf[160];
    assert(need + 8 <= sizeof(buf));
    for (size_t cap = 0; cap <= need; ++cap) {
        memset(buf, 0x5a, sizeof(buf));
        const size_t n = call(ctx, buf, cap);
        for (size_t i = cap; i < sizeof(buf); ++i) {
            assert(buf[i] == 0x5a);
        }
        if (cap < need) {
            assert(n == 0);
            if (cap > 0) {
                assert(buf[0] == '\0');
            }
        } else {
            assert(n == need - 1);
            assert(strcmp(buf, expected) == 0);
        }
    }
    assert(call(ctx, NULL, 100) == 0);
}

static size_t call_row(void *ctx, char *buf, size_t cap)
{
    return csv_format_row((const csv_sample_t *)ctx, buf, cap);
}

static size_t call_header(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    return csv_header(buf, cap);
}

static size_t call_filename(void *ctx, char *buf, size_t cap)
{
    return csv_filename((const struct tm *)ctx, "AG", buf, cap);
}

static size_t call_event(void *ctx, char *buf, size_t cap)
{
    const struct tm *t = (const struct tm *)ctx;
    return csv_event_row(&t[0], &t[1], buf, cap);
}

/* data/README.md "รูปแบบไฟล์ events", typed by hand. */
static const char EV_HEADER[] = "start_iso,end_iso,label,note\n";

static size_t call_event_header(void *ctx, char *buf, size_t cap)
{
    (void)ctx;
    return csv_event_header(buf, cap);
}

int main(void)
{
    char buf[128];

    assert(csv_header(buf, sizeof(buf)) == strlen(HEADER));
    assert(strcmp(buf, HEADER) == 0);
    check_all_caps(call_header, NULL, HEADER);
    puts("PASS header matches CLAUDE.md section 7, every short cap");

    csv_sample_t s = example();
    expect_row(&s, "2026-09-23T14:30:00+07:00,8,12,15,28.4,62.1,145,520,0.4,normal\n");
    s.marked = true;
    expect_row(&s, "2026-09-23T14:30:00+07:00,8,12,15,28.4,62.1,145,520,0.4,marked\n");
    puts("PASS full row (section 7 example), label normal and marked");

    s = example();
    s.has_th = false;
    s.temp_c = 0.0f;
    expect_row(&s, "2026-09-23T14:30:00+07:00,8,12,15,,,145,520,0.4,normal\n");
    s = example();
    s.has_pm = false;
    expect_row(&s, "2026-09-23T14:30:00+07:00,,,,28.4,62.1,145,520,0.4,normal\n");
    s = example();
    s.has_voc = false;
    expect_row(&s, "2026-09-23T14:30:00+07:00,8,12,15,28.4,62.1,,,0.4,normal\n");
    s = example();
    s.has_co = false;
    expect_row(&s, "2026-09-23T14:30:00+07:00,8,12,15,28.4,62.1,145,520,,normal\n");
    s.has_pm = s.has_th = s.has_voc = false;
    s.marked = true;
    expect_row(&s, "2026-09-23T14:30:00+07:00,,,,,,,,,marked\n");
    puts("PASS missing groups are empty fields, not 0 or -1");

    s = example();
    s.local_time = make_tm(2027, 1, 2, 3, 4, 5);
    s.pm1_0 = 0;
    s.pm2_5 = 0;
    s.pm10 = 65535;
    s.tvoc_ppb = 0;
    s.eco2_ppm = 65535;
    s.co_ppm = 0.0f;
    expect_row(&s, "2027-01-02T03:04:05+07:00,0,0,65535,28.4,62.1,0,65535,0.0,normal\n");
    puts("PASS zero-padded time, real zeros and uint16 limits");

    expect_temp(28.45f, "28.5");   /* stored as 28.4500008 */
    expect_temp(28.25f, "28.3");   /* exact half: away from zero */
    expect_temp(28.35f, "28.4");   /* stored as 28.3500004 */
    expect_temp(28.44f, "28.4");
    expect_temp(28.0f, "28.0");
    expect_temp(-0.04f, "0.0");    /* never "-0.0" */
    expect_temp(-0.05f, "-0.1");   /* stored as -0.0500000007 */
    expect_temp(-12.25f, "-12.3"); /* exact half: away from zero */
    expect_temp(-50.0f, "-50.0");
    expect_temp(99.95f, "99.9");   /* stored as 99.9499969: below half */
    expect_temp(99.96f, "100.0");  /* carry into the integer part */
    puts("PASS one-decimal rounding: half away from zero on the stored float");

    expect_temp(NAN, "");
    expect_temp(INFINITY, "");
    expect_temp(-INFINITY, "");
    expect_temp(1e8f, "");
    puts("PASS NaN, infinity and out-of-range values become empty fields");

    s = example();
    s.local_time = make_tm(9999, 12, 31, 23, 59, 59);
    s.pm1_0 = s.pm2_5 = s.pm10 = 65535;
    s.temp_c = s.humidity_pct = s.co_ppm = -99999992.0f; /* widest kept */
    s.tvoc_ppb = s.eco2_ppm = 65535;
    s.marked = true;
    static const char widest[] =
        "9999-12-31T23:59:59+07:00,65535,65535,65535,-99999992.0,"
        "-99999992.0,65535,65535,-99999992.0,marked\n";
    assert(strlen(widest) == 99); /* matches the bound in csv_row.h */
    expect_row(&s, widest);
    puts("PASS widest possible row is 99 characters");

    s = example();
    check_all_caps(call_row, &s, "2026-09-23T14:30:00+07:00,8,12,15,28.4,62.1,145,520,0.4,normal\n");
    puts("PASS row with short buffer returns 0, no byte past cap written");

    s = example();
    s.local_time.tm_mon = 12;
    assert(csv_format_row(&s, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    s = example();
    s.local_time.tm_hour = 24;
    assert(csv_format_row(&s, buf, sizeof(buf)) == 0);
    s = example();
    s.local_time.tm_mday = 0;
    assert(csv_format_row(&s, buf, sizeof(buf)) == 0);
    s = example();
    s.local_time.tm_year = 10000 - 1900;
    assert(csv_format_row(&s, buf, sizeof(buf)) == 0);
    assert(csv_format_row(NULL, buf, sizeof(buf)) == 0);
    puts("PASS invalid struct tm and NULL sample return 0");

    struct tm t = make_tm(2026, 9, 23, 14, 30, 0);
    assert(csv_filename(&t, "AG", buf, sizeof(buf)) == 22);
    assert(strcmp(buf, "AG_20260923_143000.csv") == 0);
    struct tm t2 = make_tm(2026, 1, 5, 7, 8, 9);
    assert(csv_filename(&t2, "EV", buf, sizeof(buf)) == 22);
    assert(strcmp(buf, "EV_20260105_070809.csv") == 0);
    check_all_caps(call_filename, &t, "AG_20260923_143000.csv");
    assert(csv_filename(&t, "", buf, sizeof(buf)) == 0);
    assert(csv_filename(&t, NULL, buf, sizeof(buf)) == 0);
    assert(csv_filename(NULL, "AG", buf, sizeof(buf)) == 0);
    puts("PASS file names AG_/EV_YYYYMMDD_HHMMSS.csv, short buffer, bad args");

    struct tm ev[2] = {make_tm(2026, 9, 23, 14, 32, 5),
                       make_tm(2026, 9, 23, 14, 41, 0)};
    static const char ev_row[] =
        "2026-09-23T14:32:05+07:00,2026-09-23T14:41:00+07:00,marked,\n";
    assert(csv_event_row(&ev[0], &ev[1], buf, sizeof(buf)) == strlen(ev_row));
    assert(strcmp(buf, ev_row) == 0);
    check_all_caps(call_event, ev, ev_row);
    assert(csv_event_row(NULL, &ev[1], buf, sizeof(buf)) == 0);
    assert(csv_event_row(&ev[0], NULL, buf, sizeof(buf)) == 0);
    puts("PASS event row start,end,marked,(empty note), short buffer, NULL");

    assert(csv_event_header(buf, sizeof(buf)) == strlen(EV_HEADER));
    assert(strcmp(buf, EV_HEADER) == 0);
    check_all_caps(call_event_header, NULL, EV_HEADER);
    puts("PASS events header matches data/README.md, every short cap");

    puts("ALL csv_row tests passed");
    return 0;
}
