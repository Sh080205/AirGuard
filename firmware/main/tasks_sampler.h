#ifndef AIRGUARD_TASKS_SAMPLER_H
#define AIRGUARD_TASKS_SAMPLER_H

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* FW-4 sampler: the only time-critical task of the logger.
 * Every 1000 ms (xTaskDelayUntil): read PMS7003 + AHT21 + ENS160, build one
 * csv_sample_t, score it with the FW-7 Mahalanobis model (result to
 * ml_mailbox.h) and hand both to the storage task without waiting.
 *
 * Owns: I2C bus, AHT21, ENS160, PMS7003 (UART1), GPIO4 debug pin, the MARK state.
 * mark_queue: io_mark_event_t from the io task (tasks_io.h).
 * Other task handles are only used for stack high-water reporting. */
typedef struct {
    QueueHandle_t mark_queue;
    TaskHandle_t storage_task;
    TaskHandle_t console_task;
} sampler_config_t;

/* Configures the buses (no sensor traffic yet) and starts the task.
 * The config is copied. */
esp_err_t sampler_start(const sampler_config_t *config);

/* OLED: the I2C bus the sampler created in sampler_start(), NULL when that
 * failed before the bus existed. Written before the sampler task is created
 * and never changed, so a task created after sampler_start() returns reads
 * it without a lock. The screen shares the bus; how it keeps out of the
 * sampler's way is in tasks_oled.h. */
i2c_master_bus_handle_t sampler_i2c_bus(void);

#endif
