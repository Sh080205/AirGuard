/* FW-6 step 3: the io task's join of the CO path and the FSM, run with the
 * real co_alert.c + ag_fsm.c + main/io_compose.h. The io task loop itself
 * (FreeRTOS, GPIO) is not compiled here; tick() below repeats its order:
 * buttons -> co_alert_eval -> ag_fsm_step(co_active) -> compose. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ag_fsm.h"
#include "co_alert.h"
#include "io_compose.h"

#define PASS(name) printf("PASS %s\n", name)

#define MS(x) ((int64_t)(x) * 1000)
#define S(x)  ((int64_t)(x) * 1000 * 1000)

typedef enum { CO_NONE, CO_OK, CO_ALARM, CO_FAULT } co_kind_t;

typedef struct {
    co_alert_t co;
    ag_fsm_t fsm;
    co_status_t snap;
    bool co_have;
    co_alert_out_t co_out;
    ag_fsm_out_t fsm_out;
    uint32_t outputs;
    uint16_t streak;
} io_sim_t;

static void sim_init(io_sim_t *s)
{
    memset(s, 0, sizeof(*s));
    co_alert_init(&s->co);
    ag_fsm_init(&s->fsm);
}

static void set_co(io_sim_t *s, co_kind_t kind, int64_t now)
{
    s->co_have = kind != CO_NONE;
    s->snap = (co_status_t){
        .updated_us = now, .frame_us = now, .have_frame = true,
        .alarm = kind == CO_ALARM, .fault = kind == CO_FAULT,
        .alarm_reasons = kind == CO_ALARM ? CO_REASON_INSTANT : 0,
        .fault_reasons = kind == CO_FAULT ? CO_FAULT_SILENT : 0,
    };
}

/* What the stub build gives the FSM today: no ML, no sensor health. */
static ag_fsm_in_t stub_input(bool co_active)
{
    return (ag_fsm_in_t){ .co_active = co_active, .ml_enabled = false,
                          .ml_status = AG_FSM_ML_NONE, .monitored = 0,
                          .aht_ok_us = -1, .ens_ok_us = -1, .pms_ok_us = -1,
                          .indication = AG_FSM_IND_RUNNING };
}

/* Healthy sensors; ML result `ml` sampled on whole seconds. */
static ag_fsm_in_t full_input(io_sim_t *s, bool co_active, ag_fsm_ml_t ml, int64_t now)
{
    const int64_t at = now - now % S(1);
    if (now % S(1) == 0) {
        s->streak = (ml == AG_FSM_ML_PERSISTENT || ml == AG_FSM_ML_ABOVE) ? s->streak + 1 : 0;
    }
    return (ag_fsm_in_t){ .co_active = co_active, .ml_enabled = true, .ml_status = ml,
                          .ml_streak = s->streak, .ml_at_us = at,
                          .monitored = AG_FSM_SENSOR_ALL,
                          .aht_ok_us = now, .ens_ok_us = now, .pms_ok_us = now,
                          .indication = AG_FSM_IND_RUNNING };
}

/* One io loop at now. use_full: ML/health inputs, else the stub ones. */
static void tick(io_sim_t *s, int64_t now, bool use_full, ag_fsm_ml_t ml)
{
    s->co_out = co_alert_eval(&s->co, s->co_have ? &s->snap : NULL, now);
    const ag_fsm_in_t in = use_full ? full_input(s, s->co_out.active, ml, now)
                                    : stub_input(s->co_out.active);
    s->fsm_out = ag_fsm_step(&s->fsm, &in, now);
    s->outputs = io_compose_outputs(&s->co_out, &s->fsm_out);
}

static void test_stub_build_normal(void)
{
    io_sim_t s;
    sim_init(&s);
    for (int64_t t = 0; t < S(600); t += MS(100)) {
        tick(&s, t, false, AG_FSM_ML_NONE);
        assert(s.outputs == IO_OUT_GREEN);
        assert(!s.fsm_out.sys_fault && s.fsm_out.fault_reasons == 0);
        assert(io_compose_next_change_us(&s.co_out, &s.fsm_out) == INT64_MAX);
    }
    /* no CO path built (co_have false) and nothing to mute: ACK does nothing */
    assert(io_route_ack(&s.co, s.co_have, &s.fsm, S(600)) == IO_ACK_NONE);
    PASS("stub build (no ML, no health, no CO): NORMAL steady, no fault, ACK no-op");
}

