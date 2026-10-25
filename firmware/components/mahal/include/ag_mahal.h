#pragma once

/* FW-7: Mahalanobis anomaly score of one CSV §7 row, from the frozen ML-3
 * model (model/ag_mahal_model.h). Pure C: no FreeRTOS, no I/O, no malloc,
 * constant time per row, so the sampler can call it inside its 1 Hz cycle.
 *
 * Score (ml/BASELINE.md §3, docs/05-MULTIVARIATE.md §2):
 *   t_j = transform_j(x[index_j])      log1p for PM/TVOC (right tail,
 *                                      docs/05 §3.2), identity otherwise
 *   z_j = (t_j − mean_j) / std_j       mean/std from training only — never
 *                                      recomputed on the board, otherwise a
 *                                      slowly rising anomaly becomes "normal"
 *   d_j = z_j − center_j
 *   D²  = dᵀ P d,  P = Σ*⁻¹            Σ* = Ledoit–Wolf covariance of the
 *                                      training z (inverted once on the PC)
 * D² is the squared Mahalanobis distance (x−μ)ᵀΣ⁻¹(x−μ) written in the
 * standardized space; it is kept squared (no sqrt) exactly as in Python.
 *
 * 🔴 The CO alarm path must not read or wait for anything here
 * (CLAUDE.md §2): this score is advisory input for the FSM (FW-6) only. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "csv_row.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Numeric columns of one CSV §7 row, in §7 order:
 * 0 pm1_0, 1 pm2_5, 2 pm10, 3 temp_c, 4 humidity_pct, 5 tvoc_ppb,
 * 6 eco2_ppm, 7 co_ppm. NAN = empty field. */
#define AG_MAHAL_ROW_LEN 8U

/* Persistence counts consecutive 1 Hz samples; two scored samples further
 * apart than this (esp_timer, monotonic) are not consecutive and the count
 * restarts. 1.5 s = halfway between one period (1 s) and a skipped one
 * (2 s): catches overruns that arrive late and the loop re-align (up to
 * 2 s), ignores wall-clock steps (SNTP). Firmware policy agreed in FW-7
 * step 1 (D4), not a datasheet value. */
#define AG_MAHAL_GAP_RESET_US 1500000LL

/* What the FSM (FW-6) gets. FW-7 does not map these to WATCH/ALARM. */
typedef enum {
    AG_ML_MODEL_FAULT,  /* boot self-test failed: never scored until reboot */
    AG_ML_MISSING,      /* a selected feature is empty/non-finite, or a log1p
                           input is < 0: no imputation, count reset */
    AG_ML_BELOW,        /* D² < threshold: count reset */
    AG_ML_ABOVE,        /* D² >= threshold, fewer than K in a row so far */
    AG_ML_PERSISTENT,   /* D² >= threshold on >= K consecutive samples
                           (the "alarm" of the ML-3 header) */
} ag_ml_status_t;

/* One scored sample, as published to FW-6 (FW-7 step 3). */
typedef struct {
    ag_ml_status_t status;
    float d2;           /* NAN for MISSING / MODEL_FAULT */
    uint16_t streak;    /* consecutive samples with D² >= threshold */
    int64_t at_us;      /* esp_timer time of the sample, to detect a stale result */
} ag_ml_result_t;

/* Persistence counter state; zero-initialised = no sample yet. */
typedef struct {
    bool has_prev;
    int64_t prev_us;
    uint16_t streak;    /* saturates at UINT16_MAX, never wraps to 0 */
} ag_mahal_persist_t;

typedef struct {
    bool params_ok;         /* indices < 8, known transforms, finite params,
                               std > 0, symmetric precision, threshold > 0 */
    unsigned golden_total;
    unsigned golden_pass;   /* D² within tolerance (or missing as required)
                               and row decision right where it is checked */
    unsigned seq_total;
    unsigned seq_pass;      /* persistence fixture rows that match */
    int first_golden_fail;  /* index, -1 = none */
    int first_seq_fail;     /* index, -1 = none */
    float max_abs_err;      /* largest |D²_c − D²_ref| over non-missing golden */
} ag_mahal_self_test_t;

