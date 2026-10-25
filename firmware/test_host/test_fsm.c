#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ag_fsm.h"

#define PASS(name) printf("PASS %s\n", name)

#define MS(x) ((int64_t)(x) * 1000)
#define S(x)  ((int64_t)(x) * 1000 * 1000)

/* Healthy input: every sensor just succeeded, ML BELOW sampled now. */
static ag_fsm_in_t healthy(int64_t now)
{
    return (ag_fsm_in_t){
        .co_active = false, .ml_enabled = true,
        .ml_status = AG_FSM_ML_BELOW, .ml_streak = 0, .ml_at_us = now,
        .monitored = AG_FSM_SENSOR_ALL,
        .aht_ok_us = now, .ens_ok_us = now, .pms_ok_us = now,
        .indication = AG_FSM_IND_RUNNING,
    };
}

static void expect_out(const ag_fsm_out_t *o, ag_fsm_show_t shown, int g, int y, int r, int b,
                       const char *what)
{
    if (o->shown != shown || o->green != g || o->yellow != y || o->red != r || o->buzzer != b) {
        fprintf(stderr, "%s: shown=%s g=%d y=%d r=%d b=%d, want %s g=%d y=%d r=%d b=%d\n", what,
                ag_fsm_show_name(o->shown), o->green, o->yellow, o->red, o->buzzer,
                ag_fsm_show_name(shown), g, y, r, b);
        assert(0);
    }
}

/* Feed one 1 Hz ML sample per second from `from` up to (not incl.) `to`,
 * calling the FSM every 100 ms like the io task. Sensors stay healthy.
 * streak counts up from *streak for PERSISTENT/ABOVE, resets otherwise. */
static ag_fsm_out_t run_ml(ag_fsm_t *f, int64_t from, int64_t to, ag_fsm_ml_t status,
                           uint16_t *streak)
{
    ag_fsm_out_t o = { 0 };
    int64_t sample_at = -1;
    for (int64_t t = from; t < to; t += MS(100)) {
        if ((t - from) % S(1) == 0) {
            sample_at = t;
            if (status == AG_FSM_ML_PERSISTENT || status == AG_FSM_ML_ABOVE) {
                ++*streak;
            } else {
                *streak = 0;
            }
        }
        ag_fsm_in_t in = healthy(t);
        in.ml_status = status;
        in.ml_streak = *streak;
        in.ml_at_us = sample_at;
        o = ag_fsm_step(f, &in, t);
    }
    return o;
}

static void test_normal_and_indication(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(0);
    ag_fsm_out_t o = ag_fsm_step(&f, &in, 0);
    assert(o.changed);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "normal steady");
    assert(o.next_change_us == INT64_MAX);
    o = ag_fsm_step(&f, &in, MS(50));
    assert(!o.changed);

    /* WAIT_TIME: 100 ms on every 2 s, phase restarts at the change */
    in = healthy(MS(700));
    in.indication = AG_FSM_IND_WAIT_TIME;
    o = ag_fsm_step(&f, &in, MS(700));
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "wait on");
    assert(!o.changed);   /* indication is not a shown-state change */
    assert(o.next_change_us == MS(800));
    in.ml_at_us = MS(700);
    o = ag_fsm_step(&f, &in, MS(800));
    expect_out(&o, AG_FSM_SHOW_NORMAL, 0, 0, 0, 0, "wait off");
    assert(o.next_change_us == MS(2700));

    /* MARKED: 1900 ms on, 100 ms off */
    in = healthy(S(3));
    in.indication = AG_FSM_IND_MARKED;
    o = ag_fsm_step(&f, &in, S(3));
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "marked on");
    in.ml_at_us = S(4);
    o = ag_fsm_step(&f, &in, S(3) + MS(1950));
    expect_out(&o, AG_FSM_SHOW_NORMAL, 0, 0, 0, 0, "marked off");
    PASS("normal + logger indications");
}

