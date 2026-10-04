#ifndef AIRGUARD_TASKS_STORAGE_H
#define AIRGUARD_TASKS_STORAGE_H

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "csv_row.h"

/* FW-4 storage task: the only owner of the SPI bus, the SD card and every
 * FILE*. The sampler hands it rows and events through queues and never
 * waits on it (FW-4 §7: slow I/O must not move the 1 Hz loop). */

/* Row queue capacity. FW-4 §7: must ride out an SD stall of >= 30 s at
 * 1 row/s. 64 rows = 64 s, about twice the requirement, so a 30 s stall
 * plus a remount attempt still fits.
 * Memory: 64 x sizeof(csv_sample_t) (72 B expected on ESP32-S3: struct tm
 * 36 B + flags/values; bounded by _Static_assert in tasks_storage.c and
 * printed at boot) ≈ 4.6 KB static RAM, counted in idf.py size as .bss.
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด (SD max latency in the 60 s summary) */
#define STORAGE_ROW_QUEUE_LEN 64U

/* Events are rare (two MARK presses each); 4 slots only matter when the SD
 * is gone, and every event is also printed to the console log as backup. */
#define STORAGE_EVENT_QUEUE_LEN 4U

typedef struct {
    struct tm start;   /* timestamp of the first marked row */
    struct tm end;     /* timestamp of the last marked row */
} storage_event_t;

/* Initialise the SPI bus and queues and start the task. The card itself is
 * mounted by the task (and remounted after errors), so a missing card is not
 * an error here. */
esp_err_t storage_start(TaskHandle_t *out_task);

/* Non-blocking (timeout 0). false = queue full, caller counts a dropped row. */
bool storage_submit_row(const csv_sample_t *row);
bool storage_submit_event(const storage_event_t *ev);

/* Rows currently waiting; the sampler tracks the maximum (queue high-water). */
unsigned storage_rows_waiting(void);

#endif
