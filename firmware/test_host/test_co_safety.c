/* FW-5b step 3: CO decision logic (co_safety) and display policy (co_alert).
 * Sequences of simulated readings only: no real CO is ever produced
 * (docs/03-CO_SAFETY.md §4). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "co_alert.h"
#include "co_safety.h"

#define S(sec) ((int64_t)(sec) * 1000000LL)
#define MS(ms) ((int64_t)(ms) * 1000LL)

static co_safety_t g;   /* large (480 buckets): keep off the stack */

static co_status_t status_at(const co_safety_t *s, int64_t now)
{
    co_status_t st;
    co_safety_status(s, now, &st);
    st.updated_us = now;
    return st;
}

static void frame(co_safety_t *s, int64_t t, uint16_t x10)
{
    co_safety_on_frame(s, t, x10);
    co_safety_tick(s, t);
}

/* ---------------------------------------------------------------------
 * Reference model, written separately from co_safety.c: keeps every
 * sample with its minute index and recomputes the means by brute force. */
#define REF_MAX (17 * 3600)
typedef struct {
    int64_t start;
    int32_t minute[REF_MAX];
    uint16_t x[REF_MAX];
    size_t n;
    bool have, alarm, elev, fault;
    int64_t last_valid;
    bool a_clearing, e_clearing;
    int64_t a_since, e_since;
    unsigned streak;
    uint32_t mean1, mean8;   /* as of the last frame, like co_status_t */
} ref_t;
static ref_t R;

static void ref_init(ref_t *r, int64_t t)
{
    memset(r, 0, sizeof(*r));
    r->start = t;
}

/* sum over samples whose minute is in (cur - w, cur]; returns count.
 * Samples are stored in time order, so scan back from the newest. */
static uint32_t ref_window(const ref_t *r, int64_t t, int32_t w, uint64_t *sum)
{
    const int32_t cur = (int32_t)((t - r->start) / S(60));
    uint32_t c = 0;
    *sum = 0;
    for (size_t i = r->n; i-- > 0;) {
        if (cur - r->minute[i] >= w) {
            break;
        }
        *sum += r->x[i];
        ++c;
    }
    return c;
}

static void ref_tick(ref_t *r, int64_t t)
{
    const int64_t ref = r->have ? r->last_valid : r->start;
    if (t - ref > S(6)) {
        r->fault = true;
        r->streak = 0;
        r->a_clearing = r->e_clearing = false;
    }
}

static void ref_frame(ref_t *r, int64_t t, uint16_t x)
{
    r->have = true;
    r->last_valid = t;
    if (++r->streak >= 3) {
        r->fault = false;
    }
    if (t - r->start >= S(300)) {
        assert(r->n < REF_MAX);
        r->minute[r->n] = (int32_t)((t - r->start) / S(60));
        r->x[r->n++] = x;
    }
    uint64_t s1, s8;
    const uint32_t c1 = ref_window(r, t, 60, &s1);
    const uint32_t c8 = ref_window(r, t, 480, &s8);
    /* compare as real numbers: mean >= 30.0 ppm etc. */
    const double m1 = c1 ? (double)s1 / c1 : 0.0, m8 = c8 ? (double)s8 / c8 : 0.0;
    r->mean1 = c1 ? (uint32_t)(s1 / c1) : 0;
    r->mean8 = c8 ? (uint32_t)(s8 / c8) : 0;
    const bool trip = x >= 900 || (c1 && m1 >= 300.0);
    if (trip) {
        r->alarm = true;
        r->a_clearing = false;
    } else if (r->alarm) {
        const bool clear = x < 810 && !(c1 && m1 >= 270.0);
        if (!clear) {
            r->a_clearing = false;
        } else if (!r->a_clearing) {
            r->a_clearing = true;
            r->a_since = t;
        } else if (t - r->a_since >= S(60)) {
            r->alarm = false;
            r->a_clearing = false;
        }
    }
    if (c8 && m8 >= 90.0) {
        r->elev = true;
        r->e_clearing = false;
    } else if (r->elev) {
        const bool clear = !(c8 && m8 >= 81.0);
        if (!clear) {
            r->e_clearing = false;
        } else if (!r->e_clearing) {
            r->e_clearing = true;
            r->e_since = t;
        } else if (t - r->e_since >= S(60)) {
            r->elev = false;
            r->e_clearing = false;
        }
    }
}

