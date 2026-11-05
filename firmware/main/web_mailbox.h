#ifndef AIRGUARD_WEB_MAILBOX_H
#define AIRGUARD_WEB_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "ag_fsm.h"
#include "csv_row.h"

/* FW-8 step 3 (docs/12-DASHBOARD-DESIGN.md §2.3): what the dashboard shows,
 * written by the tasks that already own it.
 *
 * Same pattern as the CO snapshot (tasks_co.h) and ml_mailbox.h: a queue of
 * length 1 written with xQueueOverwrite and read with xQueuePeek.
 * - The writers (sampler at 1 Hz, io after every FSM step) never block, so
 *   the web task can be slow or stopped without touching their timing
 *   (CLAUDE.md §2: no slow work in the time-critical tasks).
 * - The web task gets a copy of the whole struct taken under the queue's
 *   critical section, never half of one publish and half of the next
 *   (`volatile` alone would not give that across the two cores).
 * - Only the newest value matters (a state, not an event), so an unread
 *   value is simply replaced: no queue can fill up and push back.
 * Static storage: creation cannot fail for lack of heap. */

/* The sampler's row of the current cycle (also before real time exists:
 * the page shows the values with "t": null). */
typedef struct {
    csv_sample_t row;
    bool time_valid;   /* this cycle had real time, so row.local_time is set */
    int64_t at_us;     /* esp_timer time of the cycle start */
} web_row_t;

/* What the io task's FSM shows on the LEDs (ag_fsm_out_t). */
typedef struct {
    ag_fsm_show_t shown;
    bool demo;
    bool muted;
    uint8_t fault_reasons;   /* AG_FSM_FAULT_* */
    uint8_t env_reasons;     /* ENV_R_* (env_alert.h): why WATCH/ALARM */
    int64_t at_us;
} web_show_t;

/* Call once from app_main BEFORE io_init()/sampler_start(), so the handles
 * exist before the writer tasks are created (task creation orders it; no
 * lock needed to read the handles later). Built in every configuration:
 * with WEB_ENABLE=0 nobody reads them and a publish costs one copy. */
esp_err_t web_mailbox_init(void);

/* Writers: never block; ignored before web_mailbox_init(). */
void web_row_publish(const web_row_t *r);
void web_show_publish(const web_show_t *s);

/* Reader (web task): false before web_mailbox_init() or before the first
 * publish. Never blocks. */
bool web_row_peek(web_row_t *out);
bool web_show_peek(web_show_t *out);

#endif