static void test_watch_alarm_and_clear(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    uint16_t streak = 0;
    ag_fsm_out_t o = run_ml(&f, 0, S(20), AG_FSM_ML_BELOW, &streak);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "below");

    /* ABOVE (streak < K) does not raise */
    o = run_ml(&f, S(20), S(22), AG_FSM_ML_ABOVE, &streak);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "above only");

    /* PERSISTENT -> WATCH at once: yellow 500/500 from the entry */
    ag_fsm_in_t in = healthy(S(22));
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 3;
    o = ag_fsm_step(&f, &in, S(22));
    assert(o.changed && o.ml_level == AG_FSM_LEVEL_WATCH);
    expect_out(&o, AG_FSM_SHOW_ML_WATCH, 0, 1, 0, 0, "watch on");
    assert(o.next_change_us == S(22) + MS(500));
    o = ag_fsm_step(&f, &in, S(22) + MS(500));
    expect_out(&o, AG_FSM_SHOW_ML_WATCH, 0, 0, 0, 0, "watch off");
    o = ag_fsm_step(&f, &in, S(23));
    expect_out(&o, AG_FSM_SHOW_ML_WATCH, 0, 1, 0, 0, "watch on again");

    /* streak 29 stays WATCH, 30 -> ALARM */
    streak = 3;
    o = run_ml(&f, S(23), S(49), AG_FSM_ML_PERSISTENT, &streak);
    assert(streak == 29);
    expect_out(&o, AG_FSM_SHOW_ML_WATCH, 0, o.yellow, 0, 0, "streak 29");
    in = healthy(S(49));
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 30;
    o = ag_fsm_step(&f, &in, S(49));
    assert(o.changed && o.ml_level == AG_FSM_LEVEL_ALARM);
    expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, 1, 1, "alarm t=0");
    /* red 100/100 ms; beeps at 0-100, 200-300, 400-500 ms of 10 s */
    static const struct { int ms, red, buz; } alarm_wave[] = {
        { 50, 1, 1 }, { 100, 0, 0 }, { 150, 0, 0 }, { 200, 1, 1 }, { 300, 0, 0 },
        { 400, 1, 1 }, { 499, 1, 1 }, { 500, 0, 0 }, { 600, 1, 0 }, { 9900, 0, 0 },
        { 10000, 1, 1 }, { 10250, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(alarm_wave) / sizeof(alarm_wave[0]); ++i) {
        const int64_t t = S(49) + MS(alarm_wave[i].ms);
        in.ml_at_us = t - (t - S(49)) % S(1);
        in.ml_streak = (uint16_t)(30 + (t - S(49)) / S(1));
        in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = t;
        o = ag_fsm_step(&f, &in, t);
        expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, alarm_wave[i].red, alarm_wave[i].buz, "alarm wave");
    }

    /* PERSISTENT below the alarm streak does not lower ALARM */
    streak = 3;
    o = run_ml(&f, S(60), S(65), AG_FSM_ML_PERSISTENT, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_ALARM);

    /* BELOW 29 s keeps ALARM; the 30 s mark -> WATCH; another 30 s -> NORMAL */
    o = run_ml(&f, S(65), S(95), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_ALARM);
    o = run_ml(&f, S(95), S(96), AG_FSM_ML_BELOW, &streak);   /* sample at 95 s */
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    expect_out(&o, AG_FSM_SHOW_ML_WATCH, 0, o.yellow, 0, 0, "alarm -> watch");
    o = run_ml(&f, S(96), S(125), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    o = run_ml(&f, S(125), S(126), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_NONE);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "watch -> normal");
    PASS("ML WATCH/ALARM entry, patterns, one-level-per-30 s exit");
}

static void enter_watch(ag_fsm_t *f, int64_t at)
{
    ag_fsm_in_t in = healthy(at);
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 3;
    const ag_fsm_out_t o = ag_fsm_step(f, &in, at);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
}

