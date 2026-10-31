#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ag_mahal.h"
#include "csv_row.h"
/* The test reads the model constants and fixtures straight from the ML-3
 * header so it does not depend on ag_mahal.c to expose them. */
#include "ag_mahal_model.h"

static int s_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            ++s_failures;                                                    \
        }                                                                    \
    } while (0)

static bool same_float(float a, float b)
{
    if (isnan(a) || isnan(b)) {
        return isnan(a) && isnan(b);
    }
    return memcmp(&a, &b, sizeof(a)) == 0;
}

/* Independent reference: the header formula in double, written here
 * without looking at ag_mahal.c. NAN = missing. */
static double ref_d2(const float x[AG_MAHAL_ROW_LEN])
{
    double d[AG_MAHAL_N_FEATURES];
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        const double v = x[AG_MAHAL_NUMERIC_INDEX[j]];
        if (!isfinite(v)) {
            return NAN;
        }
        double t = v;
        if (AG_MAHAL_TRANSFORM[j] == AG_MAHAL_TRANSFORM_LOG1P) {
            if (v < 0.0) {
                return NAN;
            }
            t = log1p(v);
        }
        d[j] = (t - AG_MAHAL_MEAN[j]) / AG_MAHAL_STD[j] - AG_MAHAL_CENTER[j];
    }
    double acc = 0.0;
    for (unsigned i = 0; i < AG_MAHAL_N_FEATURES; ++i) {
        for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
            acc += d[i] * (double)AG_MAHAL_PRECISION[i][j] * d[j];
        }
    }
    return acc;
}

static bool is_used_column(unsigned col)
{
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        if (AG_MAHAL_NUMERIC_INDEX[j] == col) {
            return true;
        }
    }
    return false;
}

static void test_model_info(void)
{
    CHECK(strcmp(ag_mahal_model_version(), AG_MAHAL_MODEL_VERSION) == 0);
    CHECK(ag_mahal_n_features() == AG_MAHAL_N_FEATURES);
    CHECK(ag_mahal_threshold_d2() == AG_MAHAL_THRESHOLD_D2);
    CHECK(ag_mahal_persistence_k() == AG_MAHAL_PERSISTENCE_K);
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        CHECK(strcmp(ag_mahal_feature_name(j), AG_MAHAL_FEATURE_NAMES[j]) == 0);
    }
    CHECK(ag_mahal_feature_name(AG_MAHAL_N_FEATURES) == NULL);
    printf("model %s: %u features (", AG_MAHAL_MODEL_VERSION, ag_mahal_n_features());
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        printf("%s%s", j ? ", " : "", ag_mahal_feature_name(j));
    }
    printf(") threshold D2 %.7g K %u golden %d sequence %d\n",
           (double)ag_mahal_threshold_d2(), ag_mahal_persistence_k(),
           AG_MAHAL_N_GOLDEN, AG_MAHAL_N_SEQUENCE);
    puts("PASS model constants and feature names");
}

static void test_golden(void)
{
    double max_err = 0.0;
    double max_rel = 0.0;
    unsigned missing = 0;
    unsigned decisions = 0;
    for (unsigned g = 0; g < AG_MAHAL_N_GOLDEN; ++g) {
        const float ref = AG_MAHAL_GOLDEN_D2[g];
        const float got = ag_mahal_d2(AG_MAHAL_GOLDEN_INPUT[g]);
        if (isnan(ref)) {
            CHECK(isnan(got));
            ++missing;
            continue;
        }
        /* the header's tolerance, written out here too */
        const double err = fabs((double)got - (double)ref);
        const double lim = (double)AG_MAHAL_TOL_ABS + (double)AG_MAHAL_TOL_REL * fabs((double)ref);
        if (!(err <= lim)) {
            fprintf(stderr, "golden %u: got %.9g ref %.9g err %.3g > %.3g\n",
                    g, (double)got, (double)ref, err, lim);
        }
        CHECK(err <= lim);
        CHECK(ag_mahal_within_tol(got, ref));
        if (err > max_err) {
            max_err = err;
        }
        if (fabs((double)ref) > 1.0 && err / fabs((double)ref) > max_rel) {
            max_rel = err / fabs((double)ref);
        }
        if (!AG_MAHAL_GOLDEN_NEAR_THRESHOLD[g]) {
            CHECK((got >= AG_MAHAL_THRESHOLD_D2) == (AG_MAHAL_GOLDEN_ALARM_ROW[g] != 0));
            ++decisions;
        }
    }
    printf("golden: max |D2_c - D2_ref| = %.3g, max relative (|ref|>1) = %.3g, "
           "missing %u, decisions checked %u\n", max_err, max_rel, missing, decisions);
    puts("PASS golden vectors within the header tolerance, missing as required");
}

