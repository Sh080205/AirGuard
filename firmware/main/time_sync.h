#ifndef AIRGUARD_TIME_SYNC_H
#define AIRGUARD_TIME_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* D1 (CLAUDE.md §7): real time from SNTP over a phone hotspot; fallback is
 * the console command "settime" (console.c). No real time → no CSV.
 *
 * Sets TZ to UTC+7, then starts Wi-Fi STA + SNTP when secrets.h exists.
 * Never aborts: a Wi-Fi init/connect failure is logged and only SNTP is lost;
 * reconnect is retried every TIME_SYNC_RECONNECT_MS from an esp_timer.
 * Needs nvs_flash_init() first (Wi-Fi stores calibration data in NVS). */
void time_sync_start(void);

/* True once SNTP has synced or settime succeeded; never goes back to false.
 * Safe from any task. */
bool time_sync_is_valid(void);

/* Set the system clock to epoch (UTC seconds) and mark time valid. */
esp_err_t time_sync_set_manual(int64_t epoch);

#endif
