/* demo 5 พ.ย. 2569: PM2.5/RH thresholds (main/env_alert.h) fed into the
 * real FSM (components/fsm/ag_fsm.c), one row per second like the sampler. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ag_fsm.h"
#include "env_alert.h"

#define S(x) ((int64_t)(x) * 1000000LL)
#define PASS(msg) puts("PASS " msg)

typedef struct {
    env_alert_t env;
    ag_fsm_t fsm;
    ag_fsm_out_t out;
    int64_t t;    /* seconds since boot of the next row */
} sim_t;

static void sim_init(sim_t *s)
{
    memset(s, 0, sizeof(*s));
    ag_fsm_init(&s->fsm);
    s->t = 100;   /* past every boot grace */
}

/* one row (has_pm/has_rh false = empty column), then one FSM step */
static void row(sim_t *s, bool has_pm, uint16_t pm, bool has_rh, int32_t rh_x10)
{
    const int64_t at = S(s->t);
    env_alert_row(&s->env, at, has_pm, pm, has_rh, rh_x10);
    ag_fsm_in_t in = { .ml_enabled = true, .aht_ok_us = -1, .ens_ok_us = -1, .pms_ok_us = -1,
                       .indication = AG_FSM_IND_RUNNING, .ml_at_us = at };
    in.ml_status = env_alert_fsm_status(&s->env, s->out.ml_level, &in.ml_streak);
    s->out = ag_fsm_step(&s->fsm, &in, at);
    s->t++;
}

static void rows(sim_t *s, int n, uint16_t pm, int32_t rh_x10)
{
    for (int i = 0; i < n; ++i) {
        row(s, true, pm, true, rh_x10);
    }
}

static void test_boundaries(void)
{
    assert(env_alert_raw(true, 37, true, 300) == 0U);
    assert(env_alert_raw(true, 38, true, 700) == ENV_R_PM_WATCH);
    assert(env_alert_raw(true, 74, true, 500) == ENV_R_PM_WATCH);
    assert(env_alert_raw(true, 75, true, 500) == (ENV_R_PM_WATCH | ENV_R_PM_ALARM));
    assert(env_alert_raw(true, 10, true, 299) == ENV_R_RH_LOW);
    assert(env_alert_raw(true, 10, true, 701) == ENV_R_RH_HIGH);
    assert(env_alert_raw(false, 500, false, 0) == 0U);   /* empty columns never warn */
    assert(env_alert_level_of(ENV_R_RH_LOW | ENV_R_RH_HIGH) == AG_FSM_LEVEL_WATCH);
    assert(env_alert_level_of(ENV_R_PM_ALARM) == AG_FSM_LEVEL_ALARM);
    PASS("thresholds: PM 37/38, 74/75 · RH 30.0/29.9, 70.0/70.1 · empty = no warning");
}

static void test_dry_room_is_normal(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 600, 9, 354);   /* the 5 Nov room: PM 9, RH 35.4 % (ML ALARM before) */
    assert(s.out.shown == AG_FSM_SHOW_NORMAL && s.out.green && !s.out.buzzer);
    assert(s.env.reasons == 0U);
    PASS("RH 35.4 % / PM2.5 9 for 10 min: NORMAL (was ML ALARM)");
}

static void test_rh_low(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 10, 9, 450);
    rows(&s, 4, 9, 250);
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);   /* 4 rows: not yet */
    rows(&s, 1, 9, 250);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH && s.env.reasons == ENV_R_RH_LOW);
    assert(!s.out.red && !s.out.buzzer);         /* RH never goes past WATCH */
    rows(&s, 120, 9, 250);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH);
    /* back in range: WATCH held < 30 s, then NORMAL */
    rows(&s, 29, 9, 450);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH && s.env.reasons == 0U);
    rows(&s, 2, 9, 450);
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);
    PASS("RH < 30 %: WATCH after 5 rows, silent, NORMAL 30 s after it recovers");
}

static void test_rh_high(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 5, 9, 750);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH && s.env.reasons == ENV_R_RH_HIGH);
    PASS("RH > 70 %: WATCH (RH_HIGH)");
}

static void test_pm_levels(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 5, 40, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH && s.env.reasons == ENV_R_PM_WATCH);
    rows(&s, 4, 80, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH);
    rows(&s, 1, 80, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_ALARM);
    assert(s.env.reasons == (ENV_R_PM_WATCH | ENV_R_PM_ALARM));
    assert(s.out.red);
    /* down to the WATCH band: ALARM for 30 s, then WATCH (not NORMAL) */
    rows(&s, 29, 50, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_ALARM);
    rows(&s, 2, 50, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH);
    rows(&s, 60, 50, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH);
    /* clean air: NORMAL 30 s later */
    rows(&s, 31, 10, 500);
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);
    PASS("PM2.5: WATCH >= 37.5, ALARM >= 75, ALARM -> WATCH -> NORMAL, 30 s per level");
}

static void test_pm_alarm_beeps_and_ack(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 5, 120, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_ALARM);
    /* the beep pattern starts with the ALARM pattern origin: on at once */
    assert(s.out.buzzer);
    assert(ag_fsm_ack(&s.fsm, S(s.t)));
    rows(&s, 1, 120, 500);
    assert(s.out.shown == AG_FSM_SHOW_ML_ALARM && s.out.muted && !s.out.buzzer);
    PASS("PM2.5 ALARM beeps (FSM ALARM pattern) and ACK mutes it");
}

static void test_spikes_and_missing(void)
{
    sim_t s;
    sim_init(&s);
    rows(&s, 10, 9, 450);
    rows(&s, 1, 300, 450);    /* one-second spike */
    rows(&s, 1, 9, 450);
    rows(&s, 4, 300, 450);    /* 4 rows, broken by a normal one */
    rows(&s, 1, 9, 450);
    rows(&s, 4, 300, 450);
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);
    PASS("spikes shorter than 5 rows do not warn");

    sim_init(&s);
    for (int i = 0; i < 30; ++i) {
        row(&s, false, 0, false, 0);   /* PM warm-up + no T/RH: nothing to judge */
    }
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);
    /* PM empty (fan warm-up) but RH low: RH alone still warns */
    for (int i = 0; i < 5; ++i) {
        row(&s, false, 0, true, 200);
    }
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH && s.env.reasons == ENV_R_RH_LOW);
    /* an empty PM row restarts the PM count */
    sim_init(&s);
    rows(&s, 4, 50, 450);
    row(&s, false, 0, true, 450);
    rows(&s, 4, 50, 450);
    assert(s.out.shown == AG_FSM_SHOW_NORMAL);
    rows(&s, 1, 50, 450);
    assert(s.out.shown == AG_FSM_SHOW_ML_WATCH);
    PASS("empty columns: no warning from them, the other column still counts");
}

static void test_same_row_counted_once(void)
{
    env_alert_t e;
    memset(&e, 0, sizeof(e));
    for (int i = 0; i < 50; ++i) {   /* io task wakes ~10x per row */
        env_alert_row(&e, S(1), true, 90, true, 450);
    }
    assert(e.count[1] == 1U && e.reasons == 0U);
    PASS("the same row (same at_us) is counted once");
}

int main(void)
{
    test_boundaries();
    test_dry_room_is_normal();
    test_rh_low();
    test_rh_high();
    test_pm_levels();
    test_pm_alarm_beeps_and_ack();
    test_spikes_and_missing();
    test_same_row_counted_once();
    puts("env_alert: all tests passed");
    return 0;
}