static void test_co_alarm_over_ml_alarm(void)
{
    io_sim_t s;
    sim_init(&s);
    int64_t t = 0;
    set_co(&s, CO_OK, t);
    /* 40 s of PERSISTENT -> ML ALARM (streak >= 30) */
    for (; t < S(40); t += MS(100)) {
        set_co(&s, CO_OK, t);
        tick(&s, t, true, AG_FSM_ML_PERSISTENT);
    }
    assert(s.fsm_out.shown == AG_FSM_SHOW_ML_ALARM && !s.co_out.active);
    assert((s.outputs & (IO_OUT_GREEN | IO_OUT_YELLOW)) == 0);

    /* CO ALARM arrives: for 60 s only CO drives the outputs, buzzer on
     * continuously (not the ML 3-beep), no green/yellow ever */
    for (; t < S(100); t += MS(100)) {
        set_co(&s, CO_ALARM, t);
        tick(&s, t, true, AG_FSM_ML_PERSISTENT);
        assert(s.co_out.active && s.fsm_out.shown == AG_FSM_SHOW_CO);
        assert(s.fsm_out.ml_level == AG_FSM_LEVEL_ALARM);   /* still tracked */
        assert((s.outputs & (IO_OUT_GREEN | IO_OUT_YELLOW)) == 0);
        assert((s.outputs & IO_OUT_BUZZER) != 0);
        assert(((s.outputs & IO_OUT_RED) != 0) == s.co_out.red);
        assert(io_compose_next_change_us(&s.co_out, &s.fsm_out) == s.co_out.next_change_us);
    }

    /* ACK goes to CO (mute 5 min), the FSM never sees it */
    assert(io_route_ack(&s.co, s.co_have, &s.fsm, t) == IO_ACK_CO_MUTED);
    assert(!s.fsm.alarm_muted);
    /* demo cannot start under CO */
    assert(!ag_fsm_mode_long_press(&s.fsm, t));
    for (int64_t end = t + S(10); t < end; t += MS(100)) {
        set_co(&s, CO_ALARM, t);
        tick(&s, t, true, AG_FSM_ML_PERSISTENT);
        assert((s.outputs & IO_OUT_BUZZER) == 0);   /* CO muted */
        assert(s.co_out.active);
    }

    /* CO clears: ML ALARM shows at once, with its own (unmuted) beeps */
    set_co(&s, CO_OK, t);
    tick(&s, t, true, AG_FSM_ML_PERSISTENT);
    assert(!s.co_out.active && s.fsm_out.shown == AG_FSM_SHOW_ML_ALARM && s.fsm_out.changed);
    assert(s.outputs == (IO_OUT_RED | IO_OUT_BUZZER));   /* pattern restarts "on" */
    /* now ACK reaches the FSM */
    assert(io_route_ack(&s.co, s.co_have, &s.fsm, t) == IO_ACK_FSM);
    t += MS(100);
    set_co(&s, CO_OK, t);
    tick(&s, t, true, AG_FSM_ML_PERSISTENT);
    assert(s.fsm_out.muted && (s.outputs & IO_OUT_BUZZER) == 0);
    PASS("CO ALARM over ML ALARM: CO-only outputs, ACK to CO, ML resumes after");
}

