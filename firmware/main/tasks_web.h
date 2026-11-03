#ifndef AIRGUARD_TASKS_WEB_H
#define AIRGUARD_TASKS_WEB_H

#include "esp_err.h"

/* FW-8 step 3: on-device dashboard (docs/12-DASHBOARD-DESIGN.md).
 *
 * An HTTP server on the Wi-Fi STA interface (the phone hotspot already used
 * for SNTP) serving one page, a 10-minute history and a WebSocket that
 * receives one JSON message per second (components/webui/web_json.h).
 *
 * Read-only mirror: the page shows copies taken from the mailboxes of the
 * sampler, the io task (FSM), the ML result and the CO snapshot. It sends
 * no command to the device, and it is NOT a CO alarm (CLAUDE.md §8): the
 * CO path keeps deciding with its own LED/buzzer and never waits for this.
 *
 * Built only when WEB_ENABLE != 0 (default 1). -DWEB_ENABLE=0 (Q5: brownout
 * fallback) leaves no server and no web task; the mailboxes stay. */
#ifndef WEB_ENABLE
#define WEB_ENABLE 1
#endif

/* Start the server and the web task. Call from app_main after
 * time_sync_start() and sampler_start(). Without Wi-Fi (no secrets.h or a
 * failed start) nothing is started and ESP_ERR_INVALID_STATE is returned
 * for the log. Never aborts: callers log the error and carry on. */
esp_err_t web_start(void);

#endif