static void ref_check(const ref_t *r, const co_safety_t *s, int64_t t)
{
    const co_status_t st = status_at(s, t);
    assert(st.alarm == r->alarm);
    assert(st.elevated_8h == r->elev);
    assert(st.fault == r->fault);
    assert(st.mean_1h_x10 == r->mean1);
    assert(st.mean_8h_x10 == r->mean8);
}

static uint32_t lcg = 2026U;   /* fixed seed */
static uint32_t rnd(uint32_t n)
{
    lcg = lcg * 1103515245U + 12345U;
    return (lcg >> 8) % n;
}

/* --------------------------------------------------------------------- */

static void test_boot_silence_and_hw_fault(void)
{
    co_safety_init(&g, 0);
    co_safety_tick(&g, S(6));
    co_status_t st = status_at(&g, S(6));
    assert(!st.fault && st.warmup && !st.have_frame);
    co_safety_tick(&g, S(6) + 1);
    st = status_at(&g, S(6) + 1);
    assert(st.fault && st.fault_reasons == CO_FAULT_SILENT && g.faults_entered == 1);

    co_safety_init(&g, 0);
    co_safety_set_hw_fault(&g);
    for (int i = 1; i <= 10; ++i) {
        frame(&g, S(i), 10);
    }
    st = status_at(&g, S(10));
    assert(st.fault && st.fault_reasons == CO_FAULT_HW);
    puts("PASS boot: FAULT only after > 6 s without a frame; UART setup failure = FAULT until reboot");
}

static void test_fault_exit_needs_three_good_frames(void)
{
    co_safety_init(&g, 0);
    co_safety_tick(&g, S(7));
    assert(g.fault);
    frame(&g, S(8), 10);
    frame(&g, S(9), 10);
    assert(g.fault);
    co_safety_on_bad_frame(&g);          /* streak broken */
    frame(&g, S(10), 10);
    frame(&g, S(11), 10);
    assert(g.fault);
    frame(&g, S(12), 10);
    assert(!g.fault);
    /* lost again: a gap of exactly 6 s is fine, 6 s + 1 us is not */
    co_safety_tick(&g, S(18));
    assert(!g.fault);
    co_safety_tick(&g, S(18) + 1);
    assert(g.fault && g.faults_entered == 2);
    puts("PASS FAULT exit after 3 valid frames in a row; a bad frame restarts the count");
}

static void test_instant_threshold_and_warmup(void)
{
    co_safety_init(&g, 0);
    frame(&g, S(10), 899);
    co_status_t st = status_at(&g, S(10));
    assert(!st.alarm && st.warmup && st.mean_1h_x10 == 0);
    frame(&g, S(11), 900);
    st = status_at(&g, S(11));
    assert(st.alarm && st.alarm_reasons == CO_REASON_INSTANT && st.warmup);
    uint16_t v;
    assert(!co_status_csv_value(&st, S(11), MS(2500), &v));   /* warm-up: CSV empty */
    /* warm-up ends at exactly 5 min */
    assert(status_at(&g, S(300) - 1).warmup);
    assert(!status_at(&g, S(300)).warmup);
    puts("PASS instant >= 90.0 ppm trips on one frame even in warm-up; 89.9 does not; warm-up = 5 min");
}

static void test_mean_threshold_exact(void)
{
    co_safety_init(&g, 0);
    for (int t = 300; t < 400; ++t) {
        frame(&g, S(t), 299);
    }
    assert(!g.alarm && g.mean_1h_x10 == 299);
    co_safety_init(&g, 0);
    frame(&g, S(300), 300);
    assert(g.alarm && g.reasons == CO_REASON_MEAN_1H);
    /* samples before the end of warm-up never enter the mean */
    co_safety_init(&g, 0);
    for (int t = 1; t < 300; ++t) {
        frame(&g, S(t), 800);
    }
    frame(&g, S(300), 0);
    assert(g.mean_1h_x10 == 0);
    puts("PASS 1-h mean: 29.9 ppm never trips, 30.0 trips; warm-up samples are not averaged");
}

