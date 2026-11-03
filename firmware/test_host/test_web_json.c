/* FW-8 step 2: dashboard JSON + history ring (components/webui), host only.
 * Expected JSON is typed by hand from docs/12-DASHBOARD-DESIGN.md §3, not
 * taken from web_json.c. */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ag_fsm.h"
#include "csv_row.h"
#include "web_json.h"

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

/* The design-doc example (§3). */
static web_snapshot_t example(void)
{
    web_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.seq = 1234;
    s.uptime_s = 5021;
    s.time_valid = true;
    s.row.local_time = make_tm(2026, 11, 3, 15, 4, 5);
    s.row.has_pm = true;
    s.row.pm1_0 = 8;
    s.row.pm2_5 = 12;
    s.row.pm10 = 15;
    s.row.has_th = true;
    s.row.temp_c = 33.8f;
    s.row.humidity_pct = 57.0f;
    s.row.has_voc = true;
    s.row.tvoc_ppb = 145;
    s.row.eco2_ppm = 520;
    s.row.has_co = true;
    s.row.co_ppm = 0.5f;
    s.shown = "NORMAL";
    s.ml.present = true;
    s.ml.status = "below";
    s.ml.d2 = 3.21f;
    s.ml.threshold_d2 = 17.9200077f;
    s.ml.streak = 0;
    s.ml.model = "ml3-v1+pm_th+e6f29d2be5b3";
    s.co.present = true;
    s.co.has_mean_1h = true;
    s.co.mean_1h_x10 = 5;
    return s;
}

static void expect_live(const web_snapshot_t *s, const char *want)
{
    char buf[WEB_JSON_LIVE_CAP];
    const size_t n = web_json_live(s, buf, sizeof(buf));
    if (n != strlen(want) || strcmp(buf, want) != 0) {
        fprintf(stderr, "live mismatch\n got: %s\nwant: %s\n", buf, want);
        assert(0);
    }
}

static void expect_hist(const web_hist_entry_t *e, const char *want)
{
    char buf[WEB_JSON_HIST_CAP];
    const size_t n = web_json_hist_entry(e, buf, sizeof(buf));
    if (n != strlen(want) || strcmp(buf, want) != 0) {
        fprintf(stderr, "hist mismatch\n got: %s\nwant: %s\n", buf, want);
        assert(0);
    }
}

/* Text of one JSON number field, e.g. "temp_c" -> "33.8" or "null". */
static void json_field(const char *json, const char *key, char *out, size_t cap)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    assert(p != NULL);
    p += strlen(pat);
    const size_t n = strcspn(p, ",}");
    assert(n < cap);
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Field k (0-based) of a CSV row, '\n' stripped; "" = empty. */
static void csv_field(const char *row, int k, char *out, size_t cap)
{
    const char *p = row;
    for (int i = 0; i < k; ++i) {
        p = strchr(p, ',');
        assert(p != NULL);
        ++p;
    }
    const size_t n = strcspn(p, ",\n");
    assert(n < cap);
    memcpy(out, p, n);
    out[n] = '\0';
}

/* Every cap below the full length gives 0 and "" and writes nothing past
 * cap (canary), so a short buffer never leaves half an object. */
static void check_all_caps(const web_snapshot_t *s, const web_hist_entry_t *e)
{
    char full[WEB_JSON_LIVE_CAP];
    const size_t len = s != NULL ? web_json_live(s, full, sizeof(full))
                                 : web_json_hist_entry(e, full, sizeof(full));
    assert(len > 0);
    for (size_t cap = 0; cap <= len; ++cap) {
        char buf[WEB_JSON_LIVE_CAP + 1];
        memset(buf, 'X', sizeof(buf));
        const size_t n = s != NULL ? web_json_live(s, buf, cap)
                                   : web_json_hist_entry(e, buf, cap);
        assert(n == 0);
        if (cap > 0) {
            assert(buf[0] == '\0');
        }
        for (size_t i = cap; i < sizeof(buf); ++i) {
            assert(buf[i] == 'X');
        }
    }
}