static void test_self_test(void)
{
    ag_mahal_self_test_t r;
    memset(&r, 0xA5, sizeof(r));
    CHECK(ag_mahal_self_test(&r));
    CHECK(r.params_ok);
    CHECK(r.golden_total == AG_MAHAL_N_GOLDEN);
    CHECK(r.golden_pass == AG_MAHAL_N_GOLDEN);
    CHECK(r.seq_total == AG_MAHAL_N_SEQUENCE);
    CHECK(r.seq_pass == AG_MAHAL_N_SEQUENCE);
    CHECK(r.first_golden_fail == -1);
    CHECK(r.first_seq_fail == -1);
    CHECK(isfinite(r.max_abs_err) && r.max_abs_err >= 0.0f);
    CHECK(ag_mahal_self_test(NULL));
    printf("self-test: golden %u/%u seq %u/%u max_abs_err %.3g\n", r.golden_pass,
           r.golden_total, r.seq_pass, r.seq_total, (double)r.max_abs_err);
    puts("PASS boot self-test reports every fixture passing");
}

static uint32_t s_lcg = 42U;
static float uniform(float lo, float hi)
{
    s_lcg = s_lcg * 1664525U + 1013904223U;   /* Numerical Recipes LCG */
    return lo + (hi - lo) * (float)(s_lcg >> 8) / (float)(1U << 24);
}

static void test_against_double_reference(void)
{
    const unsigned n = 20000;
    double max_ratio = 0.0;   /* err / allowed */
    for (unsigned k = 0; k < n; ++k) {
        float x[AG_MAHAL_ROW_LEN];
        x[0] = floorf(uniform(0.0f, 400.0f));
        x[1] = floorf(uniform(0.0f, 500.0f));
        x[2] = floorf(uniform(0.0f, 600.0f));
        x[3] = uniform(10.0f, 45.0f);
        x[4] = uniform(15.0f, 99.0f);
        x[5] = floorf(uniform(0.0f, 2000.0f));
        x[6] = floorf(uniform(400.0f, 2000.0f));
        x[7] = uniform(0.0f, 50.0f);
        const double ref = ref_d2(x);
        const float got = ag_mahal_d2(x);
        CHECK(isfinite(ref) && isfinite(got));
        const double allowed = (double)AG_MAHAL_TOL_ABS + (double)AG_MAHAL_TOL_REL * fabs(ref);
        const double ratio = fabs((double)got - ref) / allowed;
        if (ratio > max_ratio) {
            max_ratio = ratio;
        }
    }
    CHECK(max_ratio <= 1.0);
    printf("double reference: %u rows, worst error = %.3g x tolerance\n", n, max_ratio);
    puts("PASS float32 D2 matches a double reference over sensor-range rows");
}