static void test_hysteresis_instant(void)
{
    co_safety_init(&g, 0);   /* stay in warm-up so only the instant rule acts */
    frame(&g, S(1), 950);
    assert(g.alarm);
    /* hovering just under 90 but above 81: no chatter, stays ALARM */
    for (int t = 2; t < 122; ++t) {
        frame(&g, S(t), (t % 2) ? 850 : 899);
        assert(g.alarm);
    }
    /* clear at 122; still ALARM at +59 s, released at +60 s */
    for (int t = 122; t < 182; ++t) {
        frame(&g, S(t), 800);
        assert(g.alarm);
    }
    frame(&g, S(182), 800);
    assert(!g.alarm);
    frame(&g, S(183), 899);
    assert(!g.alarm);
    frame(&g, S(184), 900);
    assert(g.alarm && g.alarms_entered == 2);
    puts("PASS hysteresis: 85-89.9 keeps ALARM, < 81 for 60 s releases it, re-entry at 90 is immediate");
}

static void test_clearing_restarts(void)
{
    co_safety_init(&g, 0);
    frame(&g, S(1), 950);
    for (int t = 2; t < 32; ++t) {
        frame(&g, S(t), 800);
    }
    frame(&g, S(32), 810);   /* not < 81.0: wait restarts */
    for (int t = 33; t < 93; ++t) {
        frame(&g, S(t), 800);
        assert(g.alarm);
    }
    frame(&g, S(93), 800);
    assert(!g.alarm);

    /* a gap long enough for FAULT also restarts the wait; ALARM survives */
    co_safety_init(&g, 0);
    frame(&g, S(1), 950);
    for (int t = 2; t < 32; ++t) {
        frame(&g, S(t), 800);
    }
    co_safety_tick(&g, S(38) + 1);
    assert(g.fault && g.alarm);
    int t = 39;
    for (; t < 99; ++t) {
        frame(&g, S(t), 800);
        assert(g.alarm);
    }
    assert(!g.fault);
    frame(&g, S(99), 800);
    assert(!g.alarm);
    puts("PASS one sample at 81.0 or a sensor gap restarts the 60 s release wait; ALARM stays through FAULT");
}

static void test_saturation(void)
{
    const uint16_t vals[] = {5000, 5001, 0xFFFF};
    for (size_t i = 0; i < 3; ++i) {
        co_safety_init(&g, 0);
        frame(&g, S(1), vals[i]);
        assert(g.alarm && !g.fault);
    }
    puts("PASS full-scale and above (500.0 ppm, 6553.5 ppm) = ALARM, not FAULT");
}

static void test_mean_decay_vs_reference(void)
{
    co_safety_init(&g, 0);
    ref_init(&R, 0);
    int64_t entered = -1, left = -1;
    for (int t = 300; t < 300 + 110 * 60; ++t) {
        const uint16_t x = t < 300 + 20 * 60 ? 400 : 0;   /* 40 ppm 20 min, then 0 */
        frame(&g, S(t), x);
        ref_frame(&R, S(t), x);
        ref_check(&R, &g, S(t));
        if (g.alarm && entered < 0) entered = t;
        if (!g.alarm && entered >= 0 && left < 0) left = t;
    }
    assert(entered == 300);
    /* after 40 ppm stops, the 1-h mean needs a while to fall under 27 */
    assert(left > 300 + 20 * 60 + 9 * 60);
    printf("PASS 40 ppm for 20 min: ALARM at once (1-h mean), released %d s after the CO stopped; "
           "means match the brute-force reference every second\n", (int)(left - (300 + 20 * 60)));
}

static void test_elevated_8h(void)
{
    co_safety_init(&g, 0);
    for (int t = 300; t < 400; ++t) frame(&g, S(t), 90);
    assert(g.elevated && !g.alarm);
    co_safety_init(&g, 0);
    for (int t = 300; t < 400; ++t) frame(&g, S(t), 89);
    assert(!g.elevated);
    puts("PASS 8-h mean 9.0 ppm sets the elevated flag (no ALARM); 8.9 does not");
}

