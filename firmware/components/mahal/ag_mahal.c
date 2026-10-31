#include "ag_mahal.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The only file that includes the model: its arrays are static. */
#include "ag_mahal_model.h"

_Static_assert(AG_MAHAL_N_NUMERIC == AG_MAHAL_ROW_LEN,
               "model row layout must be the 8 numeric CSV §7 columns");
_Static_assert(AG_MAHAL_N_FEATURES >= 1 && AG_MAHAL_N_FEATURES <= AG_MAHAL_ROW_LEN,
               "1..8 features");
_Static_assert(AG_MAHAL_PERSISTENCE_K >= 1 && AG_MAHAL_PERSISTENCE_K <= UINT16_MAX,
               "K must fit the uint16 streak");
_Static_assert(AG_MAHAL_N_GOLDEN >= 1 && AG_MAHAL_N_SEQUENCE >= 1,
               "header must carry fixtures for the self-test");

const char *ag_mahal_model_version(void)
{
    return AG_MAHAL_MODEL_VERSION;
}

unsigned ag_mahal_n_features(void)
{
    return AG_MAHAL_N_FEATURES;
}

float ag_mahal_threshold_d2(void)
{
    return AG_MAHAL_THRESHOLD_D2;
}

unsigned ag_mahal_persistence_k(void)
{
    return AG_MAHAL_PERSISTENCE_K;
}

const char *ag_mahal_feature_name(unsigned j)
{
    return j < AG_MAHAL_N_FEATURES ? AG_MAHAL_FEATURE_NAMES[j] : NULL;
}

/* Tenths → the float Python reads from the CSV field: the double division
 * of two exact values is the nearest double to the decimal (same as strtod),
 * then rounded to float32 as ml/baseline.py does for the golden rows. */
static float tenths_field(bool present, float v)
{
    long t = 0;
    if (!present || !csv_round_tenths(v, &t)) {
        return NAN;
    }
    return (float)((double)t / 10.0);
}

void ag_mahal_row_from_sample(const csv_sample_t *s, float x[AG_MAHAL_ROW_LEN])
{
    if (x == NULL) {
        return;
    }
    for (unsigned i = 0; i < AG_MAHAL_ROW_LEN; ++i) {
        x[i] = NAN;
    }
    if (s == NULL) {
        return;
    }
    if (s->has_pm) {
        x[0] = (float)s->pm1_0;
        x[1] = (float)s->pm2_5;
        x[2] = (float)s->pm10;
    }
    x[3] = tenths_field(s->has_th, s->temp_c);
    x[4] = tenths_field(s->has_th, s->humidity_pct);
    if (s->has_voc) {
        x[5] = (float)s->tvoc_ppb;
        x[6] = (float)s->eco2_ppm;
    }
    x[7] = tenths_field(s->has_co, s->co_ppm);
}

float ag_mahal_d2(const float x[AG_MAHAL_ROW_LEN])
{
    if (x == NULL) {
        return NAN;
    }
    float d[AG_MAHAL_N_FEATURES];
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        const unsigned idx = AG_MAHAL_NUMERIC_INDEX[j];
        if (idx >= AG_MAHAL_ROW_LEN) {
            return NAN;
        }
        const float v = x[idx];
        /* missing policy (BASELINE §2): no imputation, no zero */
        if (!isfinite(v)) {
            return NAN;
        }
        float t;
        switch (AG_MAHAL_TRANSFORM[j]) {
        case AG_MAHAL_TRANSFORM_LOG1P:
            if (v < 0.0f) {
                return NAN;
            }
            t = log1pf(v);
            break;
        case AG_MAHAL_TRANSFORM_IDENTITY:
            t = v;
            break;
        default:
            return NAN;
        }
        d[j] = (t - AG_MAHAL_MEAN[j]) / AG_MAHAL_STD[j] - AG_MAHAL_CENTER[j];
    }
    /* the order of the header comment: sum_i sum_j d_i * P[i][j] * d_j */
    float d2 = 0.0f;
    for (unsigned i = 0; i < AG_MAHAL_N_FEATURES; ++i) {
        for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
            d2 += d[i] * AG_MAHAL_PRECISION[i][j] * d[j];
        }
    }
    return isfinite(d2) ? d2 : NAN;
}

bool ag_mahal_within_tol(float got, float ref)
{
    if (isnan(ref)) {
        return isnan(got);
    }
    if (!isfinite(got) || !isfinite(ref)) {
        return false;
    }
    return fabsf(got - ref) <= AG_MAHAL_TOL_ABS + AG_MAHAL_TOL_REL * fabsf(ref);
}

ag_ml_status_t ag_mahal_persist_step(ag_mahal_persist_t *p, float d2, int64_t now_us)
{
    if (p == NULL) {
        return AG_ML_MISSING;
    }
    if (p->has_prev &&
        (now_us < p->prev_us || now_us - p->prev_us > AG_MAHAL_GAP_RESET_US)) {
        p->streak = 0;   /* not consecutive: a skipped sample resets */
    }
    p->has_prev = true;
    p->prev_us = now_us;

    if (!isfinite(d2)) {
        p->streak = 0;
        return AG_ML_MISSING;
    }
    /* ">=": the header's "alarm iff D2 >= THRESHOLD_D2" */
    if (!(d2 >= AG_MAHAL_THRESHOLD_D2)) {
        p->streak = 0;
        return AG_ML_BELOW;
    }
    if (p->streak < UINT16_MAX) {
        ++p->streak;
    }
    return p->streak >= AG_MAHAL_PERSISTENCE_K ? AG_ML_PERSISTENT : AG_ML_ABOVE;
}

