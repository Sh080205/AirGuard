#ifndef AIRGUARD_CO_ALERT_H
#define AIRGUARD_CO_ALERT_H

#include <stdbool.h>
#include <stdint.h>

#include "co_safety.h"

/* FW-5b step 3: what the LEDs/buzzer show for the CO path, pure C
 * (host-tested). Owned by the io task, which stays the only writer of the
 * GPIOs. The CO path overrides every other pattern (demo MODE, logger
 * indication) while it is active, so MODE and ACK cannot hide it.
 * Until FW-6 exists this is the whole display policy for CO; FW-6 may
 * replace the patterns but must keep "CO ALARM > CO FAULT > everything".
 *
 * Patterns differ by rhythm, not colour (CLAUDE.md §5, LED1 colour issue):
 *   ALARM  LED_R 5 Hz (100 ms on / 100 ms off, same as tasks_io ALARM)
 *          + buzzer on continuously
 *   FAULT  LED_R double blink every 2 s (on 0-100 ms and 200-300 ms)
 *          + buzzer chirp 100 ms every 60 s
 * Owner decisions 18 Oct 2026 (FW-5b report Q5/Q6):
 *   ACK in ALARM  mutes the buzzer for 5 min; LED_R keeps blinking; ALARM is
 *                 NOT cleared (only co_safety's hysteresis clears it). The
 *                 buzzer comes back after 5 min, or at once if a new ALARM
 *                 condition appears (e.g. instant >= 90 while only the 1-h
 *                 mean was over).
 *   ACK in FAULT  mutes the chirp until FAULT ends; LED_R keeps blinking.
 * Timing values are firmware policy; ค่าคาดหมาย ต้องยืนยันด้วยการวัด. */
#define CO_ALARM_PERIOD_US       200000LL
#define CO_ALARM_ON_US           100000LL
#define CO_FAULT_PERIOD_US      2000000LL
#define CO_FAULT_BLINK_US        100000LL   /* on, off, on: 3 x 100 ms */
#define CO_FAULT_CHIRP_PERIOD_US (60LL * 1000 * 1000)
#define CO_FAULT_CHIRP_US        100000LL
#define CO_ACK_MUTE_US           (5LL * 60 * 1000 * 1000)

typedef struct {
    bool prev_alarm, prev_fault;
    int64_t alarm_origin_us, fault_origin_us;
    bool alarm_muted;
    int64_t mute_until_us;
    uint8_t muted_reasons;
    uint8_t last_reasons;
    bool fault_muted;
} co_alert_t;

typedef struct {
    bool active;          /* CO decides LED_R/buzzer; other LEDs off */
    bool alarm;           /* showing ALARM (wins over FAULT) */
    bool fault;           /* FAULT true (incl. a stale CO snapshot) */
    bool red;
    bool buzzer;
    bool muted;           /* buzzer muted by ACK */
    bool alarm_entered;   /* edges, for logging */
    bool fault_entered;
    int64_t next_change_us;   /* INT64_MAX: nothing changes by itself */
} co_alert_out_t;

void co_alert_init(co_alert_t *a);

/* st: latest snapshot from the CO task, or NULL when the CO path is not
 * built (ZE07_ENABLE=0) -> never active. A snapshot older than
 * CO_SILENCE_US counts as FAULT (the CO task stopped). */
co_alert_out_t co_alert_eval(co_alert_t *a, const co_status_t *st, int64_t now_us);

/* ACK press at now_us, using the state of the last co_alert_eval().
 * Returns true when it muted something (ALARM buzzer or FAULT chirp). */
bool co_alert_ack(co_alert_t *a, int64_t now_us);

#endif
