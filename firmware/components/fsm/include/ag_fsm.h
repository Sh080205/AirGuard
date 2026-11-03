#pragma once

/* FW-6: device state machine (NORMAL / WATCH / ALARM / FAULT) shown on
 * LED_G / LED_Y / LED_R / buzzer. Pure C: no FreeRTOS, no GPIO, no malloc,
 * no float, so firmware/test_host compiles it with gcc.
 *
 * Owned by the io task only (it stays the only writer of the GPIOs). The
 * inputs are copies the io task takes from mailboxes (CO, ML + sensor
 * health), so nothing here is shared between tasks.
 *
 * 🔴 The CO path is NOT decided here. FW-5b's co_alert (co_alert.h) keeps
 * deciding CO ALARM / CO FAULT and their patterns, and the io task keeps
 * "co.active ? co outputs : fsm outputs". This module only learns that CO
 * is active (to stand aside) and decides everything below it, so the CO
 * path works even if this module or the ML path misbehaves (CLAUDE.md §2).
 *
 * Shown state, first true line wins (owner decisions 21 Oct 2026, FW-6
 * report §3 and Q1-Q7):
 *   CO        co_active: outputs all off here, the io task shows co_alert
 *   ML_ALARM  ML level ALARM      LED_R 5 Hz + 3 beeps every 10 s (Q3)
 *   SYS_FAULT sensor/ML fault     LED_Y double blink every 2 s, silent (Q6)
 *   ML_WATCH  ML level WATCH      LED_Y 1 Hz
 *   NORMAL    otherwise           LED_G steady / logger indication (FW-4)
 * Patterns differ by rhythm, not colour (CLAUDE.md §5, LED1 colour issue).
 *
 * All times are esp_timer_get_time() microseconds since boot. All values
 * below are firmware policy, not datasheet values:
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด / ข้อมูลจริง. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- ML level (Q1 option B, Q2) ----
 * WATCH  = ML result PERSISTENT (D² >= threshold on >= K rows, FW-7).
 * ALARM  = PERSISTENT with streak >= 30 rows (30 s of 1 Hz rows). Close to
 *          docs/03 §5 "ML path persistence ~20 s" while keeping the ML-3
 *          threshold and K unchanged.
 * Leave  = BELOW on consecutive samples for 30 s, one level at a time
 *          (ALARM -> WATCH -> NORMAL), so the light does not flicker with a
 *          D² that wanders around the threshold.
 * MISSING neither raises nor clears; it restarts the 30 s. With no valid
 * (non-missing) sample for 60 s the level drops to NORMAL: no evidence left. */
#define AG_FSM_ML_ALARM_STREAK     30U
#define AG_FSM_ML_CLEAR_US         (30LL * 1000 * 1000)
#define AG_FSM_ML_DROP_US          (60LL * 1000 * 1000)

/* ---- SYS FAULT (FW-6 report §5, Q6) ----
 * A monitored sensor is FAULT when its last success is LIMIT or more ago,
 * counting from no earlier than GRACE after boot (start-up is not a fault).
 *   AHT21   measures every 2 s; 30 s = 15 misses; grace 30 s after boot
 *   ENS160  "ok" = the sensor answers (warm-up / initial start-up are
 *           normal, DS §10.1); 60 s covers the 10 s start-up retry x 6
 *   PMS7003 ~1 frame/s (bringup 14 Oct); grace = the 30 s fan warm-up in
 *           which the sampler drops frames
 * ML_STALE: the newest ML result is older than 3 s (FW-7 step 1 D2: the
 * sampler stopped, so no CSV rows either). Before the first result the
 * sampler gets 10 s from boot.
 * Leaving SYS FAULT needs every reason gone for 10 s (no flicker). */
#define AG_FSM_AHT_GRACE_US        (30LL * 1000 * 1000)
#define AG_FSM_AHT_LIMIT_US        (30LL * 1000 * 1000)
#define AG_FSM_ENS_GRACE_US        0LL
#define AG_FSM_ENS_LIMIT_US        (60LL * 1000 * 1000)
#define AG_FSM_PMS_GRACE_US        (30LL * 1000 * 1000)
#define AG_FSM_PMS_LIMIT_US        (30LL * 1000 * 1000)
#define AG_FSM_ML_STALE_US         (3LL * 1000 * 1000)
#define AG_FSM_ML_BOOT_GRACE_US    (10LL * 1000 * 1000)
#define AG_FSM_FAULT_EXIT_US       (10LL * 1000 * 1000)