static void test_example_and_empty(void)
{
    web_snapshot_t s = example();
    expect_live(&s,
        "{\"v\":1,\"seq\":1234,\"up_s\":5021,\"t\":\"2026-11-03T15:04:05+07:00\","
        "\"pm1_0\":8,\"pm2_5\":12,\"pm10\":15,\"temp_c\":33.8,\"humidity_pct\":57.0,"
        "\"tvoc_ppb\":145,\"eco2_ppm\":520,\"co_ppm\":0.5,\"marked\":false,"
        "\"shown\":\"NORMAL\",\"demo\":false,\"muted\":false,\"sys_fault\":[],"
        "\"ml\":{\"status\":\"below\",\"d2\":3.2100,\"thr\":17.9200,\"streak\":0,"
        "\"model\":\"ml3-v1+pm_th+e6f29d2be5b3\"},"
        "\"co\":{\"alarm\":false,\"fault\":false,\"warmup\":false,\"stale\":false,"
        "\"mean_1h\":0.5}}");
    puts("PASS design-doc example message, field by field");

    web_snapshot_t e;
    memset(&e, 0, sizeof(e));   /* nothing measured, no clock, no ML/CO */
    e.row.marked = true;
    expect_live(&e,
        "{\"v\":1,\"seq\":0,\"up_s\":0,\"t\":null,"
        "\"pm1_0\":null,\"pm2_5\":null,\"pm10\":null,\"temp_c\":null,"
        "\"humidity_pct\":null,\"tvoc_ppb\":null,\"eco2_ppm\":null,\"co_ppm\":null,"
        "\"marked\":true,\"shown\":null,\"demo\":false,\"muted\":false,"
        "\"sys_fault\":[],\"ml\":null,\"co\":null}");
    puts("PASS empty values are null, never 0 or -1; no clock -> t null");

    s = example();
    s.time_valid = false;       /* values still shown, timestamp is not */
    char buf[WEB_JSON_LIVE_CAP], f[32];
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "t", f, sizeof(f));
    assert(strcmp(f, "null") == 0);
    json_field(buf, "pm2_5", f, sizeof(f));
    assert(strcmp(f, "12") == 0);
    s = example();
    s.row.local_time.tm_mon = 12;   /* invalid struct tm is never printed */
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "t", f, sizeof(f));
    assert(strcmp(f, "null") == 0);
    puts("PASS time_valid false or an invalid struct tm -> t null, values kept");

    s = example();
    s.ml.d2 = NAN;
    s.ml.status = "missing";
    s.co.has_mean_1h = false;
    s.co.alarm = s.co.stale = true;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "d2", f, sizeof(f));
    assert(strcmp(f, "null") == 0);
    json_field(buf, "mean_1h", f, sizeof(f));
    assert(strcmp(f, "null") == 0);
    assert(strstr(buf, "\"alarm\":true,\"fault\":false,\"warmup\":false,\"stale\":true") != NULL);
    s.ml.d2 = INFINITY;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "d2", f, sizeof(f));
    assert(strcmp(f, "null") == 0);
    s.ml.d2 = 2.5e9f;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "d2", f, sizeof(f));
    assert(strcmp(f, "2.5000e+09") == 0);
    puts("PASS d2 NaN/inf -> null, >= 1e9 -> %.4e; CO flags and missing mean");

    assert(web_json_live(NULL, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    assert(web_json_live(&s, NULL, 100) == 0);
    puts("PASS NULL snapshot or buffer returns 0");
}

static void test_tenths_match_csv(void)
{
    /* temp_c / humidity_pct / co_ppm text must equal the CSV field for the
     * same float, including the rounding edge cases of csv_row.h */
    static const float edge[] = { 28.45f, 28.25f, -0.04f, -0.05f, -0.4f, 0.0f,
                                  -99999992.0f, 99999992.0f, 1e9f, NAN, INFINITY };
    unsigned checked = 0;
    for (int pass = 0; pass < 2; ++pass) {
        const int n = pass == 0 ? (int)(sizeof(edge) / sizeof(edge[0])) : 40001;
        for (int i = 0; i < n; ++i) {
            const float v = pass == 0 ? edge[i] : -50.0f + (float)i * 0.00375f;
            web_snapshot_t s = example();
            s.row.temp_c = s.row.humidity_pct = s.row.co_ppm = v;
            char row[128], json[WEB_JSON_LIVE_CAP], a[32], b[32];
            assert(csv_format_row(&s.row, row, sizeof(row)) > 0);
            assert(web_json_live(&s, json, sizeof(json)) > 0);
            static const char *const keys[] = { "temp_c", "humidity_pct", "co_ppm" };
            static const int cols[] = { 4, 5, 8 };
            for (int k = 0; k < 3; ++k) {
                csv_field(row, cols[k], a, sizeof(a));
                json_field(json, keys[k], b, sizeof(b));
                if (a[0] == '\0') {
                    assert(strcmp(b, "null") == 0);
                } else if (strcmp(a, b) != 0) {
                    fprintf(stderr, "v=%.9g csv=%s json=%s\n", (double)v, a, b);
                    assert(0);
                }
                ++checked;
            }
        }
    }
    printf("PASS temp/RH/CO text equals the CSV field (%u comparisons, edges incl.)\n",
           checked);
}

static void test_strings_and_faults(void)
{
    web_snapshot_t s = example();
    s.shown = "a\"b\\c\nd\x01";
    char buf[WEB_JSON_LIVE_CAP], f[64];
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"shown\":\"a\\\"b\\\\c\\u000ad\\u0001\"") != NULL);
    puts("PASS quote, backslash and control characters are escaped");

    char longest[WEB_JSON_STR_MAX + 2];
    memset(longest, 'm', sizeof(longest));
    longest[WEB_JSON_STR_MAX] = '\0';
    s.ml.model = longest;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    longest[WEB_JSON_STR_MAX] = 'm';
    longest[WEB_JSON_STR_MAX + 1] = '\0';
    assert(web_json_live(&s, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    puts("PASS string of WEB_JSON_STR_MAX accepted, one longer refused (whole message)");

    s = example();
    s.shown = "SYS FAULT";
    s.fault_reasons = AG_FSM_FAULT_AHT21 | AG_FSM_FAULT_ML_STALE;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"sys_fault\":[\"AHT21\",\"ML_STALE\"]") != NULL);
    s.fault_reasons = 0xFFU;   /* unknown high bits are ignored */
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "\"sys_fault\":[\"AHT21\",\"ENS160\",\"PMS7003\",\"ML_MODEL\","
                       "\"ML_STALE\"]") != NULL);
    s.demo = s.muted = true;
    assert(web_json_live(&s, buf, sizeof(buf)) > 0);
    json_field(buf, "demo", f, sizeof(f));
    assert(strcmp(f, "true") == 0);
    json_field(buf, "muted", f, sizeof(f));
    assert(strcmp(f, "true") == 0);
    puts("PASS sys_fault names follow the AG_FSM_FAULT_* bits; demo/muted flags");
}