/* Model constants (from the header) for logs. */
const char *ag_mahal_model_version(void);
unsigned ag_mahal_n_features(void);
float ag_mahal_threshold_d2(void);
unsigned ag_mahal_persistence_k(void);
/* CSV column name of feature j (0 .. n_features-1), NULL out of range. */
const char *ag_mahal_feature_name(unsigned j);

/* Fill x[] from the row the logger writes. temp_c / humidity_pct / co_ppm
 * go through csv_round_tenths — the same rounding as the CSV field — and
 * become (float)(tenths / 10.0), which is what Python gets reading that
 * field (decimal → float64 → float32). So the board scores exactly the
 * values in the CSV, and a replay of the CSV reproduces the board's D².
 * has_* == false or a field the CSV would leave empty → NAN. */
void ag_mahal_row_from_sample(const csv_sample_t *s, float x[AG_MAHAL_ROW_LEN]);

/* D² in float32, or NAN when the row is missing (see AG_ML_MISSING).
 * Columns the model does not use may hold anything, NAN included. A
 * non-finite result (only possible far outside every sensor's range) is
 * also NAN. x == NULL → NAN. */
float ag_mahal_d2(const float x[AG_MAHAL_ROW_LEN]);

/* Compare a C result with a reference D² the way the ML-3 header says:
 * |got − ref| <= TOL_ABS + TOL_REL·|ref|; a NAN reference needs NAN. */
bool ag_mahal_within_tol(float got, float ref);

/* Advance the persistence counter with one sample taken at now_us
 * (esp_timer µs). d2 = NAN means missing. Before the sample is counted the
 * streak restarts when the previous sample is more than
 * AG_MAHAL_GAP_RESET_US older or now_us went backwards. Then:
 * missing → 0 (MISSING) · D² < threshold → 0 (BELOW) ·
 * D² >= threshold → +1, PERSISTENT when streak >= K, else ABOVE.
 * p == NULL → MISSING. */
ag_ml_status_t ag_mahal_persist_step(ag_mahal_persist_t *p, float d2, int64_t now_us);

/* Golden vectors + persistence fixture of the model header, run with this
 * build's arithmetic (FW-7 step 3 runs it on the ESP32-S3 at boot).
 * Fills *out when non-NULL. true = everything passed. */
bool ag_mahal_self_test(ag_mahal_self_test_t *out);

/* One golden vector of the model header (g < ag_mahal_golden_count()):
 * *got = this build's D², *ref = the header's (either may be NULL). true =
 * the same check as the self-test (tolerance, missing, row decision).
 * Used to log the vectors that fail on the board. */
bool ag_mahal_golden_check(unsigned g, float *got, float *ref);
unsigned ag_mahal_golden_count(void);

/* "missing", "below", ... for logs. */
const char *ag_ml_status_name(ag_ml_status_t status);

/* FW-7 step 3, D3-B: the per-row score line for the serial console,
 * printed by the storage task as "AGML,<this>" right after the row's
 * "AGCSV," line. Format (4 fields, no header on the board):
 *   timestamp_iso,d2,status,streak\n

 * - timestamp_iso: copied from the first field of csv_row (the text
 *   csv_format_row made for the same row), so a replay pairs the two lines
 *   by identical text.
 * - d2: "%.4f", empty when not finite (missing / model_fault).
 * - status: ag_ml_status_name(); streak: decimal.
 * Not part of the CSV contract (CLAUDE.md §7): serial_logger.py keeps only
 * AGCSV/AGEV lines in the CSV files and leaves AGML in serial_*.log.
 * Returns the length (without NUL), 0 when an argument is invalid, csv_row
 * has no timestamp field or buf is too small (buf[0] = '\0' when cap > 0).
 * 96 bytes always fit a real row. */
size_t ag_mahal_format_line(const char *csv_row, const ag_ml_result_t *r,
                            char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
