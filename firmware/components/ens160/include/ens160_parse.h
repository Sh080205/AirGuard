#pragma once

/* FW-5a: pure C helpers for the ENS160 (no I2C, no FreeRTOS) so
 * firmware/test_host can compile them with gcc.
 *
 * Source for every number: ScioSense ENS160 Datasheet Version 1.3,
 * SC-001224-DS-9, 2023-03-29, Production ("DS" below). Rev 0.95 is
 * pre-production and is NOT used.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- register map: DS §16.1 Table 16 ---- */
#define ENS160_REG_PART_ID        0x00  /* 2 B, little endian */
#define ENS160_REG_OPMODE         0x10  /* 1 B, R/W */
#define ENS160_REG_TEMP_IN        0x13  /* 2 B, R/W */
#define ENS160_REG_RH_IN          0x15  /* 2 B, R/W */
#define ENS160_REG_DEVICE_STATUS  0x20  /* 1 B */
#define ENS160_REG_DATA_AQI       0x21  /* 1 B */
#define ENS160_REG_DATA_TVOC      0x22  /* 2 B, ppb */
#define ENS160_REG_DATA_ECO2      0x24  /* 2 B, ppm */
#define ENS160_REG_DATA_T         0x30  /* 2 B, temperature used */
#define ENS160_REG_DATA_RH        0x32  /* 2 B, humidity used */
#define ENS160_REG_DATA_MISR      0x38  /* 1 B, checksum of DATA_* reads */

/* DS §16.2.1 Table 17: PART_ID LSB 0x60, MSB 0x01. */
#define ENS160_PART_ID            0x0160u

/* DS §16.2.2 Table 18 */
#define ENS160_OPMODE_DEEP_SLEEP  0x00
#define ENS160_OPMODE_IDLE        0x01
#define ENS160_OPMODE_STANDARD    0x02

/* DS §16.2.7 Table 26: DEVICE_STATUS bits */
#define ENS160_STATUS_STATAS      0x80u  /* an OPMODE is running */
#define ENS160_STATUS_STATER      0x40u  /* error detected */
#define ENS160_STATUS_VALIDITY_SHIFT 2   /* bits 3:2 */
#define ENS160_STATUS_VALIDITY_MASK  0x0Cu
#define ENS160_STATUS_NEWDAT      0x02u  /* new DATA_x, cleared on DATA read */
#define ENS160_STATUS_NEWGPR      0x01u

/* DS §11 Table 10 / Table 26: validity flag */
#define ENS160_VALIDITY_NORMAL    0  /* operating ok */
#define ENS160_VALIDITY_WARMUP    1  /* first 3 min after power-on (§10.2) */
#define ENS160_VALIDITY_STARTUP   2  /* first hour of the sensor's life (§10.1) */
#define ENS160_VALIDITY_INVALID   3  /* no valid output */

/* DS §5 Table 4: output ranges. */
#define ENS160_TVOC_MAX_PPB       65000u
#define ENS160_ECO2_MIN_PPM       400u
#define ENS160_ECO2_MAX_PPM       65000u

/* DS p. 2 "Wide operating ranges": temperature -40..+85 °C. Compensation is
 * only written inside this range; RH is accepted for 0..100 %. */
#define ENS160_COMP_T_MIN_C       (-40.0f)
#define ENS160_COMP_T_MAX_C       85.0f

/* One burst read from DEVICE_STATUS: STATUS, AQI, TVOC LSB/MSB,
 * ECO2 LSB/MSB (0x20..0x25). Auto-increment read, DS §14.1.1 / Figure 9. */
#define ENS160_DATA_LEN           6

typedef struct {
    uint8_t status;      /* raw DEVICE_STATUS */
    uint8_t validity;    /* 0..3, bits 3:2 */
    bool running;        /* STATAS */
    bool error;          /* STATER */
    bool new_data;       /* NEWDAT */
    uint8_t aqi;         /* AQI-UBA, bits 0:2 (DS Table 27), not logged in CSV */
    uint16_t tvoc_ppb;
    uint16_t eco2_ppm;
} ens160_data_t;