static void test_clear_interrupted(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    uint16_t streak = 0;
    enter_watch(&f, 0);
    ag_fsm_out_t o = run_ml(&f, S(1), S(25), AG_FSM_ML_BELOW, &streak);
    /* ABOVE restarts the 30 s */
    o = run_ml(&f, S(25), S(26), AG_FSM_ML_ABOVE, &streak);
    o = run_ml(&f, S(26), S(56), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    o = run_ml(&f, S(56), S(57), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_NONE);

    /* MISSING restarts the 30 s too, and keeps the level */
    ag_fsm_init(&f);
    enter_watch(&f, 0);
    o = run_ml(&f, S(1), S(25), AG_FSM_ML_BELOW, &streak);
    o = run_ml(&f, S(25), S(30), AG_FSM_ML_MISSING, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    o = run_ml(&f, S(30), S(60), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    o = run_ml(&f, S(60), S(61), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_NONE);

    /* the same published sample read 10 times per second counts once */
    ag_fsm_init(&f);
    enter_watch(&f, 0);
    ag_fsm_in_t in = healthy(S(1));
    in.ml_at_us = S(1);              /* one BELOW sample, never a new one */
    for (int64_t t = S(1); t < S(40); t += MS(100)) {
        in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = t;
        o = ag_fsm_step(&f, &in, t);
    }
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);   /* one BELOW sample cannot clear */
    PASS("clearing restarts on ABOVE / MISSING, duplicate samples count once");
}

static void test_missing_drop_and_stale(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    uint16_t streak = 0;
    enter_watch(&f, 0);
    ag_fsm_out_t o = run_ml(&f, S(1), S(60), AG_FSM_ML_MISSING, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);   /* 59.9 s since the last valid one */
    assert(!o.sys_fault);
    o = run_ml(&f, S(60), S(61), AG_FSM_ML_MISSING, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_NONE);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "missing 60 s -> normal");

    /* stale: the sampler stops publishing (at_us frozen at 0) */
    ag_fsm_init(&f);
    enter_watch(&f, 0);
    ag_fsm_in_t in = healthy(0);
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 3;
    in.ml_at_us = 0;
    o = ag_fsm_step(&f, &in, S(3));
    assert(!o.sys_fault);
    o = ag_fsm_step(&f, &in, S(3) + MS(100));
    assert(o.sys_fault && (o.fault_reasons & AG_FSM_FAULT_ML_STALE));
    expect_out(&o, AG_FSM_SHOW_SYS_FAULT, 0, 1, 0, 0, "stale -> sys fault over watch");
    o = ag_fsm_step(&f, &in, S(61));
    assert(o.ml_level == AG_FSM_LEVEL_NONE && o.sys_fault);

    /* before the first result: 10 s grace, then stale */
    ag_fsm_init(&f);
    in = healthy(0);
    in.ml_status = AG_FSM_ML_NONE;
    o = ag_fsm_step(&f, &in, S(9));
    assert(!o.sys_fault);
    in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = S(10);
    o = ag_fsm_step(&f, &in, S(10));
    assert(o.sys_fault && o.fault_reasons == AG_FSM_FAULT_ML_STALE);

    /* ML not built: no ML fault, never WATCH/ALARM */
    ag_fsm_init(&f);
    in = healthy(S(100));
    in.ml_enabled = false;
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 100;
    in.ml_at_us = 0;
    o = ag_fsm_step(&f, &in, S(100));
    assert(!o.sys_fault && o.ml_level == AG_FSM_LEVEL_NONE);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "ml disabled");
    PASS("MISSING 60 s drop, ML stale, boot grace, ML disabled");
}

static void test_model_fault(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(1));
    in.ml_status = AG_FSM_ML_MODEL_FAULT;
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(1));
    assert(o.sys_fault && o.fault_reasons == AG_FSM_FAULT_ML_MODEL);
    assert(o.ml_level == AG_FSM_LEVEL_NONE);
    expect_out(&o, AG_FSM_SHOW_SYS_FAULT, 0, 1, 0, 0, "model fault");
    /* double blink: on 0-100, off 100-200, on 200-300, off to 2 s */
    static const struct { int ms, y; } wave[] = {
        { 50, 1 }, { 100, 0 }, { 199, 0 }, { 200, 1 }, { 299, 1 }, { 300, 0 }, { 1999, 0 }, { 2000, 1 },
    };
    for (size_t i = 0; i < sizeof(wave) / sizeof(wave[0]); ++i) {
        const int64_t t = S(1) + MS(wave[i].ms);
        in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = t;
        o = ag_fsm_step(&f, &in, t);
        expect_out(&o, AG_FSM_SHOW_SYS_FAULT, 0, wave[i].y, 0, 0, "fault wave");
    }
    PASS("ML model fault -> SYS FAULT double blink, silent");
}