static void test_missing(void)
{
    const float *base = AG_MAHAL_GOLDEN_INPUT[0];
    float x[AG_MAHAL_ROW_LEN];
    const float d0 = ag_mahal_d2(base);
    CHECK(isfinite(d0));

    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        const unsigned col = AG_MAHAL_NUMERIC_INDEX[j];
        const float bad[] = { NAN, INFINITY, -INFINITY };
        for (unsigned b = 0; b < 3; ++b) {
            memcpy(x, base, sizeof(x));
            x[col] = bad[b];
            CHECK(isnan(ag_mahal_d2(x)));
        }
        if (AG_MAHAL_TRANSFORM[j] == AG_MAHAL_TRANSFORM_LOG1P) {
            memcpy(x, base, sizeof(x));
            x[col] = -1.0f;
            CHECK(isnan(ag_mahal_d2(x)));
            x[col] = -1e-30f;      /* any negative input is rejected */
            CHECK(isnan(ag_mahal_d2(x)));
            x[col] = 0.0f;         /* zero dust is a valid reading */
            CHECK(isfinite(ag_mahal_d2(x)));
            x[col] = -0.0f;        /* -0 >= 0 in IEEE, as in NumPy */
            CHECK(isfinite(ag_mahal_d2(x)));
        } else {
            memcpy(x, base, sizeof(x));
            x[col] = -10.0f;       /* identity: negative T is a real value */
            CHECK(isfinite(ag_mahal_d2(x)));
        }
    }
    puts("PASS NaN / +inf / -inf in a used column, negative log1p input -> missing");

    /* unused columns may hold anything: the score must not change at all */
    unsigned unused = 0;
    for (unsigned col = 0; col < AG_MAHAL_ROW_LEN; ++col) {
        if (is_used_column(col)) {
            continue;
        }
        ++unused;
        const float junk[] = { NAN, INFINITY, -INFINITY, -1.0f, 1e30f };
        for (unsigned b = 0; b < 5; ++b) {
            memcpy(x, base, sizeof(x));
            x[col] = junk[b];
            CHECK(same_float(ag_mahal_d2(x), d0));
        }
    }
    for (unsigned col = 0; col < AG_MAHAL_ROW_LEN; ++col) {
        x[col] = is_used_column(col) ? base[col] : NAN;
    }
    CHECK(same_float(ag_mahal_d2(x), d0));
    CHECK(isnan(ag_mahal_d2(NULL)));
    printf("unused columns checked: %u\n", unused);
    puts("PASS unused columns (empty or junk) leave D2 bit-identical; NULL -> missing");

    /* far outside any sensor range: overflow is reported missing, not inf */
    memcpy(x, base, sizeof(x));
    unsigned identity = 0;
    for (unsigned j = 0; j < AG_MAHAL_N_FEATURES; ++j) {
        if (AG_MAHAL_TRANSFORM[j] == AG_MAHAL_TRANSFORM_IDENTITY) {
            x[AG_MAHAL_NUMERIC_INDEX[j]] = 3e38f;
            ++identity;
        }
    }
    if (identity > 0) {
        CHECK(isnan(ag_mahal_d2(x)));
        puts("PASS a non-finite D2 (float overflow) is reported missing");
    } else {
        puts("SKIP overflow case: model has no identity feature");
    }
}

/* Parse one csv_format_row output the way ml/load.py reads it:
 * empty field -> NAN, otherwise decimal -> double -> float32. */
static void parse_row(const char *row, float out[AG_MAHAL_ROW_LEN])
{
    const char *p = strchr(row, ',');   /* skip timestamp */
    assert(p != NULL);
    ++p;
    for (unsigned i = 0; i < AG_MAHAL_ROW_LEN; ++i) {
        const char *end = strchr(p, ',');
        assert(end != NULL);
        if (end == p) {
            out[i] = NAN;
        } else {
            char field[32];
            const size_t len = (size_t)(end - p);
            assert(len < sizeof(field));
            memcpy(field, p, len);
            field[len] = '\0';
            out[i] = (float)strtod(field, NULL);
        }
        p = end + 1;
    }
}

static csv_sample_t sample(float t, float rh, float co)
{
    csv_sample_t s;
    memset(&s, 0, sizeof(s));
    s.local_time.tm_year = 2026 - 1900;
    s.local_time.tm_mon = 9;
    s.local_time.tm_mday = 18;
    s.local_time.tm_hour = 12;
    s.has_pm = true;
    s.pm1_0 = 8;
    s.pm2_5 = 12;
    s.pm10 = 15;
    s.has_th = true;
    s.temp_c = t;
    s.humidity_pct = rh;
    s.has_voc = true;
    s.tvoc_ppb = 145;
    s.eco2_ppm = 520;
    s.has_co = true;
    s.co_ppm = co;
    return s;
}

