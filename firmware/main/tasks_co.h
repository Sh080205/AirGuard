#ifndef AIRGUARD_TASKS_CO_H
#define AIRGUARD_TASKS_CO_H

#include <stdbool.h>

#include "esp_err.h"
#include "co_safety.h"

/* FW-5b: CO path task. Records and tracks the CO level reported by the
 * ZE07-CO for research. NOT a CO alarm (CLAUDE.md §8): never use this
 * device instead of a UL 2034 / EN 50291 CO alarm.
 *
 * Separate from the ML path and from the logger (CLAUDE.md §2): it reads
 * UART2 (GPIO5 RX only), decides with fixed ppm thresholds (co_safety.h)
 * and publishes a co_status_t snapshot. It uses no real time, SD, Wi-Fi or
 * model, and never waits for the sampler or any other task.
 *
 * Built only when ZE07_ENABLE != 0 (default 1). With ZE07_ENABLE=0 there is
 * no task and no mailbox: co_peek_status() returns false, co_ppm stays
 * empty and the boot log says so. */
#ifndef ZE07_ENABLE
#define ZE07_ENABLE 1
#endif

/* Create the mailbox with an initial "warming up, no frame" snapshot.
 * Call once from app_main BEFORE io_init()/sampler_start(), so their tasks
 * are created after the handle is written (task creation orders it; no
 * lock needed to read the handle later). */
esp_err_t co_mailbox_init(void);

/* Set up UART2 and start the CO task. If UART2 cannot be set up the task
 * still runs and publishes FAULT (CO_FAULT_HW) so the failure is visible;
 * the error is returned for the log. Returns ESP_ERR_NO_MEM only when the
 * task itself could not be created (then readers see a stale snapshot,
 * which co_alert shows as FAULT). */
esp_err_t co_start(void);

/* Copy of the latest snapshot (xQueuePeek, never blocks). false when the
 * CO path is not built or co_mailbox_init() was not called/failed. */
bool co_peek_status(co_status_t *out);

#endif