static void test_sensor_faults(void)
{
    ag_fsm_t f;
    /* never OK: AHT21 and PMS after grace 30 s + limit 30 s, ENS160 after 60 s */
    static const struct { uint8_t sensor, reason; } cases[] = {
        { AG_FSM_SENSOR_AHT21, AG_FSM_FAULT_AHT21 },
        { AG_FSM_SENSOR_ENS160, AG_FSM_FAULT_ENS160 },
        { AG_FSM_SENSOR_PMS7003, AG_FSM_FAULT_PMS7003 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        ag_fsm_init(&f);
        ag_fsm_in_t in = healthy(S(59));
        in.aht_ok_us = (cases[i].sensor == AG_FSM_SENSOR_AHT21) ? -1 : in.aht_ok_us;
        in.ens_ok_us = (cases[i].sensor == AG_FSM_SENSOR_ENS160) ? -1 : in.ens_ok_us;
        in.pms_ok_us = (cases[i].sensor == AG_FSM_SENSOR_PMS7003) ? -1 : in.pms_ok_us;
        ag_fsm_out_t o = ag_fsm_step(&f, &in, S(59) + MS(999));
        assert(!o.sys_fault);
        in.ml_at_us = S(60);
        o = ag_fsm_step(&f, &in, S(60));
        assert(o.sys_fault && o.fault_reasons == cases[i].reason);
        /* not monitored -> no fault */
        ag_fsm_init(&f);
        in.monitored = (uint8_t)(AG_FSM_SENSOR_ALL & ~cases[i].sensor);
        o = ag_fsm_step(&f, &in, S(60));
        assert(!o.sys_fault);
    }

    /* AHT21 OK at 100 s then silent: fault at 130 s, not at 129.9 s */
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(129) + MS(900));
    in.aht_ok_us = S(100);
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(129) + MS(900));
    assert(!o.sys_fault);
    in.ml_at_us = S(130);
    o = ag_fsm_step(&f, &in, S(130));
    assert(o.sys_fault && o.fault_reasons == AG_FSM_FAULT_AHT21);

    /* exit needs 10 s clean; a reason in between restarts it */
    in.aht_ok_us = S(131);
    in.ml_at_us = S(131);
    o = ag_fsm_step(&f, &in, S(131));
    assert(o.sys_fault && o.fault_reasons == 0);
    in.ml_at_us = S(140);
    o = ag_fsm_step(&f, &in, S(140));
    assert(o.sys_fault);                       /* 9 s clean */
    in.aht_ok_us = S(110);                     /* stale again for a moment */
    in.ml_at_us = S(140) + MS(500);
    o = ag_fsm_step(&f, &in, S(140) + MS(500));
    assert(o.sys_fault && o.fault_reasons == AG_FSM_FAULT_AHT21);
    in.aht_ok_us = S(141);
    in.ml_at_us = S(141);
    o = ag_fsm_step(&f, &in, S(141));
    in.ml_at_us = S(150);
    in.aht_ok_us = S(150);
    o = ag_fsm_step(&f, &in, S(150));
    assert(o.sys_fault);                       /* 9 s since the restart */
    in.ml_at_us = S(151);
    in.aht_ok_us = S(151);
    o = ag_fsm_step(&f, &in, S(151));
    assert(!o.sys_fault);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "fault cleared");

    /* a last-OK time in the future (clock mix-up) is not a fault */
    ag_fsm_init(&f);
    in = healthy(S(200));
    in.ens_ok_us = S(500);
    o = ag_fsm_step(&f, &in, S(200));
    assert(!o.sys_fault);
    PASS("sensor FAULT timing per sensor, monitored mask, 10 s exit");
}

static void test_priorities(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    /* ML ALARM */
    ag_fsm_in_t in = healthy(S(100));
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 30;
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(100));
    expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, 1, 1, "alarm");

    /* + SYS FAULT (ENS160 dead): ML ALARM still shown */
    in.ens_ok_us = 0;
    in.ml_at_us = S(101);
    in.ml_streak = 31;
    o = ag_fsm_step(&f, &in, S(101));
    assert(o.sys_fault && o.fault_reasons == AG_FSM_FAULT_ENS160);
    assert(o.shown == AG_FSM_SHOW_ML_ALARM);

    /* + CO active: CO shown, all FSM outputs off, ML level kept */
    in.co_active = true;
    in.ml_at_us = S(102);
    in.ml_streak = 32;
    o = ag_fsm_step(&f, &in, S(102));
    assert(o.changed && o.ml_level == AG_FSM_LEVEL_ALARM && o.sys_fault);
    expect_out(&o, AG_FSM_SHOW_CO, 0, 0, 0, 0, "co over all");
    assert(o.next_change_us == INT64_MAX);

    /* CO ends: the ML ALARM underneath is shown again */
    in.co_active = false;
    in.ml_at_us = S(103);
    o = ag_fsm_step(&f, &in, S(103));
    assert(o.changed);
    expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, 1, 1, "back to alarm");

    /* ALARM clears to WATCH while SYS FAULT stays: SYS FAULT over WATCH */
    ag_fsm_init(&f);
    enter_watch(&f, S(100));
    in = healthy(S(100) + MS(500));
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 4;
    in.ml_at_us = S(100);
    in.pms_ok_us = 0;
    o = ag_fsm_step(&f, &in, S(100) + MS(500));
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    expect_out(&o, AG_FSM_SHOW_SYS_FAULT, 0, 1, 0, 0, "fault over watch");
    PASS("priority CO > ML ALARM > SYS FAULT > ML WATCH > NORMAL");
}

