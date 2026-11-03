#ifndef AIRGUARD_ML_MAILBOX_H
#define AIRGUARD_ML_MAILBOX_H

#include <stdbool.h>

#include "esp_err.h"

#include "ag_mahal.h"

/* FW-7 step 3: latest Mahalanobis result for the FSM (FW-6).
 *
 * One writer (the sampler, once per 1 Hz cycle) and any number of readers.
 * A queue of length 1 written with xQueueOverwrite and read with xQueuePeek,
 * the same pattern as the CO snapshot (tasks_co.h):
 * - the writer never blocks: an unread result is simply replaced, so the
 *   sampler never waits for FW-6;
 * - a reader gets a copy of the whole struct, taken under the queue's
 *   critical section, so it never sees half of one cycle and half of the
 *   next. `volatile` alone would not give that across two cores (CLAUDE.md
 *   §2), and a hand-written portMUX getter/setter is easy to bypass by
 *   accident.
 *
 * 🔴 The CO path (tasks_co.c) does not read this and nothing in it waits
 * for ML (CLAUDE.md §2). FW-7 itself does not drive any LED or buzzer:
 * mapping status to WATCH/ALARM is FW-6's decision.
 *
 * Freshness: at_us is the esp_timer time of the sample. A result older than
 * ~3 s means the sampler stopped; FW-6 decides what to show then. */

/* Create the mailbox with "no score yet" (AG_ML_MISSING, d2 NAN, at_us now).
 * Call once from app_main BEFORE io_init()/sampler_start() so their tasks
 * are created after the handle is written (task creation orders it; no lock
 * needed to read the handle later). Static storage: cannot fail for lack of
 * memory. */
esp_err_t ml_mailbox_init(void);

/* Sampler only. Never blocks. Ignored before ml_mailbox_init(). */
void ml_mailbox_publish(const ag_ml_result_t *r);

/* Copy of the latest result (never blocks). false before ml_mailbox_init(). */
bool ml_mailbox_peek(ag_ml_result_t *out);

#endif