static void test_bounds(void)
{
    /* widest possible message: every number at its longest text, every
     * string WEB_JSON_STR_MAX control bytes (6 output chars each) */
    char ctl[WEB_JSON_STR_MAX + 1];
    memset(ctl, 0x01, WEB_JSON_STR_MAX);
    ctl[WEB_JSON_STR_MAX] = '\0';
    web_snapshot_t s = example();
    s.seq = UINT32_MAX;
    s.uptime_s = INT64_MIN;
    s.row.local_time = make_tm(9999, 12, 31, 23, 59, 59);
    s.row.pm1_0 = s.row.pm2_5 = s.row.pm10 = 65535;
    s.row.tvoc_ppb = s.row.eco2_ppm = 65535;
    s.row.temp_c = s.row.humidity_pct = s.row.co_ppm = -99999992.0f;
    s.row.marked = true;
    s.shown = ctl;
    s.demo = s.muted = true;
    s.fault_reasons = 0x1FU;
    s.ml.status = ctl;
    s.ml.model = ctl;
    s.ml.d2 = -FLT_MAX;
    s.ml.threshold_d2 = -FLT_MAX;
    s.ml.streak = UINT16_MAX;
    s.co.alarm = s.co.fault = s.co.warmup = s.co.stale = true;
    s.co.mean_1h_x10 = UINT16_MAX;
    char buf[WEB_JSON_LIVE_CAP];
    const size_t n = web_json_live(&s, buf, sizeof(buf));
    assert(n > 0 && n < WEB_JSON_LIVE_CAP);
    printf("PASS widest live message is %zu chars < WEB_JSON_LIVE_CAP %u\n", n,
           (unsigned)WEB_JSON_LIVE_CAP);

    web_hist_entry_t e = { .uptime_s = UINT32_MAX, .pm2_5 = 65535,
                           .temp_x10 = INT16_MIN, .rh_x10 = INT16_MIN,
                           .flags = WEB_HIST_PM | WEB_HIST_TEMP | WEB_HIST_RH,
                           .d2 = -FLT_MAX, .shown = ctl };
    char hb[WEB_JSON_HIST_CAP];
    const size_t hn = web_json_hist_entry(&e, hb, sizeof(hb));
    assert(hn > 0 && hn < WEB_JSON_HIST_CAP);
    printf("PASS widest history entry is %zu chars < WEB_JSON_HIST_CAP %u\n", hn,
           (unsigned)WEB_JSON_HIST_CAP);

    web_snapshot_t ex = example();
    check_all_caps(&ex, NULL);
    web_hist_entry_t he = { .uptime_s = 5021, .pm2_5 = 12, .temp_x10 = 338,
                            .rh_x10 = 570,
                            .flags = WEB_HIST_PM | WEB_HIST_TEMP | WEB_HIST_RH,
                            .d2 = 3.21f, .shown = "NORMAL" };
    check_all_caps(NULL, &he);
    puts("PASS every short buffer returns 0 and \"\", nothing written past cap");
}

