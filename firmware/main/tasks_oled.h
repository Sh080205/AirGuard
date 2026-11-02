#ifndef AIRGUARD_TASKS_OLED_H
#define AIRGUARD_TASKS_OLED_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#include "csv_row.h"

/* OLED status screen (docs/tasks/reports/OLED.md, step 1 approved 2 Nov).
 * A reader only: it shows what the io task shows on the LEDs (io_peek_shown),
 * the CO snapshot (co_peek_status), the ML result (ml_mailbox_peek) and the
 * sampler's latest row (oled_publish_row). It decides nothing, writes no
 * CSV/serial line, and nothing waits for it. A missing or broken screen is
 * only logged (CLAUDE.md §2: the CO path and the logger never depend on it).
 *
 * Shared I2C bus (sampler owns it, 100 kHz) — three layers keep the screen
 * out of the sampler's way:
 * 1. Chunks of half a page (64 data bytes ≈ 5.9 ms on the wire), so the
 *    bus lock is never held longer than that; the AHT21/ENS160 transfers
 *    (20 ms timeout incl. the lock wait) can wait at most one chunk.
 * 2. Sent only after the sampler's cycle: oled_publish_row() is called once
 *    the cycle's I2C work is done and wakes the screen task, which sends
 *    changed chunks until 700 ms after the cycle start, then stops.
 * 3. Priority 4 on core 1 (task_config.h).
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด (OLED.md step 3).
 *
 * -DOLED_ENABLE=0 builds without the screen (no task, no mailbox, the
 * sampler's publish does nothing). */
#ifndef OLED_ENABLE
#define OLED_ENABLE 1
#endif

/* Create the row mailbox (static, cannot fail for lack of memory). Call
 * from app_main BEFORE sampler_start(), so the sampler task is created after
 * the handle is written. ESP_ERR_NOT_SUPPORTED when OLED_ENABLE=0. */
esp_err_t oled_mailbox_init(void);

/* Sampler only, once per cycle after its last I2C transfer. Never blocks
 * (overwrite of a 1-slot queue). row->local_time and row->marked are
 * meaningful only when time_valid. t0_us: esp_timer time of the cycle
 * start. Does nothing before oled_mailbox_init(). */
void oled_publish_row(const csv_sample_t *row, bool time_valid, int64_t t0_us);

/* Add the screen to the sampler's bus and start the task (call after
 * sampler_start()). The screen itself is probed by the task, so a missing
 * screen still returns ESP_OK here. Errors: ESP_ERR_INVALID_STATE (no bus or
 * no mailbox), the i2c_master_bus_add_device() error, ESP_ERR_NO_MEM (task),
 * ESP_ERR_NOT_SUPPORTED (OLED_ENABLE=0). */
esp_err_t oled_start(i2c_master_bus_handle_t bus);

#endif