/* Why a reading cannot fill the CSV columns (first failing check wins,
 * in this order). */
typedef enum {
    ENS160_VERDICT_OK,
    ENS160_VERDICT_ERROR,        /* STATER = 1 */
    ENS160_VERDICT_NOT_RUNNING,  /* STATAS = 0: sensor not in an OPMODE (e.g. reset to IDLE) */
    ENS160_VERDICT_WARMUP,       /* validity 1 */
    ENS160_VERDICT_STARTUP,      /* validity 2 */
    ENS160_VERDICT_INVALID,      /* validity 3 */
    ENS160_VERDICT_RANGE,        /* TVOC/eCO2 outside DS Table 4 */
} ens160_verdict_t;

/* What the sampler does after a reading (P7, bring-up 16 Oct). */
typedef enum {
    ENS160_ACTION_NONE,     /* keep going */
    ENS160_ACTION_WAIT,     /* STATAS = 0 but OPMODE still reads STANDARD:
                             * the mode is starting, read again next cycle */
    ENS160_ACTION_RESTART,  /* run the start-up sequence again */
} ens160_action_t;

/* On the board, 1 s after OPMODE STANDARD was written and read back as
 * 0x02, DEVICE_STATUS was 0x03 (STATAS = 0); the DS gives no time for
 * STATAS to rise. NOT_RUNNING alone therefore does not prove the sensor
 * left STANDARD, so OPMODE (DS Table 18, R/W) decides:
 *   0x02 -> WAIT (no guessed delay; the row stays empty meanwhile)
 *   any other value, or the OPMODE read failed -> RESTART
 * (e.g. the sensor lost power and came back in IDLE 0x01).
 * STATER -> RESTART. Every other verdict -> NONE.
 * opmode is ignored unless opmode_ok.
 *
 * Cap on WAIT (review 17 Oct, item 1): *wait_streak counts consecutive
 * WAIT results. The ENS160_WAIT_MAX_CYCLES-th one in a row returns RESTART
 * instead, so a sensor stuck with STATAS = 0 and OPMODE 0x02 cannot leave
 * tvoc/eco2 empty for days during unattended logging. Every non-WAIT
 * result (including that RESTART) sets *wait_streak to 0. Readings that
 * fail (I2C/MISR) never reach this function and leave the streak as is.
 * wait_streak NULL -> NOT_RUNNING always RESTARTs (no unbounded wait). */
ens160_action_t ens160_status_action(ens160_verdict_t verdict, bool opmode_ok, uint8_t opmode,
                                     uint32_t *wait_streak);

/* 60 consecutive WAIT cycles of the 1 Hz sampler ≈ 60 s. Firmware policy,
 * not a datasheet value (DS v1.3 gives no time for STATAS to rise): on the
 * board STATAS rose within 1-2 s both times (bring-up 16-17 Oct), so 60 s
 * leaves ~30x margin. A forced restart starts a new warm-up (DS §10.2
 * "after idle periods"), which beats an empty column. */
#define ENS160_WAIT_MAX_CYCLES 60u

/* PART_ID bytes as read from 0x00/0x01 (little endian) -> 16-bit value. */
uint16_t ens160_u16le(const uint8_t b[2]);

/* Decode the 6-byte burst (see ENS160_DATA_LEN). NULL -> INVALID_ARG,
 * *out untouched. */
esp_err_t ens160_parse_data(const uint8_t raw[ENS160_DATA_LEN], ens160_data_t *out);

/* Status + range checks above. NULL -> ENS160_VERDICT_ERROR. */
ens160_verdict_t ens160_verdict(const ens160_data_t *d);

/* TEMP_IN format, DS §16.2.5 Table 23: Kelvin * 64, Kelvin = °C + 273.15,
 * rounded to nearest (DS example 25 °C -> 0x4A8A = round(19081.6)).
 * NaN or outside ENS160_COMP_T_MIN_C..MAX_C -> INVALID_ARG, *out untouched. */
