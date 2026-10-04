#ifndef AIRGUARD_CONSOLE_H
#define AIRGUARD_CONSOLE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* D1 fallback: line commands on the console UART (U0RXD/U0TXD, the DevKit
 * port labelled "UART"):
 *   settime 2026-10-06T09:00:00+07:00
 *   help
 * Installs the UART RX driver on the console UART and starts one task. */
esp_err_t console_start(TaskHandle_t *out_task);

#endif