static void expect_row_matches_csv(const csv_sample_t *s)
{
    char row[128];
    CHECK(csv_format_row(s, row, sizeof(row)) > 0);
    float from_csv[AG_MAHAL_ROW_LEN];
    float x[AG_MAHAL_ROW_LEN];
    parse_row(row, from_csv);
    ag_mahal_row_from_sample(s, x);
    for (unsigned i = 0; i < AG_MAHAL_ROW_LEN; ++i) {
        if (!same_float(x[i], from_csv[i])) {
            fprintf(stderr, "column %u: board %.9g csv %.9g (row %s)\n", i,
                    (double)x[i], (double)from_csv[i], row);
        }
        CHECK(same_float(x[i], from_csv[i]));
    }
    CHECK(same_float(ag_mahal_d2(x), ag_mahal_d2(from_csv)));
}

static void test_row_from_sample(void)
{
    /* rounding edges from test_csv_row plus ordinary values */
    const float temps[] = { 28.4f, 28.45f, 28.25f, 28.35f, 27.9f, -0.04f, -0.05f,
                            -12.25f, 99.95f, 99.96f, 26.0964718f };
    for (unsigned i = 0; i < sizeof(temps) / sizeof(temps[0]); ++i) {
        csv_sample_t s = sample(temps[i], 62.1f + (float)i * 0.37f, 0.4f + (float)i * 0.05f);
        expect_row_matches_csv(&s);
    }
    puts("PASS board input == the CSV row read back as float32 (T/RH/CO tenths)");

    /* the stored float is not used directly: 28.45f becomes 28.5 */
    float x[AG_MAHAL_ROW_LEN];
    csv_sample_t s = sample(28.45f, 60.04f, 0.0f);
    ag_mahal_row_from_sample(&s, x);
    CHECK(x[3] == 28.5f);
    CHECK(x[4] == 60.0f);
    CHECK(x[1] == 12.0f);

    /* every has_* flag off, one at a time and all together */
    s = sample(25.0f, 60.0f, 1.0f);
    s.has_pm = false;
    ag_mahal_row_from_sample(&s, x);
    CHECK(isnan(x[0]) && isnan(x[1]) && isnan(x[2]) && isfinite(x[3]));
    expect_row_matches_csv(&s);
    s = sample(25.0f, 60.0f, 1.0f);
    s.has_th = false;
    ag_mahal_row_from_sample(&s, x);
    CHECK(isnan(x[3]) && isnan(x[4]) && isfinite(x[1]));
    CHECK(isnan(ag_mahal_d2(x)));   /* pm_th needs T/RH */
    expect_row_matches_csv(&s);
    s = sample(25.0f, 60.0f, 1.0f);
    s.has_voc = false;
    s.has_co = false;               /* the board before FW-5b */
    ag_mahal_row_from_sample(&s, x);
    CHECK(isnan(x[5]) && isnan(x[6]) && isnan(x[7]));
    expect_row_matches_csv(&s);
    s = sample(NAN, 60.0f, INFINITY);   /* CSV writes these empty too */
    ag_mahal_row_from_sample(&s, x);
    CHECK(isnan(x[3]) && isfinite(x[4]) && isnan(x[7]));
    expect_row_matches_csv(&s);
    s = sample(25.0f, 60.0f, 1.0f);
    s.has_pm = s.has_th = s.has_voc = s.has_co = false;
    ag_mahal_row_from_sample(&s, x);
    for (unsigned i = 0; i < AG_MAHAL_ROW_LEN; ++i) {
        CHECK(isnan(x[i]));
    }
    CHECK(isnan(ag_mahal_d2(x)));
    ag_mahal_row_from_sample(NULL, x);
    for (unsigned i = 0; i < AG_MAHAL_ROW_LEN; ++i) {
        CHECK(isnan(x[i]));
    }
    ag_mahal_row_from_sample(&s, NULL);   /* must not crash */
    s = sample(25.0f, 60.0f, 1.0f);
    s.pm1_0 = s.pm2_5 = s.pm10 = 65535;
    s.tvoc_ppb = s.eco2_ppm = 0;
    expect_row_matches_csv(&s);
    puts("PASS empty groups -> NAN columns, uint16 limits, NULL arguments");
}

