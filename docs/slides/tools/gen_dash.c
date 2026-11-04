/* Sample data for the slide screenshots of the dashboard: the real
 * web_json.c / ag_mahal.c turn made-up rows into the exact live and
 * history JSON the board would send. Not measurements.
 * Build from firmware/: gcc -std=c11 -I test_host/stubs -I components/webui/include
 *   -I components/storage/include -I components/mahal/include -I components/mahal/model
 *   -I components/fsm/include ../docs/slides/tools/gen_dash.c components/webui/web_json.c
 *   components/storage/csv_row.c components/mahal/ag_mahal.c -lm
 * Run "gen_dash normal" or "gen_dash alarm"; the output is a <script> body that
 * stands in for /history.json and /ws in a copy of main/web/index.html. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "web_json.h"
#include "ag_mahal.h"

static void row_at(csv_sample_t *r, int i, int alarm, int n)
{
    memset(r, 0, sizeof *r);
    long t = 1793779200L + 13 * 3600 + i;          /* 2026-11-05 13:xx +07:00 */
    time_t tt = (time_t)t;
    struct tm *g = gmtime(&tt);
    r->local_time = *g;
    double w = sin(i / 37.0), v = cos(i / 23.0);
    r->has_pm = true;
    r->pm2_5 = (uint16_t)(20 + 4 * w + 2 * v);
    r->pm1_0 = (uint16_t)(r->pm2_5 * 0.6);
    r->pm10 = (uint16_t)(r->pm2_5 + 2);
    r->has_th = true;
    r->temp_c = (float)(32.6 + 0.25 * v);
    r->humidity_pct = (float)(56.8 + 0.9 * w);
    if (alarm && i > n - 160) {                      /* drier room for the last ~2.5 min */
        double k = (i - (n - 160)) / 60.0;
        if (k > 1) k = 1;
        r->humidity_pct = (float)(r->humidity_pct - 18.0 * k);
    }
    r->has_voc = true; r->tvoc_ppb = 213; r->eco2_ppm = 708;
    r->has_co = true; r->co_ppm = 0.5f;
}

int main(int argc, char **argv)
{
    const int alarm = argc > 1 && strcmp(argv[1], "alarm") == 0;
    const int n = 600;
    static web_hist_t h;
    web_hist_init(&h);
    ag_mahal_persist_t p;
    memset(&p, 0, sizeof p);
    web_snapshot_t s;
    unsigned streak_above = 0;
    for (int i = 0; i < n; ++i) {
        memset(&s, 0, sizeof s);
        row_at(&s.row, i, alarm, n);
        float x[AG_MAHAL_ROW_LEN];
        ag_mahal_row_from_sample(&s.row, x);
        const float d2 = ag_mahal_d2(x);
        const ag_ml_status_t st = ag_mahal_persist_step(&p, d2, (int64_t)i * 1000000);
        streak_above = d2 >= ag_mahal_threshold_d2() ? streak_above + 1 : 0;
        s.seq = 4000u + (unsigned)i;
        s.uptime_s = 3000 + i;
        s.time_valid = true;
        s.shown = streak_above >= 30 ? "ML ALARM" : streak_above > 0 ? "ML WATCH" : "NORMAL";
        s.ml.present = true;
        s.ml.status = ag_ml_status_name(st);
        s.ml.d2 = d2;
        s.ml.threshold_d2 = ag_mahal_threshold_d2();
        s.ml.streak = (uint16_t)streak_above;
        s.ml.model = ag_mahal_model_version();
        s.co.present = true; s.co.has_mean_1h = true; s.co.mean_1h_x10 = 5;
        web_hist_push(&h, &s);
    }
    char buf[WEB_JSON_LIVE_CAP], e[WEB_JSON_HIST_CAP];
    if (!web_json_live(&s, buf, sizeof buf)) return 1;
    printf("window.__L=%s;\nwindow.__H=[", buf);
    for (unsigned i = 0; i + 1 < web_hist_count(&h); ++i) {   /* last entry comes live */
        if (!web_json_hist_entry(web_hist_at(&h, i), e, sizeof e)) return 2;
        printf("%s%s", i ? "," : "", e);
    }
    printf("];\n");
    return 0;
}