static void test_random_vs_reference(void)
{
    co_safety_init(&g, 0);
    ref_init(&R, 0);
    int v = 0;
    uint32_t steps = 0, alarms = 0, faults = 0, elev = 0, elev_changes = 0;
    bool prev_elev = false;
    int64_t t = S(1);
    while (t < S(16 * 3600)) {
        /* quiet room most of the time; busy hours 1-2 and 9 (bursts that
         * can cross 90 ppm and push the 1-h / 8-h means); occasional bad
         * frames and gaps (some long enough for FAULT) */
        const int hour = (int)(t / S(3600));
        const bool busy = hour == 1 || hour == 2 || hour == 9;
        const uint32_t r = rnd(1000);
        if (r < 3) {
            t += S(4 + rnd(10));          /* gap: 4..13 s */
        } else if (r < 6) {
            co_safety_on_bad_frame(&g);
            R.streak = 0;
        } else {
            if (busy && r < 20) v += (int)rnd(700);
            if (!busy && r < 8) v += (int)rnd(120);
            v += (int)rnd(41) - 25;
            if (v < 0) v = 0;
            if (v > 1500) v = 1500;
            co_safety_on_frame(&g, t, (uint16_t)v);
            ref_frame(&R, t, (uint16_t)v);
        }
        co_safety_tick(&g, t);
        ref_tick(&R, t);
        ref_check(&R, &g, t);
        alarms += g.alarm; faults += g.fault; elev += g.elevated;
        elev_changes += g.elevated != prev_elev;
        prev_elev = g.elevated;
        ++steps;
        t += S(1);
    }
    assert(g.alarms_entered >= 5 && alarms < steps / 2);
    assert(g.faults_entered >= 5 && elev_changes >= 2);
    printf("PASS 16 h random sequence (%u steps, seed 2026): alarm/elevated/fault/means equal the "
           "reference every step (ALARM entered %u times, %u steps; FAULT entered %u; "
           "elevated changed %u times, %u steps)\n",
           (unsigned)steps, (unsigned)g.alarms_entered, (unsigned)alarms,
           (unsigned)g.faults_entered, (unsigned)elev_changes, (unsigned)elev);
    (void)faults;
}

static void test_status_helpers(void)
{
    co_safety_init(&g, 0);
    frame(&g, S(400), 37);
    co_status_t st = status_at(&g, S(400));
    uint16_t v = 0;
    assert(co_status_csv_value(&st, S(400), MS(2500), &v) && v == 37);
    assert(co_status_csv_value(&st, S(400) + MS(2500), MS(2500), &v));
    assert(!co_status_csv_value(&st, S(400) + MS(2500) + 1, MS(2500), &v));
    assert(!co_status_csv_value(&st, S(400) - 1, MS(2500), &v));   /* from the future */
    st.fault = true;
    assert(!co_status_csv_value(&st, S(400), MS(2500), &v));
    co_safety_init(&g, 0);
    st = status_at(&g, S(400));
    assert(!co_status_csv_value(&st, S(400), MS(2500), &v));      /* no frame yet */

    st.updated_us = S(10);
    assert(!co_status_stale(&st, S(16)));
    assert(co_status_stale(&st, S(16) + 1));
    puts("PASS co_ppm only when not warm-up/FAULT and frame <= 2.5 s old; snapshot stale after 6 s");
}

/* ------------------------------ co_alert ----------------------------- */

static co_status_t plain(int64_t now)
{
    co_status_t st;
    memset(&st, 0, sizeof(st));
    st.updated_us = now;
    st.have_frame = true;
    st.frame_us = now;
    return st;
}

static void test_alert_inactive(void)
{
    co_alert_t a;
    co_alert_init(&a);
    co_alert_out_t o = co_alert_eval(&a, NULL, S(1));
    assert(!o.active && o.next_change_us == INT64_MAX);
    co_status_t st = plain(S(1));
    o = co_alert_eval(&a, &st, S(1));
    assert(!o.active && !o.red && !o.buzzer && o.next_change_us == S(7) + 1);
    assert(!co_alert_ack(&a, S(1)));
    puts("PASS no CO path / normal CO: not active; ACK does nothing");
}

