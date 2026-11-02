#ifndef AIRGUARD_I2C_BOOT_H
#define AIRGUARD_I2C_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "i2c_recover.h"

/* I2C bus check and recovery at boot (docs/tasks/reports/I2C-recovery.md;
 * owner approved 2 Nov 2026: D1 + D2 + R1 + boot bus reset, nothing while
 * running). Called from sampler_start() only, before the sampler and OLED
 * tasks exist, so nothing else is on the bus while it runs.
 *
 *   i2c_boot_recover()  D1 sample SDA/SCL, then R1 9-clock bus clear + STOP
 *                       (before i2c_new_master_bus())
 *   i2c_boot_probe()    D2 probe 0x38 / 0x53 / 0x3C; if a sensor does not
 *                       ACK: i2c_master_bus_reset() once and probe again
 *                       (after i2c_new_master_bus(), before the attaches)
 *
 * Neither returns an error to the caller: a failed check must not stop the
 * sampler (the sensors may still answer, and the CO path never depends on
 * this bus, CLAUDE.md §2). Every esp_err_t is checked, kept in the report
 * and logged. */

#define I2C_BOOT_ADDR_N 3

typedef struct {
    bool recover_ran;
    i2c_lines_t lines;              /* D1, before anything is driven */
    i2c_lines_state_t before;
    esp_err_t gpio_err;             /* gpio_config / gpio_set_level error, ESP_OK if none */
    i2c_recover_result_t r1;
    int64_t r1_us;                  /* D1 + R1 duration */

    bool probe_ran;
    /* AHT21 0x38, ENS160 0x53, OLED 0x3C (OLED optional: not in the case) */
    i2c_probe_t first[I2C_BOOT_ADDR_N];  /* right after the bus is created */
    bool bus_reset;                 /* boot i2c_master_bus_reset() was run */
    esp_err_t bus_reset_err;
    i2c_probe_t final[I2C_BOOT_ADDR_N];  /* after the reset (= first if none) */
    i2c_case_t kase;
} i2c_boot_report_t;

void i2c_boot_recover(int sda_gpio, int scl_gpio);
void i2c_boot_probe(i2c_master_bus_handle_t bus);

/* Written by the task that runs sampler_start() before the sampler task is
 * created and never changed afterwards, so the sampler reads it without a
 * lock (task creation orders the writes before the new task runs). */
const i2c_boot_report_t *i2c_boot_report(void);

/* One line with the whole boot result; the sampler repeats it in its 60 s
 * summary so a long serial log still shows how this boot started. */
void i2c_boot_log_line(void);

#endif