/* ---- patterns ----
 * WATCH and ALARM keep the FW-1 rhythms of tasks_io.c (1000 / 200 ms,
 * 50 %). ML ALARM sound (Q3): beeps at 0-100, 200-300, 400-500 ms of every
 * 10 s, so it never sounds like the continuous CO ALARM tone (co_alert.h).
 * SYS FAULT uses the same double blink as CO FAULT but on LED_Y and with no
 * chirp, so the two differ by colour AND by sound.
 * NORMAL indications are the FW-4 patterns of tasks_io.h. */
#define AG_FSM_WATCH_PERIOD_US     1000000LL
#define AG_FSM_WATCH_ON_US          500000LL
#define AG_FSM_ALARM_PERIOD_US      200000LL
#define AG_FSM_ALARM_ON_US          100000LL
#define AG_FSM_BEEP_PERIOD_US     10000000LL
#define AG_FSM_BEEP_US              100000LL
#define AG_FSM_BEEPS                3U
#define AG_FSM_FAULT_PERIOD_US     2000000LL
#define AG_FSM_FAULT_BLINK_US       100000LL   /* on, off, on: 3 x 100 ms */
#define AG_FSM_IND_PERIOD_US       2000000LL
#define AG_FSM_IND_WAIT_ON_US       100000LL
#define AG_FSM_IND_MARKED_ON_US    1900000LL

/* ---- demo (Q5) ----
 * MODE held >= 3 s (measured by the io task) starts a 60 s demo that shows
 * NORMAL -> WATCH -> ML ALARM -> SYS FAULT for 5 s each, then returns by
 * itself. CO always wins: CO becoming active ends the demo, and the demo
 * cannot start while CO is shown. ACK or a second long press ends it early.
 * The FSM writes no CSV column, so the demo never touches the data. */
#define AG_FSM_LONG_PRESS_US       (3LL * 1000 * 1000)
#define AG_FSM_DEMO_STEP_US        (5LL * 1000 * 1000)
#define AG_FSM_DEMO_US             (60LL * 1000 * 1000)

/* Mirror of ag_ml_status_t (FW-7 ag_mahal.h) so this module does not depend
 * on the mahal component; the io task maps one to the other (FW-6 step 3). */
typedef enum {
    AG_FSM_ML_NONE,          /* no result published yet */
    AG_FSM_ML_MODEL_FAULT,   /* boot self-test failed */
    AG_FSM_ML_MISSING,
    AG_FSM_ML_BELOW,
    AG_FSM_ML_ABOVE,
    AG_FSM_ML_PERSISTENT,
} ag_fsm_ml_t;

/* Mirror of io_indication_t (tasks_io.h). */
typedef enum {
    AG_FSM_IND_RUNNING,
    AG_FSM_IND_WAIT_TIME,
    AG_FSM_IND_MARKED,
} ag_fsm_ind_t;

typedef enum {
    AG_FSM_LEVEL_NONE,
    AG_FSM_LEVEL_WATCH,
    AG_FSM_LEVEL_ALARM,
} ag_fsm_level_t;

typedef enum {
    AG_FSM_SHOW_NORMAL,
    AG_FSM_SHOW_ML_WATCH,
    AG_FSM_SHOW_SYS_FAULT,
    AG_FSM_SHOW_ML_ALARM,
    AG_FSM_SHOW_CO,
} ag_fsm_show_t;

/* ag_fsm_in_t.monitored */
#define AG_FSM_SENSOR_AHT21        (1U << 0)
#define AG_FSM_SENSOR_ENS160       (1U << 1)
#define AG_FSM_SENSOR_PMS7003      (1U << 2)
#define AG_FSM_SENSOR_ALL          (AG_FSM_SENSOR_AHT21 | AG_FSM_SENSOR_ENS160 | AG_FSM_SENSOR_PMS7003)

/* ag_fsm_out_t.fault_reasons */
#define AG_FSM_FAULT_AHT21         (1U << 0)
#define AG_FSM_FAULT_ENS160        (1U << 1)
#define AG_FSM_FAULT_PMS7003       (1U << 2)
#define AG_FSM_FAULT_ML_MODEL      (1U << 3)
#define AG_FSM_FAULT_ML_STALE      (1U << 4)