esp_err_t ens160_encode_temp(float temp_c, uint16_t *out);

/* RH_IN format, DS §16.2.6 Table 25: %RH * 512, rounded to nearest
 * (DS example 50 % -> 0x6400). NaN or outside 0..100 -> INVALID_ARG. */
esp_err_t ens160_encode_rh(float rh_pct, uint16_t *out);

/* DATA_T / DATA_RH back to °C / %RH (DS §16.2.12-13: raw/64 - 273.15,
 * raw/512). For logging only. */
float ens160_decode_temp(uint16_t raw);
float ens160_decode_rh(uint16_t raw);

/* DATA_MISR mirror, DS §16.2.14: for every byte read from 0x20..0x37,
 * misr = (misr << 1) ^ data, and when the old misr had bit 7 set the result
 * is XORed with POLY 0x1D (x^8+x^4+x^3+x^2+1).
 * NOTE: the DS example code writes `if( misr&0x80==0 )`, which C parses as
 * misr & (0x80==0) == 0 (always false). The comment and polynomial show the
 * intent is "MSB of misr is 0"; that is what is implemented here.
 * [ต้องวัดจริง: ตัวนับ misr_err บนบอร์ดต้องเป็น 0] */
#define ENS160_MISR_POLY 0x1Du
uint8_t ens160_misr_update(uint8_t misr, uint8_t data);
uint8_t ens160_misr_bytes(uint8_t misr, const uint8_t *data, size_t len);

/* ---- hold the last good reading for the CSV row ----
 * DS v1.3 gives NO data rate for STANDARD mode (only T63 = 1 s, §10
 * Table 9), so a row may come between two NEWDAT updates. Rule (same idea as
 * the PM columns, CLAUDE.md §7 note 4 Oct): reuse the last good reading
 * while it is at most ENS160_MAX_AGE_MS old, counted from the cycle that saw
 * NEWDAT; older -> empty.
 * 2500 ms = same bound as PMS_MAX_AGE_MS: a reading seen in cycle n fills
 * rows n, n+1, n+2 and never n+3; the bound sits mid-period so scheduling
 * jitter cannot flip it. This is a firmware policy, not a datasheet value.
 * [ต้องวัดจริง: คาบ NEWDAT — log "newdat gap" ทุก 60 s]
 *
 * Any non-OK verdict (warm-up, start-up, invalid, error, not running, out of
 * range) drops the held reading at once: values from before a warm-up must
 * not leak into rows after it. A failed read (I2C error / MISR mismatch)
 * says nothing about the sensor, so the held reading is kept, age-limited. */
#define ENS160_MAX_AGE_MS 2500
#define ENS160_MAX_AGE_US ((int64_t)ENS160_MAX_AGE_MS * 1000)

typedef struct {
    bool have;
    uint16_t tvoc_ppb;
    uint16_t eco2_ppm;
    int64_t data_us;    /* esp_timer time of the cycle that saw NEWDAT */
} ens160_hold_t;

typedef enum {
    ENS160_FILL_EMPTY,
    ENS160_FILL_NEW,      /* this cycle's NEWDAT reading */
    ENS160_FILL_REUSED,   /* held reading, still <= max_age_us old */
} ens160_fill_t;

/* One sampler cycle. read_ok == false: d is ignored (may be NULL).
 * On FILL_NEW / FILL_REUSED writes *tvoc_ppb / *eco2_ppm; on FILL_EMPTY
 * leaves them untouched. NULL h or outputs -> FILL_EMPTY.
 * Age check: 0 <= now_us - data_us <= max_age_us (logutil_frame_fresh). */
ens160_fill_t ens160_hold_step(ens160_hold_t *h, bool read_ok, const ens160_data_t *d,
                               int64_t now_us, int64_t max_age_us,
                               uint16_t *tvoc_ppb, uint16_t *eco2_ppm);

#ifdef __cplusplus
}
#endif