static void test_co_fault_over_sys_fault(void)
{
    io_sim_t s;
    sim_init(&s);
    int64_t t = 0;
    /* PMS never succeeds -> SYS FAULT after grace + limit (60 s) */
    for (; t < S(70); t += MS(100)) {
        set_co(&s, CO_OK, t);
        s.co_out = co_alert_eval(&s.co, &s.snap, t);
        ag_fsm_in_t in = full_input(&s, s.co_out.active, AG_FSM_ML_BELOW, t);
        in.pms_ok_us = -1;
        s.fsm_out = ag_fsm_step(&s.fsm, &in, t);
        s.outputs = io_compose_outputs(&s.co_out, &s.fsm_out);
    }
    assert(s.fsm_out.shown == AG_FSM_SHOW_SYS_FAULT);
    assert(s.fsm_out.fault_reasons == AG_FSM_FAULT_PMS7003);

    /* CO task stops publishing: snapshot goes stale -> CO FAULT wins; the
     * yellow SYS FAULT blink never shows */
    const int64_t last_pub = t;
    set_co(&s, CO_OK, last_pub);
    bool saw_chirp = false;
    for (; t < last_pub + S(30); t += MS(100)) {
        s.co_out = co_alert_eval(&s.co, &s.snap, t);
        ag_fsm_in_t in = full_input(&s, s.co_out.active, AG_FSM_ML_BELOW, t);
        in.pms_ok_us = -1;
        s.fsm_out = ag_fsm_step(&s.fsm, &in, t);
        s.outputs = io_compose_outputs(&s.co_out, &s.fsm_out);
        if (t > last_pub + CO_SILENCE_US) {
            assert(s.co_out.active && s.co_out.fault);
            assert((s.outputs & (IO_OUT_YELLOW | IO_OUT_GREEN)) == 0);
            assert(s.fsm_out.sys_fault);   /* still tracked underneath */
            saw_chirp |= (s.outputs & IO_OUT_BUZZER) != 0;
        }
    }
    assert(saw_chirp);   /* CO FAULT chirp sounds; SYS FAULT is silent */
    PASS("CO FAULT over SYS FAULT: CO pattern only, SYS FAULT kept underneath");
}

static void test_demo_and_co(void)
{
    io_sim_t s;
    sim_init(&s);
    int64_t t = 0;
    set_co(&s, CO_OK, t);
    tick(&s, t, false, AG_FSM_ML_NONE);
    /* demo starts, ACK (no CO) ends it through the FSM */
    assert(ag_fsm_mode_long_press(&s.fsm, t));
    for (; t < S(7); t += MS(100)) {
        set_co(&s, CO_OK, t);
        tick(&s, t, false, AG_FSM_ML_NONE);
    }
    assert(s.fsm_out.demo && s.fsm_out.shown == AG_FSM_SHOW_ML_WATCH);
    assert(io_route_ack(&s.co, s.co_have, &s.fsm, t) == IO_ACK_FSM);
    set_co(&s, CO_OK, t);
    tick(&s, t, false, AG_FSM_ML_NONE);
    assert(!s.fsm_out.demo && s.outputs == IO_OUT_GREEN);

    /* demo again, CO ALARM in the ML ALARM step: demo ends, CO only */
    assert(ag_fsm_mode_long_press(&s.fsm, t));
    const int64_t demo_start = t;
    for (; t < demo_start + S(11); t += MS(100)) {
        set_co(&s, CO_OK, t);
        tick(&s, t, false, AG_FSM_ML_NONE);
    }
    assert(s.fsm_out.demo && s.fsm_out.shown == AG_FSM_SHOW_ML_ALARM);
    set_co(&s, CO_ALARM, t);
    tick(&s, t, false, AG_FSM_ML_NONE);
    assert(s.co_out.active && !s.fsm_out.demo);
    assert(s.outputs == (IO_OUT_RED | IO_OUT_BUZZER));
    /* CO ends: the demo does not come back */
    t += MS(100);
    set_co(&s, CO_OK, t);
    tick(&s, t, false, AG_FSM_ML_NONE);
    assert(!s.fsm_out.demo && s.outputs == IO_OUT_GREEN);
    PASS("demo: ACK ends it, CO ALARM ends it and wins, no comeback");
}

int main(void)
{
    test_stub_build_normal();
    test_co_alarm_over_ml_alarm();
    test_co_fault_over_sys_fault();
    test_demo_and_co();
    puts("ALL io_compose tests passed");
    return 0;
}