typedef struct {
    bool co_active;            /* co_alert_out_t.active: CO owns the outputs */
    bool ml_enabled;           /* false: no ML in this build -> never WATCH/ALARM, no ML fault */
    ag_fsm_ml_t ml_status;     /* newest published ML result */
    uint16_t ml_streak;        /* ag_ml_result_t.streak */
    int64_t ml_at_us;          /* ag_ml_result_t.at_us; a new value = a new sample */
    uint8_t monitored;         /* AG_FSM_SENSOR_* checked for SYS FAULT */
    int64_t aht_ok_us;         /* last success per sensor, -1 = never */
    int64_t ens_ok_us;
    int64_t pms_ok_us;
    ag_fsm_ind_t indication;   /* logger state, shown on LED_G while NORMAL */
} ag_fsm_in_t;

typedef struct {
    ag_fsm_show_t shown;       /* what the outputs show (the demo step while demo) */
    bool demo;
    bool green, yellow, red, buzzer;
    bool muted;                /* ML ALARM sound muted by ACK */
    bool changed;              /* shown or demo differs from the previous step */
    ag_fsm_level_t ml_level;   /* real ML level, also while CO or demo is shown */
    bool sys_fault;            /* real SYS FAULT, also while something else is shown */
    uint8_t fault_reasons;     /* AG_FSM_FAULT_* true now */
    int64_t next_change_us;    /* next pattern/demo edge; INT64_MAX = steady.
                                  State changes driven by timeouts are not in
                                  it: call ag_fsm_step at least every 100 ms */
} ag_fsm_out_t;

/* Owned by the io task; ag_fsm_init() before the first step. */
typedef struct {
    bool started;
    int64_t last_now_us;

    ag_fsm_level_t level;
    bool ml_seen;
    int64_t ml_last_at_us;
    bool have_valid;
    int64_t ml_valid_us;       /* at_us of the last non-missing sample */
    bool clearing;
    int64_t clear_since_us;
    bool alarm_muted;

    bool fault;
    bool fault_clearing;
    int64_t fault_clear_since_us;

    bool demo;
    int64_t demo_start_us;

    ag_fsm_show_t shown;
    bool shown_demo;
    int64_t demo_step;
    ag_fsm_ind_t ind;
    int64_t origin_us;         /* phase origin of the current pattern */
} ag_fsm_t;

void ag_fsm_init(ag_fsm_t *f);

/* One evaluation at now_us (a now_us that goes backwards is treated as the
 * previous now). in == NULL keeps NORMAL logic with nothing monitored. */
ag_fsm_out_t ag_fsm_step(ag_fsm_t *f, const ag_fsm_in_t *in, int64_t now_us);

/* ACK press, using the state of the last ag_fsm_step(). The io task gives
 * ACK to co_alert_ack() first; this one does nothing while CO is shown.
 * Ends a demo, or mutes the ML ALARM sound until the ML ALARM ends (the
 * light keeps blinking, the level is not lowered: Q4). true = did something. */
bool ag_fsm_ack(ag_fsm_t *f, int64_t now_us);

/* MODE held >= AG_FSM_LONG_PRESS_US: starts the demo, or ends a running one.
 * Refused (false) while CO is shown. */
bool ag_fsm_mode_long_press(ag_fsm_t *f, int64_t now_us);

const char *ag_fsm_show_name(ag_fsm_show_t shown);

/* ---- MODE long press (FW-6 step 3) ----
 * The button ISR only reports falling edges, so "held for 3 s" is measured
 * by the io task: a debounced press starts the hold, then the io task feeds
 * the pin level on every wake (<= 100 ms) until release. A release seen at
 * any wake cancels the hold; the next press starts a new 3 s. One hold
 * fires at most once, however long it lasts. */
typedef struct {
    bool down;
    bool fired;
    int64_t since_us;
} ag_fsm_hold_t;

/* pressed: debounced press confirmed or pin still low. Returns true exactly
 * once per hold, when it reaches AG_FSM_LONG_PRESS_US. */
bool ag_fsm_hold_update(ag_fsm_hold_t *h, bool pressed, int64_t now_us);

/* When the io task must look at the pin again: the 3 s mark of a hold that
 * has not fired yet, else INT64_MAX. */
int64_t ag_fsm_hold_deadline_us(const ag_fsm_hold_t *h);

#ifdef __cplusplus
}
#endif