static void test_ack(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(100));
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(100));
    assert(!ag_fsm_ack(&f, S(100)));           /* NORMAL: nothing to do */
    enter_watch(&f, S(101));
    assert(!ag_fsm_ack(&f, S(101)));           /* WATCH: nothing */

    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 30;
    in.ml_at_us = S(102);
    o = ag_fsm_step(&f, &in, S(102));
    assert(o.buzzer && !o.muted);
    assert(ag_fsm_ack(&f, S(102)));
    assert(!ag_fsm_ack(&f, S(102)));           /* already muted */
    o = ag_fsm_step(&f, &in, S(102) + MS(10));
    expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, 1, 0, "muted: red keeps blinking");
    assert(o.muted && o.ml_level == AG_FSM_LEVEL_ALARM);
    o = ag_fsm_step(&f, &in, S(112));          /* next 10 s cycle: still silent */
    assert(!o.buzzer && o.red);

    /* ALARM ends (-> WATCH) and comes back: sound again */
    uint16_t streak = 0;
    o = run_ml(&f, S(113), S(144), AG_FSM_ML_BELOW, &streak);
    assert(o.ml_level == AG_FSM_LEVEL_WATCH);
    in = healthy(S(144));
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = 30;
    o = ag_fsm_step(&f, &in, S(144));
    expect_out(&o, AG_FSM_SHOW_ML_ALARM, 0, 0, 1, 1, "new alarm unmuted");

    /* CO shown: ACK is co_alert's, this one does nothing */
    in.co_active = true;
    in.ml_at_us = S(145);
    in.ml_streak = 31;
    o = ag_fsm_step(&f, &in, S(145));
    assert(!ag_fsm_ack(&f, S(145)));
    in.co_active = false;
    in.ml_at_us = S(146);
    o = ag_fsm_step(&f, &in, S(146));
    assert(o.buzzer && !o.muted);

    /* SYS FAULT: ACK does nothing */
    ag_fsm_init(&f);
    in = healthy(S(100));
    in.ml_status = AG_FSM_ML_MODEL_FAULT;
    o = ag_fsm_step(&f, &in, S(100));
    assert(o.shown == AG_FSM_SHOW_SYS_FAULT);
    assert(!ag_fsm_ack(&f, S(100)));
    PASS("ACK mutes ML ALARM until it ends, no-op elsewhere and under CO");
}