static void test_persistence(void)
{
    const unsigned k = AG_MAHAL_PERSISTENCE_K;
    const float hi = AG_MAHAL_THRESHOLD_D2 * 1.5f;
    const float lo = AG_MAHAL_THRESHOLD_D2 * 0.5f;
    const int64_t s1 = 1000000LL;

    /* the header's fixture, row by row */
    ag_mahal_persist_t p = { 0 };
    for (unsigned i = 0; i < AG_MAHAL_N_SEQUENCE; ++i) {
        const ag_ml_status_t st = ag_mahal_persist_step(&p, AG_MAHAL_SEQUENCE_D2[i], (int64_t)i * s1);
        CHECK((st == AG_ML_PERSISTENT) == (AG_MAHAL_SEQUENCE_ALARM[i] != 0));
        if (isnan(AG_MAHAL_SEQUENCE_D2[i])) {
            CHECK(st == AG_ML_MISSING && p.streak == 0);
        }
    }
    puts("PASS header persistence sequence (reset by low value and by missing)");

    /* exactly at the threshold counts as above (">=") */
    p = (ag_mahal_persist_t){ 0 };
    CHECK(ag_mahal_persist_step(&p, AG_MAHAL_THRESHOLD_D2, 0) ==
          (k == 1 ? AG_ML_PERSISTENT : AG_ML_ABOVE));
    CHECK(p.streak == 1);
    p = (ag_mahal_persist_t){ 0 };
    CHECK(ag_mahal_persist_step(&p, nextafterf(AG_MAHAL_THRESHOLD_D2, 0.0f), 0) == AG_ML_BELOW);

    /* ABOVE for the first K-1 rows, PERSISTENT from row K on */
    p = (ag_mahal_persist_t){ 0 };
    for (unsigned i = 1; i <= k + 2; ++i) {
        const ag_ml_status_t st = ag_mahal_persist_step(&p, hi, (int64_t)i * s1);
        CHECK(st == (i >= k ? AG_ML_PERSISTENT : AG_ML_ABOVE));
        CHECK(p.streak == i);
    }
    CHECK(ag_mahal_persist_step(&p, lo, (int64_t)(k + 3) * s1) == AG_ML_BELOW);
    CHECK(p.streak == 0);
    puts("PASS ABOVE before K, PERSISTENT from K, below resets; >= at threshold");

    /* gap rule: 1.5 s apart still consecutive, 1.5 s + 1 us is not */
    p = (ag_mahal_persist_t){ 0 };
    int64_t t = 0;
    for (unsigned i = 0; i + 1 < k; ++i) {
        ag_mahal_persist_step(&p, hi, t);
        t += s1;
    }
    t += AG_MAHAL_GAP_RESET_US - s1;   /* last step was at t - s1 */
    CHECK(ag_mahal_persist_step(&p, hi, t) == AG_ML_PERSISTENT);
    CHECK(p.streak == k);
    t += AG_MAHAL_GAP_RESET_US + 1;
    CHECK(ag_mahal_persist_step(&p, hi, t) == (k == 1 ? AG_ML_PERSISTENT : AG_ML_ABOVE));
    CHECK(p.streak == 1);
    /* time going backwards also restarts the count */
    ag_mahal_persist_step(&p, hi, t + s1);
    CHECK(p.streak == 2);
    ag_mahal_persist_step(&p, hi, t);
    CHECK(p.streak == 1);
    /* a gap followed by a missing / below row is still just a reset */
    CHECK(ag_mahal_persist_step(&p, NAN, t + 10 * s1) == AG_ML_MISSING);
    CHECK(p.streak == 0);
    puts("PASS gap > 1.5 s or backwards time restarts the streak; 1.5 s does not");

    /* the streak saturates instead of wrapping to 0 (18 h of 1 Hz rows) */
    p = (ag_mahal_persist_t){ 0 };
    for (uint32_t i = 0; i < 70000U; ++i) {
        const ag_ml_status_t st = ag_mahal_persist_step(&p, hi, (int64_t)i * s1);
        if (i + 1 >= k) {
            CHECK(st == AG_ML_PERSISTENT);
        }
    }
    CHECK(p.streak == UINT16_MAX);
    CHECK(ag_mahal_persist_step(NULL, hi, 0) == AG_ML_MISSING);
    CHECK(ag_mahal_persist_step(&p, INFINITY, 70000LL * s1) == AG_ML_MISSING);
    CHECK(p.streak == 0);
    puts("PASS streak saturates at UINT16_MAX; NULL and non-finite D2 -> missing");
}