static void test_history(void)
{
    static web_hist_t h;    /* ~12 KB on the board: keep it off the stack */
    web_hist_init(&h);
    assert(web_hist_count(&h) == 0 && web_hist_at(&h, 0) == NULL);

    web_snapshot_t s = example();
    web_hist_push(&h, &s);
    assert(web_hist_count(&h) == 1);
    expect_hist(web_hist_at(&h, 0), "[5021,12,33.8,57.0,3.2100,\"NORMAL\"]");

    s.row.has_pm = s.row.has_th = false;
    s.ml.present = false;   /* no ML: d2 null even if a value is left in */
    s.shown = NULL;
    s.uptime_s = -5;        /* clamped, never wraps */
    web_hist_push(&h, &s);
    expect_hist(web_hist_at(&h, 1), "[0,null,null,null,null,null]");
    s = example();
    s.row.temp_c = 4000.0f;        /* tenths 40000 does not fit int16 */
    s.row.humidity_pct = -0.04f;   /* "0.0", never "-0.0" */
    s.uptime_s = (int64_t)UINT32_MAX + 10;
    web_hist_push(&h, &s);
    expect_hist(web_hist_at(&h, 2), "[4294967295,12,null,0.0,3.2100,\"NORMAL\"]");
    puts("PASS history entry text; empty -> null; int16/uint32 limits are not wrapped");

    web_hist_init(&h);
    for (unsigned i = 0; i < WEB_HIST_LEN + 7U; ++i) {
        s = example();
        s.uptime_s = (int64_t)i;
        web_hist_push(&h, &s);
    }
    assert(web_hist_count(&h) == WEB_HIST_LEN);
    for (unsigned i = 0; i < WEB_HIST_LEN; ++i) {
        assert(web_hist_at(&h, i)->uptime_s == 7U + i);   /* oldest first */
    }
    assert(web_hist_at(&h, WEB_HIST_LEN) == NULL);
    web_hist_push(NULL, &s);
    web_hist_push(&h, NULL);
    assert(web_hist_count(NULL) == 0 && web_hist_at(NULL, 0) == NULL);
    char buf[WEB_JSON_HIST_CAP];
    assert(web_json_hist_entry(NULL, buf, sizeof(buf)) == 0 && buf[0] == '\0');
    printf("PASS ring keeps the newest %u entries oldest-first; NULL arguments ignored\n",
           (unsigned)WEB_HIST_LEN);
    printf("     sizeof(web_hist_entry_t) on this host = %zu B (ESP32-S3 expected 20 B)\n",
           sizeof(web_hist_entry_t));
}

int main(void)
{
    test_example_and_empty();
    test_tenths_match_csv();
    test_strings_and_faults();
    test_bounds();
    test_history();
    puts("ALL web_json tests passed");
    return 0;
}