static void test_demo(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(100));
    in.indication = AG_FSM_IND_WAIT_TIME;
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(100));
    assert(ag_fsm_mode_long_press(&f, S(100)));

    static const struct { int64_t at; ag_fsm_show_t shown; int g, y, r, b; } steps[] = {
        { 0,           AG_FSM_SHOW_NORMAL,    1, 0, 0, 0 },   /* steady, not the WAIT blip */
        { MS(4900),    AG_FSM_SHOW_NORMAL,    1, 0, 0, 0 },
        { S(5),        AG_FSM_SHOW_ML_WATCH,  0, 1, 0, 0 },
        { S(5) + MS(600), AG_FSM_SHOW_ML_WATCH, 0, 0, 0, 0 },
        { S(10),       AG_FSM_SHOW_ML_ALARM,  0, 0, 1, 1 },   /* demo beeps */
        { S(15),       AG_FSM_SHOW_SYS_FAULT, 0, 1, 0, 0 },
        { S(20),       AG_FSM_SHOW_NORMAL,    1, 0, 0, 0 },   /* second round */
        { S(30),       AG_FSM_SHOW_ML_ALARM,  0, 0, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); ++i) {
        const int64_t t = S(100) + steps[i].at;
        in.ml_at_us = t;
        in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = t;
        o = ag_fsm_step(&f, &in, t);
        assert(o.demo);
        expect_out(&o, steps[i].shown, steps[i].g, steps[i].y, steps[i].r, steps[i].b, "demo step");
    }
    /* next_change_us includes the step boundary: the steady NORMAL step
     * (40-45 s) has no pattern edge, so the next change is the step end */
    in.ml_at_us = S(100) + S(41);
    o = ag_fsm_step(&f, &in, S(100) + S(41));
    assert(o.demo && o.shown == AG_FSM_SHOW_NORMAL);
    assert(o.next_change_us == S(100) + S(45));
    /* ends by itself after 60 s, back to the real state */
    in.ml_at_us = S(160);
    in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = S(160);
    o = ag_fsm_step(&f, &in, S(159) + MS(999));
    assert(o.demo);
    o = ag_fsm_step(&f, &in, S(160));
    assert(!o.demo && o.changed);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "demo over (WAIT blip on)");

    /* ACK ends the demo; a second long press ends it */
    assert(ag_fsm_mode_long_press(&f, S(161)));
    o = ag_fsm_step(&f, &in, S(161));
    assert(o.demo);
    assert(ag_fsm_ack(&f, S(161)));
    o = ag_fsm_step(&f, &in, S(161) + MS(10));
    assert(!o.demo);
    assert(ag_fsm_mode_long_press(&f, S(162)));
    assert(ag_fsm_mode_long_press(&f, S(162) + MS(10)));
    o = ag_fsm_step(&f, &in, S(162) + MS(20));
    assert(!o.demo);

    /* CO during the demo ends it; long press refused while CO is shown */
    assert(ag_fsm_mode_long_press(&f, S(163)));
    in.co_active = true;
    o = ag_fsm_step(&f, &in, S(163) + MS(100));
    assert(!o.demo && o.shown == AG_FSM_SHOW_CO);
    assert(!ag_fsm_mode_long_press(&f, S(164)));
    in.co_active = false;
    o = ag_fsm_step(&f, &in, S(164) + MS(100));
    assert(!o.demo);
    PASS("demo: 4 steps x 5 s, 60 s auto end, ACK/long press end, CO wins");
}

/* For every state: outputs never change before next_change_us, and
 * stepping exactly at next_change_us is where they may change. */
static void test_next_change(void)
{
    static const ag_fsm_ml_t mls[] = { AG_FSM_ML_BELOW, AG_FSM_ML_PERSISTENT, AG_FSM_ML_PERSISTENT,
                                       AG_FSM_ML_MODEL_FAULT, AG_FSM_ML_BELOW, AG_FSM_ML_BELOW };
    static const uint16_t streaks[] = { 0, 3, 30, 0, 0, 0 };
    static const ag_fsm_ind_t inds[] = { AG_FSM_IND_RUNNING, AG_FSM_IND_RUNNING, AG_FSM_IND_RUNNING,
                                         AG_FSM_IND_RUNNING, AG_FSM_IND_WAIT_TIME, AG_FSM_IND_MARKED };
    for (size_t c = 0; c < sizeof(mls) / sizeof(mls[0]); ++c) {
        ag_fsm_t f;
        ag_fsm_init(&f);
        ag_fsm_in_t in = healthy(S(100));
        in.ml_status = mls[c];
        in.ml_streak = streaks[c];
        in.indication = inds[c];
        ag_fsm_out_t o = ag_fsm_step(&f, &in, S(100));
        int64_t next = o.next_change_us;
        ag_fsm_out_t ref = o;
        int edges = 0;
        for (int64_t t = S(100) + MS(1); t <= S(112); t += MS(1)) {
            /* healthy, one ML sample per second with the same status */
            in.aht_ok_us = in.ens_ok_us = in.pms_ok_us = t;
            in.ml_at_us = t - t % S(1);
            o = ag_fsm_step(&f, &in, t);
            const int same = o.green == ref.green && o.yellow == ref.yellow &&
                             o.red == ref.red && o.buzzer == ref.buzzer;
            if (t < next) {
                if (!same) {
                    fprintf(stderr, "case %u: output changed at %.0f before next %.0f\n",
                            (unsigned)c, (double)t, (double)next);
                    assert(0);
                }
            } else {
                assert(t == next);
                edges += !same;
                ref = o;
                next = o.next_change_us;
                assert(next > t);
            }
        }
        /* only NORMAL + RUNNING is steady; every other case blinks */
        assert(c == 0 ? edges == 0 : edges > 0);
    }
    PASS("next_change_us is never later than an output edge");
}