static void test_alert_alarm_pattern_and_ack(void)
{
    co_alert_t a;
    co_alert_init(&a);
    const int64_t t0 = S(100);
    co_status_t st = plain(t0);
    st.alarm = true;
    st.alarm_reasons = CO_REASON_MEAN_1H;
    co_alert_out_t o = co_alert_eval(&a, &st, t0);
    assert(o.active && o.alarm && o.alarm_entered && o.red && o.buzzer);
    assert(o.next_change_us == t0 + MS(100));
    o = co_alert_eval(&a, &st, t0 + MS(100) - 1);
    assert(o.red && !o.alarm_entered);
    o = co_alert_eval(&a, &st, t0 + MS(100));
    assert(!o.red && o.buzzer && o.next_change_us == t0 + MS(200));
    o = co_alert_eval(&a, &st, t0 + MS(200));
    assert(o.red);
    puts("PASS ALARM: LED_R 100 ms on / 100 ms off (5 Hz) from entry, buzzer continuous");

    const int64_t tack = t0 + S(3);
    st.updated_us = tack;
    o = co_alert_eval(&a, &st, tack);
    assert(co_alert_ack(&a, tack));
    o = co_alert_eval(&a, &st, tack);
    assert(o.active && o.alarm && !o.buzzer && o.muted);   /* ACK never clears ALARM */
    st.updated_us = tack + CO_ACK_MUTE_US - 1;
    o = co_alert_eval(&a, &st, tack + CO_ACK_MUTE_US - 1);
    assert(!o.buzzer && o.alarm);
    st.updated_us = tack + CO_ACK_MUTE_US;
    o = co_alert_eval(&a, &st, tack + CO_ACK_MUTE_US);
    assert(o.buzzer && !o.muted);
    puts("PASS ACK in ALARM: buzzer muted 5 min, LED_R keeps blinking, ALARM not cleared, sound returns");

    /* escalation: muted on the 1-h mean, then instant >= 90 appears */
    int64_t t = tack + CO_ACK_MUTE_US + S(1);
    st.updated_us = t;
    co_alert_eval(&a, &st, t);
    assert(co_alert_ack(&a, t));
    o = co_alert_eval(&a, &st, t + S(1));
    assert(!o.buzzer);
    st.alarm_reasons = CO_REASON_MEAN_1H | CO_REASON_INSTANT;
    st.updated_us = t + S(2);
    o = co_alert_eval(&a, &st, t + S(2));
    assert(o.buzzer);
    puts("PASS a new ALARM condition while muted brings the buzzer back at once");

    /* ALARM ends -> inactive; next ALARM is loud again */
    st.alarm = false;
    st.alarm_reasons = 0;
    st.updated_us = t + S(3);
    o = co_alert_eval(&a, &st, t + S(3));
    assert(!o.active);
    co_alert_ack(&a, t + S(3));
    st.alarm = true;
    st.alarm_reasons = CO_REASON_INSTANT;
    st.updated_us = t + S(4);
    o = co_alert_eval(&a, &st, t + S(4));
    assert(o.alarm_entered && o.buzzer && o.red);
    puts("PASS after ALARM ends the mute is gone; the next ALARM starts loud with LED on");
}

