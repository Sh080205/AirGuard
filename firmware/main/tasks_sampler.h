#ifndef AIRGUARD_TASKS_SAMPLER_H
#define AIRGUARD_TASKS_SAMPLER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/* FW-4 sampler: the only time-critical task of the logger.
 * Every 1000 ms (xTaskDelayUntil): read PMS7003 + AHT21 + ENS160, build one
 * csv_sample_t and hand it to the storage task without waiting.
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

#endif
