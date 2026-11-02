#pragma once

/* I2C bus recovery at boot (docs/tasks/reports/I2C-bus-recovery-proposal.md
 * §4-§5, scope approved by the owner 2 Nov 2026: D1 + D2 + R1 + a boot-time
 * i2c_master_bus_reset() when a probe fails; no recovery while running).
 *
 * Pure C: the pins are reached only through the callbacks in
 * i2c_recover_io_t, so firmware/test_host runs the same sequence against a
 * simulated slave. No FreeRTOS, no malloc, no float. The ESP-IDF side
 * (gpio_config, gpio_get_level, esp_rom_delay_us, i2c_master_probe) is in
 * main/i2c_boot.c.
 *
 * Bus clear (R1) follows the software clear of ESP-IDF v5.5.5
 * (esp_driver_i2c/i2c_master.c s_i2c_master_clear_bus, used on chips
 * without a hardware clear) and the "Bus clear" section of the I2C spec
 * [ต้องเปิด datasheet: NXP UM10204 Rev.7 §3.1.16 ยืนยันเลขหัวข้อ]:
 *   1. both lines released (open-drain, 4.7 kΩ board pull-ups, CLAUDE.md §4)
 *   2. SCL low: nothing to do from here (a slave holds the clock; 9 clocks
 *      cannot help, proposal §4.2 case B) -> report and stop
 *   3. SDA low: pulse SCL until SDA is released, at most 9 times
 *      (a slave stuck inside a byte has <= 8 data bits + 1 ACK left)
 *   4. STOP: SDA low while SCL low, SCL high, then SDA high
 * Only ever "low" or "released" is written: a line is never driven high,
 * so a slave pulling low never fights an output (pin limit ~20 mA,
 * CLAUDE.md §5). */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum SCL pulses of the bus clear (see above). */
#define I2C_RECOVER_MAX_CLOCKS 9U
/* After releasing SCL, wait this many half periods for it to read high
 * (a slave may stretch the clock) before calling the bus "SCL stuck".
 * 20 x 5 µs = 100 µs. Firmware policy, not a datasheet value. */
#define I2C_RECOVER_SCL_WAIT_HALVES 20U
/* D1: line samples before anything is driven (proposal §4.1). */
#define I2C_RECOVER_SAMPLES 16U

typedef struct {
    void *ctx;
    /* level of the line now: 0 = low, 1 = high */
    int (*read_sda)(void *ctx);
    int (*read_scl)(void *ctx);
    /* 0 = pull low, 1 = release (open-drain); ESP_OK or the driver error */
    esp_err_t (*set_sda)(void *ctx, int level);
    esp_err_t (*set_scl)(void *ctx, int level);
    /* half an SCL period (5 µs = 100 kHz, the bus speed) */
    void (*delay_half)(void *ctx);
} i2c_recover_io_t;

/* D1: how many of `n` samples read low, one half period apart. */
typedef struct {
    uint8_t samples;
    uint8_t sda_low;
    uint8_t scl_low;
} i2c_lines_t;

typedef enum {
    I2C_LINES_OK = 0,   /* both high in most samples */
    I2C_LINES_SDA_LOW,  /* SDA low, SCL high: a slave stopped mid-byte (case A) */
    I2C_LINES_SCL_LOW,  /* SCL low (case B); SDA may be either */
} i2c_lines_state_t;

typedef enum {
    I2C_RECOVER_IDLE = 0,       /* lines were high: only a STOP was sent */
    I2C_RECOVER_RECOVERED,      /* SDA was low and is high after the clocks */
    I2C_RECOVER_SDA_STILL_LOW,  /* 9 clocks did not release SDA */
    I2C_RECOVER_SCL_STUCK,      /* SCL low before or during the clocks: nothing driven
                                   further (case B) */
} i2c_recover_outcome_t;

typedef struct {
    i2c_recover_outcome_t outcome;
    uint8_t clocks;        /* SCL pulses sent (0..9) */
    bool sda_high_after;   /* read once after the STOP */
    bool scl_high_after;
} i2c_recover_result_t;

/* Result of i2c_master_probe() on one address, as the D2 log shows it. */
typedef enum {
    I2C_PROBE_OK = 0,   /* ESP_OK: ACK */
    I2C_PROBE_NACK,     /* ESP_ERR_NOT_FOUND */
    I2C_PROBE_TIMEOUT,  /* ESP_ERR_TIMEOUT: bus busy / held */
    I2C_PROBE_ERROR,    /* anything else */
    I2C_PROBE_SKIPPED,  /* not probed (no bus) */
} i2c_probe_t;

/* Proposal §4.2 cases, from the lines before R1 and the sensor probes after
 * it (AHT21 0x38 and ENS160 0x53; the optional OLED is not part of it). */
typedef enum {
    I2C_CASE_NORMAL = 0,  /* lines high, both sensors ACK */
    I2C_CASE_A,           /* SDA low before R1 (slave stuck mid-transfer) */
    I2C_CASE_B,           /* SCL low before R1 */
    I2C_CASE_C,           /* lines high, neither sensor ACKs (no bus hang) */
    I2C_CASE_OTHER,       /* lines high, mixed/timeouts: see the probe list */
} i2c_case_t;

/* D1: sample both lines `n` times (n clamped to 1..255). */
void i2c_recover_sample(const i2c_recover_io_t *io, uint8_t n, i2c_lines_t *out);

/* Majority of the samples decides "low". */
i2c_lines_state_t i2c_lines_state(const i2c_lines_t *lines);

/* R1. ESP_OK when the sequence ran (look at out->outcome for the bus
 * state); the first set_* error otherwise, with SDA and SCL released on a
 * best-effort basis. ESP_ERR_INVALID_ARG on NULL arguments. */
esp_err_t i2c_recover_run(const i2c_recover_io_t *io, i2c_recover_result_t *out);

i2c_probe_t i2c_probe_from_err(esp_err_t err);
i2c_case_t i2c_case_classify(i2c_lines_state_t before, i2c_probe_t aht21, i2c_probe_t ens160);

/* true when the boot bus reset should run: a sensor probe after R1 failed */
bool i2c_probe_wants_reset(i2c_probe_t aht21, i2c_probe_t ens160);

const char *i2c_lines_state_name(i2c_lines_state_t s);
const char *i2c_recover_outcome_name(i2c_recover_outcome_t o);
const char *i2c_probe_name(i2c_probe_t p);
/* "N" "A" "B" "C" "X" */
const char *i2c_case_name(i2c_case_t c);

#ifdef __cplusplus
}
#endif