static void test_alert_fault_pattern(void)
{
    co_alert_t a;
    co_alert_init(&a);
    const int64_t f0 = S(50);
    co_status_t st = plain(f0);
    st.fault = true;
    const struct { int64_t dt; bool red; bool buz; } exp[] = {
        {0, true, true}, {MS(100) - 1, true, true}, {MS(100), false, false},
        {MS(200), true, false}, {MS(300) - 1, true, false}, {MS(300), false, false},
        {MS(2000), true, false}, {MS(2100), false, false}, {MS(2200), true, false},
        {S(60), true, true}, {S(60) + MS(100), false, false},
    };
    for (size_t i = 0; i < sizeof(exp) / sizeof(exp[0]); ++i) {
        st.updated_us = f0 + exp[i].dt;
        const co_alert_out_t o = co_alert_eval(&a, &st, f0 + exp[i].dt);
        assert(o.active && o.fault && !o.alarm);
        assert(o.red == exp[i].red && o.buzzer == exp[i].buz);
        assert(o.fault_entered == (i == 0));
    }
    puts("PASS FAULT: LED_R double blink every 2 s, chirp 100 ms every 60 s");

    assert(co_alert_ack(&a, f0 + S(61)));
    st.updated_us = f0 + S(120);
    co_alert_out_t o = co_alert_eval(&a, &st, f0 + S(120));
    assert(o.red && !o.buzzer && o.muted);
    st.fault = false;
    st.updated_us = f0 + S(121);
    o = co_alert_eval(&a, &st, f0 + S(121));
    assert(!o.active);
    st.fault = true;
    st.updated_us = f0 + S(122);
    o = co_alert_eval(&a, &st, f0 + S(122));
    assert(o.fault_entered && o.buzzer);
    puts("PASS ACK in FAULT mutes the chirp only until FAULT ends; LED keeps blinking");
}

static void test_alert_priority_and_stale(void)
{
    co_alert_t a;
    co_alert_init(&a);
    co_status_t st = plain(S(10));
    st.alarm = true;
    st.fault = true;
    co_alert_out_t o = co_alert_eval(&a, &st, S(10));
    assert(o.alarm && o.fault && o.buzzer && o.red);
    o = co_alert_eval(&a, &st, S(10) + MS(400));
    /* 5 Hz ALARM rhythm (on at 400 ms) and continuous buzzer, not the FAULT
     * double blink (off at 400 ms) or its 100 ms chirp */
    assert(o.red && o.buzzer);

    co_alert_init(&a);
    st = plain(S(10));
    o = co_alert_eval(&a, &st, S(16));
    assert(!o.active);
    o = co_alert_eval(&a, &st, S(16) + 1);   /* CO task stopped publishing */
    assert(o.active && o.fault && o.fault_entered);
    puts("PASS ALARM wins over FAULT; a snapshot older than 6 s is shown as FAULT");
}

static void test_alert_next_change(void)
{
    /* Output never changes before next_change_us and the schedule never
     * stalls: walk 130 s of ALARM-with-mute and FAULT by next_change. */
    for (int mode = 0; mode < 2; ++mode) {
        co_alert_t a;
        co_alert_init(&a);
        int64_t t = S(5);
        co_status_t st = plain(t);
        st.alarm = mode == 0;
        st.fault = mode == 1;
        st.alarm_reasons = CO_REASON_INSTANT;
        co_alert_eval(&a, &st, t);
        co_alert_ack(&a, t + MS(330));
        unsigned changes = 0;
        while (t < S(135)) {
            st.updated_us = t;   /* keep the snapshot fresh */
            const co_alert_out_t o = co_alert_eval(&a, &st, t);
            assert(o.next_change_us > t);
            const int64_t probe = t + (o.next_change_us - t) / 2;
            st.updated_us = probe;
            const co_alert_out_t mid = co_alert_eval(&a, &st, probe);
            assert(mid.red == o.red && mid.buzzer == o.buzzer);
            st.updated_us = o.next_change_us - 1;
            const co_alert_out_t last = co_alert_eval(&a, &st, o.next_change_us - 1);
            assert(last.red == o.red && last.buzzer == o.buzzer);
            t = o.next_change_us;
            ++changes;
        }
        assert(changes > 100);
    }
    puts("PASS next_change_us: outputs constant until it, schedule always moves forward");
}

int main(void)
{
    test_boot_silence_and_hw_fault();
    test_fault_exit_needs_three_good_frames();
    test_instant_threshold_and_warmup();
    test_mean_threshold_exact();
    test_hysteresis_instant();
    test_clearing_restarts();
    test_saturation();
    test_mean_decay_vs_reference();
    test_elevated_8h();
    test_random_vs_reference();
    test_status_helpers();
    test_alert_inactive();
    test_alert_alarm_pattern_and_ack();
    test_alert_fault_pattern();
    test_alert_priority_and_stale();
    test_alert_next_change();
    puts("ALL CO SAFETY TESTS PASSED");
    return 0;
}