static void test_tolerance_and_names(void)
{
    const float ref = 100.0f;
    const float lim = AG_MAHAL_TOL_ABS + AG_MAHAL_TOL_REL * ref;   /* 0.101 */
    CHECK(ag_mahal_within_tol(ref, ref));
    CHECK(ag_mahal_within_tol(ref + lim * 0.99f, ref));
    CHECK(!ag_mahal_within_tol(ref + lim * 1.01f, ref));
    CHECK(!ag_mahal_within_tol(ref - lim * 1.01f, ref));
    CHECK(ag_mahal_within_tol(NAN, NAN));
    CHECK(!ag_mahal_within_tol(1.0f, NAN));
    CHECK(!ag_mahal_within_tol(NAN, 1.0f));
    CHECK(!ag_mahal_within_tol(INFINITY, 1.0f));
    CHECK(strcmp(ag_ml_status_name(AG_ML_MODEL_FAULT), "model_fault") == 0);
    CHECK(strcmp(ag_ml_status_name(AG_ML_MISSING), "missing") == 0);
    CHECK(strcmp(ag_ml_status_name(AG_ML_BELOW), "below") == 0);
    CHECK(strcmp(ag_ml_status_name(AG_ML_ABOVE), "above") == 0);
    CHECK(strcmp(ag_ml_status_name(AG_ML_PERSISTENT), "persistent") == 0);
    CHECK(strcmp(ag_ml_status_name((ag_ml_status_t)99), "?") == 0);
    puts("PASS tolerance rule (incl. NAN / inf) and status names");
}

/* FW-7 step 3: per-vector check used for the board's failure log; must
 * agree with the self-test and report the header's reference. */
static void test_golden_check(void)
{
    CHECK(ag_mahal_golden_count() == AG_MAHAL_N_GOLDEN);
    for (unsigned g = 0; g < AG_MAHAL_N_GOLDEN; ++g) {
        float got = 0.0f;
        float ref = 0.0f;
        CHECK(ag_mahal_golden_check(g, &got, &ref));
        CHECK(same_float(ref, AG_MAHAL_GOLDEN_D2[g]));
        CHECK(same_float(got, ag_mahal_d2(AG_MAHAL_GOLDEN_INPUT[g])));
        CHECK(ag_mahal_golden_check(g, NULL, NULL));
    }
    float got = 1.0f;
    CHECK(!ag_mahal_golden_check(AG_MAHAL_N_GOLDEN, &got, NULL));
    CHECK(got == 1.0f);   /* untouched out of range */
    puts("PASS golden_check == self-test per vector; out of range -> false");
}

/* FW-7 step 3, D3-B: the AGML line pairs with the AGCSV row by its exact
 * timestamp text and carries D2 to 4 decimals. */