static void test_time_and_null(void)
{
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(100));
    ag_fsm_out_t o = ag_fsm_step(&f, &in, S(100));
    o = ag_fsm_step(&f, &in, S(50));            /* backwards: treated as S(100) */
    assert(!o.sys_fault);
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "time backwards");

    /* streak saturated at 65535 stays ALARM */
    in.ml_status = AG_FSM_ML_PERSISTENT;
    in.ml_streak = UINT16_MAX;
    in.ml_at_us = S(101);
    o = ag_fsm_step(&f, &in, S(101));
    assert(o.ml_level == AG_FSM_LEVEL_ALARM);

    /* NULL input / state */
    ag_fsm_init(&f);
    o = ag_fsm_step(&f, NULL, S(1));
    expect_out(&o, AG_FSM_SHOW_NORMAL, 1, 0, 0, 0, "null input");
    o = ag_fsm_step(NULL, &in, S(1));
    assert(o.shown == AG_FSM_SHOW_NORMAL && !o.green);
    assert(!ag_fsm_ack(NULL, 0) && !ag_fsm_mode_long_press(NULL, 0));
    ag_fsm_init(NULL);
    assert(strcmp(ag_fsm_show_name(AG_FSM_SHOW_ML_ALARM), "ML ALARM") == 0);
    assert(strcmp(ag_fsm_show_name((ag_fsm_show_t)99), "?") == 0);
    PASS("time backwards, saturated streak, NULL arguments");
}

/* FW-6 step 3: MODE hold measured by the io task (polls <= 100 ms) */
static void test_mode_hold(void)
{
    ag_fsm_hold_t h = { 0 };
    assert(ag_fsm_hold_deadline_us(&h) == INT64_MAX);

    /* short press: released at 2.9 s, never fires */
    assert(!ag_fsm_hold_update(&h, true, S(10)));
    assert(ag_fsm_hold_deadline_us(&h) == S(13));
    for (int64_t t = S(10) + MS(100); t < S(12) + MS(900); t += MS(100)) {
        assert(!ag_fsm_hold_update(&h, true, t));
    }
    assert(!ag_fsm_hold_update(&h, false, S(12) + MS(900)));
    assert(!h.down && ag_fsm_hold_deadline_us(&h) == INT64_MAX);

    /* a new press restarts the 3 s; fires once at 3 s, never again while held */
    assert(!ag_fsm_hold_update(&h, true, S(20)));
    assert(!ag_fsm_hold_update(&h, true, S(23) - 1));
    assert(ag_fsm_hold_update(&h, true, S(23)));
    assert(ag_fsm_hold_deadline_us(&h) == INT64_MAX);
    int fired = 0;
    for (int64_t t = S(23) + MS(100); t < S(40); t += MS(100)) {
        fired += ag_fsm_hold_update(&h, true, t);
    }
    assert(fired == 0);

    /* release then hold again: fires again after another 3 s */
    assert(!ag_fsm_hold_update(&h, false, S(40)));
    assert(!ag_fsm_hold_update(&h, true, S(41)));
    assert(ag_fsm_hold_update(&h, true, S(44) + MS(50)));

    /* NULL arguments */
    assert(!ag_fsm_hold_update(NULL, true, 0) && ag_fsm_hold_deadline_us(NULL) == INT64_MAX);

    /* hold -> long press toggles the demo through the real FSM call */
    ag_fsm_t f;
    ag_fsm_init(&f);
    ag_fsm_in_t in = healthy(S(50));
    (void)ag_fsm_step(&f, &in, S(50));
    ag_fsm_hold_t m = { 0 };
    ag_fsm_out_t o = { 0 };
    for (int64_t t = S(50); t <= S(54); t += MS(100)) {
        if (ag_fsm_hold_update(&m, true, t)) {
            assert(t == S(53));
            assert(ag_fsm_mode_long_press(&f, t));
        }
        in = healthy(t);
        o = ag_fsm_step(&f, &in, t);
    }
    assert(o.demo && o.shown == AG_FSM_SHOW_NORMAL);
    PASS("MODE hold: 3 s fires once, release cancels, starts the demo");
}

int main(void)
{
    test_normal_and_indication();
    test_watch_alarm_and_clear();
    test_clear_interrupted();
    test_missing_drop_and_stale();
    test_model_fault();
    test_sensor_faults();
    test_priorities();
    test_ack();
    test_demo();
    test_next_change();
    test_time_and_null();
    test_mode_hold();
    puts("ALL fsm tests passed");
    return 0;
}