static bool params_ok(void)
{
    if (!isfinite(AG_MAHAL_THRESHOLD_D2) || !(AG_MAHAL_THRESHOLD_D2 > 0.0f)) {
        return false;
    }
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        if (AG_MAHAL_NUMERIC_INDEX[j] >= AG_MAHAL_ROW_LEN) {
            return false;
        }
        /* features in §7 order, each column once (BASELINE §1) */
        if (j > 0 && AG_MAHAL_NUMERIC_INDEX[j] <= AG_MAHAL_NUMERIC_INDEX[j - 1]) {
            return false;
        }
        if (AG_MAHAL_TRANSFORM[j] != AG_MAHAL_TRANSFORM_IDENTITY &&
            AG_MAHAL_TRANSFORM[j] != AG_MAHAL_TRANSFORM_LOG1P) {
            return false;
        }
        if (!isfinite(AG_MAHAL_MEAN[j]) || !isfinite(AG_MAHAL_CENTER[j]) ||
            !isfinite(AG_MAHAL_STD[j]) || !(AG_MAHAL_STD[j] > 0.0f)) {
            return false;
        }
        for (unsigned i = 0; i < AG_MAHAL_N_FEATURES; ++i) {
            if (!isfinite(AG_MAHAL_PRECISION[j][i]) ||
                AG_MAHAL_PRECISION[j][i] != AG_MAHAL_PRECISION[i][j]) {
                return false;
            }
        }
        /* diagonal of a positive definite matrix is > 0 */
        if (!(AG_MAHAL_PRECISION[j][j] > 0.0f)) {
            return false;
        }
    }
    return true;
}

bool ag_mahal_golden_check(unsigned g, float *got, float *ref)
{
    if (g >= AG_MAHAL_N_GOLDEN) {
        return false;
    }
    const float r = AG_MAHAL_GOLDEN_D2[g];
    const float c = ag_mahal_d2(AG_MAHAL_GOLDEN_INPUT[g]);
    if (got != NULL) {
        *got = c;
    }
    if (ref != NULL) {
        *ref = r;
    }
    bool ok = ag_mahal_within_tol(c, r);
    /* row decision, skipped where the reference sits on the threshold */
    if (ok && !isnan(r) && !AG_MAHAL_GOLDEN_NEAR_THRESHOLD[g]) {
        const bool above = c >= AG_MAHAL_THRESHOLD_D2;
        ok = above == (AG_MAHAL_GOLDEN_ALARM_ROW[g] != 0);
    }
    return ok;
}

unsigned ag_mahal_golden_count(void)
{
    return AG_MAHAL_N_GOLDEN;
}

bool ag_mahal_self_test(ag_mahal_self_test_t *out)
{
    ag_mahal_self_test_t r = {
        .params_ok = params_ok(),
        .golden_total = AG_MAHAL_N_GOLDEN,
        .seq_total = AG_MAHAL_N_SEQUENCE,
        .first_golden_fail = -1,
        .first_seq_fail = -1,
        .max_abs_err = 0.0f,
    };

    for (unsigned g = 0; g < AG_MAHAL_N_GOLDEN; ++g) {
        float got = NAN;
        float ref = NAN;
        const bool ok = ag_mahal_golden_check(g, &got, &ref);
        if (ok && !isnan(ref)) {
            const float err = fabsf(got - ref);
            if (err > r.max_abs_err) {
                r.max_abs_err = err;
            }
        }
        if (ok) {
            ++r.golden_pass;
        } else if (r.first_golden_fail < 0) {
            r.first_golden_fail = (int)g;
        }
    }

    /* fixture rows are consecutive 1 Hz samples */
    ag_mahal_persist_t p = { 0 };
    for (unsigned i = 0; i < AG_MAHAL_N_SEQUENCE; ++i) {
        const ag_ml_status_t st =
            ag_mahal_persist_step(&p, AG_MAHAL_SEQUENCE_D2[i], (int64_t)i * 1000000LL);
        const bool alarm = st == AG_ML_PERSISTENT;
        if (alarm == (AG_MAHAL_SEQUENCE_ALARM[i] != 0)) {
            ++r.seq_pass;
        } else if (r.first_seq_fail < 0) {
            r.first_seq_fail = (int)i;
        }
    }

    if (out != NULL) {
        *out = r;
    }
    return r.params_ok && r.golden_pass == r.golden_total && r.seq_pass == r.seq_total;
}

const char *ag_ml_status_name(ag_ml_status_t status)
{
    switch (status) {
    case AG_ML_MODEL_FAULT: return "model_fault";
    case AG_ML_MISSING: return "missing";
    case AG_ML_BELOW: return "below";
    case AG_ML_ABOVE: return "above";
    case AG_ML_PERSISTENT: return "persistent";
    default: return "?";
    }
}

size_t ag_mahal_format_line(const char *csv_row, const ag_ml_result_t *r,
                            char *buf, size_t cap)
{
    if (buf != NULL && cap > 0) {
        buf[0] = '\0';
    }
    if (csv_row == NULL || r == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    /* timestamp = the row's first field, so AGML and AGCSV pair by text */
    const char *comma = strchr(csv_row, ',');
    if (comma == NULL || comma == csv_row) {
        return 0;
    }
    const int ts_len = (int)(comma - csv_row);
    char d2[48] = "";
    if (isfinite(r->d2)) {
        /* 4 decimals: 5e-5 rounding, far below the header tolerance
         * (TOL_ABS 1e-3), so a replay can compare against Python */
        if (snprintf(d2, sizeof(d2), "%.4f", (double)r->d2) >= (int)sizeof(d2)) {
            return 0;
        }
    }
    const int n = snprintf(buf, cap, "%.*s,%s,%s,%u\n", ts_len, csv_row, d2,
                           ag_ml_status_name(r->status), (unsigned)r->streak);
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