static void test_format_line(void)
{
    csv_sample_t s;
    memset(&s, 0, sizeof(s));
    s.local_time.tm_year = 2026 - 1900;
    s.local_time.tm_mon = 9;
    s.local_time.tm_mday = 24;
    s.local_time.tm_hour = 9;
    s.local_time.tm_min = 5;
    s.local_time.tm_sec = 7;
    s.has_pm = true;
    s.pm2_5 = 12;
    s.has_th = true;
    s.temp_c = 28.4f;
    s.humidity_pct = 62.1f;
    char row[128];
    CHECK(csv_format_row(&s, row, sizeof(row)) > 0);

    char line[96];
    ag_ml_result_t r = { .status = AG_ML_ABOVE, .d2 = 35.25f, .streak = 2, .at_us = 0 };
    size_t n = ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(strcmp(line, "2026-10-24T09:05:07+07:00,35.2500,above,2\n") == 0);
    CHECK(n == strlen(line));
    CHECK(strncmp(line, row, 26) == 0);   /* timestamp + comma, same text */

    r = (ag_ml_result_t){ .status = AG_ML_BELOW, .d2 = 0.0f, .streak = 0 };
    (void)ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(strcmp(line, "2026-10-24T09:05:07+07:00,0.0000,below,0\n") == 0);

    r = (ag_ml_result_t){ .status = AG_ML_MISSING, .d2 = NAN, .streak = 0 };
    (void)ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(strcmp(line, "2026-10-24T09:05:07+07:00,,missing,0\n") == 0);

    r = (ag_ml_result_t){ .status = AG_ML_MODEL_FAULT, .d2 = INFINITY, .streak = 0 };
    (void)ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(strcmp(line, "2026-10-24T09:05:07+07:00,,model_fault,0\n") == 0);

    r = (ag_ml_result_t){ .status = AG_ML_PERSISTENT, .d2 = 964.114075f,
                          .streak = UINT16_MAX };
    (void)ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(strcmp(line, "2026-10-24T09:05:07+07:00,964.1141,persistent,65535\n") == 0);

    /* the widest possible line (largest float, max streak) fits 96 bytes */
    r = (ag_ml_result_t){ .status = AG_ML_MODEL_FAULT, .d2 = -3.4028235e38f,
                          .streak = UINT16_MAX };
    n = ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(n > 0 && n < 96);
    printf("widest AGML line: %u chars\n", (unsigned)n);

    /* 4-decimal text read back is within the header tolerance of D2 */
    for (unsigned g = 0; g < AG_MAHAL_N_GOLDEN; ++g) {
        r = (ag_ml_result_t){ .status = AG_ML_BELOW, .d2 = AG_MAHAL_GOLDEN_D2[g] };
        (void)ag_mahal_format_line(row, &r, line, sizeof(line));
        const char *f = strchr(line, ',') + 1;
        if (isnan(AG_MAHAL_GOLDEN_D2[g])) {
            CHECK(*f == ',');
        } else {
            CHECK(ag_mahal_within_tol(strtof(f, NULL), AG_MAHAL_GOLDEN_D2[g]));
        }
    }

    /* invalid input / too small buffer: 0 and an empty buffer */
    r = (ag_ml_result_t){ .status = AG_ML_ABOVE, .d2 = 35.25f, .streak = 2 };
    const size_t full = ag_mahal_format_line(row, &r, line, sizeof(line));
    CHECK(ag_mahal_format_line(row, &r, line, full) == 0 && line[0] == '\0');
    CHECK(ag_mahal_format_line(row, &r, line, full + 1) == full);
    CHECK(ag_mahal_format_line(NULL, &r, line, sizeof(line)) == 0 && line[0] == '\0');
    CHECK(ag_mahal_format_line(row, NULL, line, sizeof(line)) == 0);
    CHECK(ag_mahal_format_line("no comma", &r, line, sizeof(line)) == 0);
    CHECK(ag_mahal_format_line(",x", &r, line, sizeof(line)) == 0);
    CHECK(ag_mahal_format_line(row, &r, NULL, sizeof(line)) == 0);
    CHECK(ag_mahal_format_line(row, &r, line, 0) == 0);
    puts("PASS AGML line: timestamp text of the row, D2 %.4f / empty, status, streak");
}

int main(void)
{
    test_model_info();
    test_golden();
    test_self_test();
    test_against_double_reference();
    test_missing();
    test_row_from_sample();
    test_persistence();
    test_tolerance_and_names();
    test_golden_check();
    test_format_line();
    if (s_failures != 0) {
        fprintf(stderr, "%d mahal check(s) failed\n", s_failures);
        return 1;
    }
    puts("ALL mahal tests passed");
    return 0;
}
