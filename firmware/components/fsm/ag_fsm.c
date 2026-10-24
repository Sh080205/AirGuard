#include "ag_fsm.h"

#include <stddef.h>

/* One output's blink pattern: on during [on[i][0], on[i][1]) of every
 * period. period == 0: steady at `steady`. */
typedef struct {
    int64_t period;
    unsigned n;
    int64_t on[AG_FSM_BEEPS][2];
    bool steady;
} wave_t;

static const wave_t WAVE_OFF = { 0, 0, { { 0, 0 } }, false };
static const wave_t WAVE_ON = { 0, 0, { { 0, 0 } }, true };

static wave_t wave_one(int64_t period, int64_t on_us)
{
    return (wave_t){ period, 1, { { 0, on_us } }, false };
}

static int64_t phase_of(const wave_t *w, int64_t t)
{
    const int64_t p = t % w->period;
    return p < 0 ? p + w->period : p;
}

static bool wave_level(const wave_t *w, int64_t t)
{
    if (w->period == 0) {
        return w->steady;
    }
    const int64_t p = phase_of(w, t);
    for (unsigned i = 0; i < w->n; ++i) {
        if (p >= w->on[i][0] && p < w->on[i][1]) {
            return true;
        }
    }
    return false;
}

/* time from t to the next boundary of w (> 0), INT64_MAX when steady */
static int64_t wave_until_edge(const wave_t *w, int64_t t)
{
    if (w->period == 0) {
        return INT64_MAX;
    }
    const int64_t p = phase_of(w, t);
    int64_t best = w->period - p;           /* next period start */
    for (unsigned i = 0; i < w->n; ++i) {
        for (unsigned k = 0; k < 2; ++k) {
            const int64_t e = w->on[i][k];
            if (e > p && e - p < best) {
                best = e - p;
            }
        }
    }
    return best;
}

typedef struct {
    wave_t green, yellow, red, buzzer;
} waves_t;

static waves_t waves_for(ag_fsm_show_t shown, ag_fsm_ind_t ind, bool muted)
{
    waves_t w = { WAVE_OFF, WAVE_OFF, WAVE_OFF, WAVE_OFF };
    switch (shown) {
    case AG_FSM_SHOW_ML_ALARM:
        w.red = wave_one(AG_FSM_ALARM_PERIOD_US, AG_FSM_ALARM_ON_US);
        if (!muted) {
            w.buzzer.period = AG_FSM_BEEP_PERIOD_US;
            w.buzzer.n = AG_FSM_BEEPS;
            for (unsigned i = 0; i < AG_FSM_BEEPS; ++i) {
                w.buzzer.on[i][0] = (int64_t)(2U * i) * AG_FSM_BEEP_US;
                w.buzzer.on[i][1] = (int64_t)(2U * i + 1U) * AG_FSM_BEEP_US;
            }
        }
        break;
    case AG_FSM_SHOW_SYS_FAULT:
        w.yellow.period = AG_FSM_FAULT_PERIOD_US;
        w.yellow.n = 2;
        w.yellow.on[0][0] = 0;
        w.yellow.on[0][1] = AG_FSM_FAULT_BLINK_US;
        w.yellow.on[1][0] = 2 * AG_FSM_FAULT_BLINK_US;
        w.yellow.on[1][1] = 3 * AG_FSM_FAULT_BLINK_US;
        break;
    case AG_FSM_SHOW_ML_WATCH:
        w.yellow = wave_one(AG_FSM_WATCH_PERIOD_US, AG_FSM_WATCH_ON_US);
        break;
    case AG_FSM_SHOW_NORMAL:
        switch (ind) {
        case AG_FSM_IND_WAIT_TIME:
            w.green = wave_one(AG_FSM_IND_PERIOD_US, AG_FSM_IND_WAIT_ON_US);
            break;
        case AG_FSM_IND_MARKED:
            w.green = wave_one(AG_FSM_IND_PERIOD_US, AG_FSM_IND_MARKED_ON_US);
            break;
        case AG_FSM_IND_RUNNING:
        default:
            w.green = WAVE_ON;
            break;
        }
        break;
    case AG_FSM_SHOW_CO:
    default:
        break;   /* the io task shows co_alert */
    }
    return w;
}

void ag_fsm_init(ag_fsm_t *f)
{
    if (f == NULL) {
        return;
    }
    *f = (ag_fsm_t){ .level = AG_FSM_LEVEL_NONE, .shown = AG_FSM_SHOW_NORMAL,
                     .ind = AG_FSM_IND_RUNNING };
}

/* One new ML sample (each published result is counted once). */
static void ml_sample(ag_fsm_t *f, ag_fsm_ml_t status, uint16_t streak, int64_t at)
{
    f->ml_seen = true;
    f->ml_last_at_us = at;
    switch (status) {
    case AG_FSM_ML_PERSISTENT: {
        const ag_fsm_level_t target = streak >= AG_FSM_ML_ALARM_STREAK ?
                                      AG_FSM_LEVEL_ALARM : AG_FSM_LEVEL_WATCH;
        if (target > f->level) {
            f->level = target;
        }
        f->clearing = false;
        break;
    }
    case AG_FSM_ML_ABOVE:
        /* over the threshold, not yet persistent: not evidence of "clear" */
        f->clearing = false;
        break;
    case AG_FSM_ML_BELOW:
        if (f->level == AG_FSM_LEVEL_NONE) {
            f->clearing = false;
        } else if (!f->clearing || at < f->clear_since_us) {
            f->clearing = true;
            f->clear_since_us = at;
        } else if (at - f->clear_since_us >= AG_FSM_ML_CLEAR_US) {
            /* one level per AG_FSM_ML_CLEAR_US */
            f->level = (ag_fsm_level_t)(f->level - 1);
            f->clear_since_us = at;
            f->clearing = f->level != AG_FSM_LEVEL_NONE;
        }
        break;
    case AG_FSM_ML_MISSING:
    default:
        f->clearing = false;   /* clearing needs consecutive valid samples */
        return;                /* not a valid sample */
    }
    f->have_valid = true;
    f->ml_valid_us = at;
}

static uint8_t ml_update(ag_fsm_t *f, const ag_fsm_in_t *in, int64_t now)
{
    if (!in->ml_enabled) {
        f->level = AG_FSM_LEVEL_NONE;
        f->clearing = false;
        return 0;
    }
    if (in->ml_status == AG_FSM_ML_MODEL_FAULT) {
        /* never show WATCH/ALARM from a model that failed its self-test */
        f->level = AG_FSM_LEVEL_NONE;
        f->clearing = false;
        f->ml_seen = true;
        f->ml_last_at_us = in->ml_at_us;
        return AG_FSM_FAULT_ML_MODEL;
    }
    if (in->ml_status != AG_FSM_ML_NONE && (!f->ml_seen || in->ml_at_us != f->ml_last_at_us)) {
        ml_sample(f, in->ml_status, in->ml_streak, in->ml_at_us);
    }
    if (f->level != AG_FSM_LEVEL_NONE && f->have_valid &&
        now - f->ml_valid_us > AG_FSM_ML_DROP_US) {
        f->level = AG_FSM_LEVEL_NONE;
        f->clearing = false;
    }
    const bool stale = f->ml_seen ? now - f->ml_last_at_us > AG_FSM_ML_STALE_US
                                  : now >= AG_FSM_ML_BOOT_GRACE_US;
    return stale ? AG_FSM_FAULT_ML_STALE : 0;
}

static bool sensor_fault(int64_t ok_us, int64_t grace_us, int64_t limit_us, int64_t now)
{
    int64_t from = ok_us >= 0 ? ok_us : 0;
    if (from < grace_us) {
        from = grace_us;
    }
    return now >= from && now - from >= limit_us;
}

static uint8_t health_update(const ag_fsm_in_t *in, int64_t now)
{
    uint8_t r = 0;
    if ((in->monitored & AG_FSM_SENSOR_AHT21) != 0U &&
        sensor_fault(in->aht_ok_us, AG_FSM_AHT_GRACE_US, AG_FSM_AHT_LIMIT_US, now)) {
        r |= AG_FSM_FAULT_AHT21;
    }
    if ((in->monitored & AG_FSM_SENSOR_ENS160) != 0U &&
        sensor_fault(in->ens_ok_us, AG_FSM_ENS_GRACE_US, AG_FSM_ENS_LIMIT_US, now)) {
        r |= AG_FSM_FAULT_ENS160;
    }
    if ((in->monitored & AG_FSM_SENSOR_PMS7003) != 0U &&
        sensor_fault(in->pms_ok_us, AG_FSM_PMS_GRACE_US, AG_FSM_PMS_LIMIT_US, now)) {
        r |= AG_FSM_FAULT_PMS7003;
    }
    return r;
}

static void fault_update(ag_fsm_t *f, uint8_t reasons, int64_t now)
{
    if (reasons != 0U) {
        f->fault = true;
        f->fault_clearing = false;
    } else if (f->fault) {
        if (!f->fault_clearing) {
            f->fault_clearing = true;
            f->fault_clear_since_us = now;
        } else if (now - f->fault_clear_since_us >= AG_FSM_FAULT_EXIT_US) {
            f->fault = false;
            f->fault_clearing = false;
        }
    }
}

static ag_fsm_show_t real_shown(const ag_fsm_t *f, bool co_active)
{
    if (co_active) {
        return AG_FSM_SHOW_CO;
    }
    if (f->level == AG_FSM_LEVEL_ALARM) {
        return AG_FSM_SHOW_ML_ALARM;
    }
    if (f->fault) {
        return AG_FSM_SHOW_SYS_FAULT;
    }
    if (f->level == AG_FSM_LEVEL_WATCH) {
        return AG_FSM_SHOW_ML_WATCH;
    }
    return AG_FSM_SHOW_NORMAL;
}

static const ag_fsm_show_t DEMO_ORDER[] = {
    AG_FSM_SHOW_NORMAL, AG_FSM_SHOW_ML_WATCH, AG_FSM_SHOW_ML_ALARM, AG_FSM_SHOW_SYS_FAULT,
};
#define DEMO_STEPS ((int64_t)(sizeof(DEMO_ORDER) / sizeof(DEMO_ORDER[0])))

ag_fsm_out_t ag_fsm_step(ag_fsm_t *f, const ag_fsm_in_t *in, int64_t now_us)
{
    static const ag_fsm_in_t NO_INPUT = { .ml_enabled = false, .monitored = 0,
                                          .aht_ok_us = -1, .ens_ok_us = -1, .pms_ok_us = -1,
                                          .indication = AG_FSM_IND_RUNNING };
    ag_fsm_out_t out = { .shown = AG_FSM_SHOW_NORMAL, .next_change_us = INT64_MAX };
    if (f == NULL) {
        return out;
    }
    if (in == NULL) {
        in = &NO_INPUT;
    }
    const bool first = !f->started;
    if (!first && now_us < f->last_now_us) {
        now_us = f->last_now_us;   /* esp_timer is monotonic; guard anyway */
    }
    f->started = true;
    f->last_now_us = now_us;

    const uint8_t reasons = (uint8_t)(ml_update(f, in, now_us) | health_update(in, now_us));
    fault_update(f, reasons, now_us);
    if (f->level != AG_FSM_LEVEL_ALARM) {
        f->alarm_muted = false;    /* Q4: muted until this ML ALARM ends */
    }

    if (in->co_active || (f->demo && now_us - f->demo_start_us >= AG_FSM_DEMO_US)) {
        f->demo = false;
    }
    ag_fsm_show_t shown = real_shown(f, in->co_active);
    int64_t demo_step = 0;
    int64_t origin = f->origin_us;
    if (f->demo) {
        demo_step = (now_us - f->demo_start_us) / AG_FSM_DEMO_STEP_US;
        shown = DEMO_ORDER[demo_step % DEMO_STEPS];
    }
    const ag_fsm_ind_t ind = f->demo ? AG_FSM_IND_RUNNING : in->indication;

    out.changed = first || shown != f->shown || f->demo != f->shown_demo;
    /* restart the pattern so it always begins with its "on" part, as
     * tasks_io.c does on every mode/indication change */
    if (out.changed || (f->demo && demo_step != f->demo_step) ||
        (shown == AG_FSM_SHOW_NORMAL && ind != f->ind)) {
        origin = f->demo ? f->demo_start_us + demo_step * AG_FSM_DEMO_STEP_US : now_us;
    }
    f->shown = shown;
    f->shown_demo = f->demo;
    f->demo_step = demo_step;
    f->ind = ind;
    f->origin_us = origin;

    const bool muted = !f->demo && f->alarm_muted;
    const waves_t w = waves_for(shown, ind, muted);
    const int64_t t = now_us - origin;
    out.shown = shown;
    out.demo = f->demo;
    out.green = wave_level(&w.green, t);
    out.yellow = wave_level(&w.yellow, t);
    out.red = wave_level(&w.red, t);
    out.buzzer = wave_level(&w.buzzer, t);
    out.muted = muted && shown == AG_FSM_SHOW_ML_ALARM;
    out.ml_level = f->level;
    out.sys_fault = f->fault;
    out.fault_reasons = reasons;

    int64_t until = INT64_MAX;
    const wave_t *all[] = { &w.green, &w.yellow, &w.red, &w.buzzer };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const int64_t u = wave_until_edge(all[i], t);
        if (u < until) {
            until = u;
        }
    }
    if (f->demo) {
        const int64_t step_end = f->demo_start_us + (demo_step + 1) * AG_FSM_DEMO_STEP_US;
        if (step_end - now_us < until) {
            until = step_end - now_us;   /* the last step end is the demo end */
        }
    }
    out.next_change_us = until == INT64_MAX ? INT64_MAX : now_us + until;
    return out;
}

bool ag_fsm_ack(ag_fsm_t *f, int64_t now_us)
{
    (void)now_us;
    if (f == NULL || f->shown == AG_FSM_SHOW_CO) {
        return false;
    }
    if (f->demo) {
        f->demo = false;
        return true;
    }
    if (f->level == AG_FSM_LEVEL_ALARM && !f->alarm_muted) {
        f->alarm_muted = true;
        return true;
    }
    return false;
}

bool ag_fsm_mode_long_press(ag_fsm_t *f, int64_t now_us)
{
    if (f == NULL || f->shown == AG_FSM_SHOW_CO) {
        return false;
    }
    f->demo = !f->demo;
    if (f->demo) {
        f->demo_start_us = now_us;
    }
    return true;
}

const char *ag_fsm_show_name(ag_fsm_show_t shown)
{
    switch (shown) {
    case AG_FSM_SHOW_NORMAL: return "NORMAL";
    case AG_FSM_SHOW_ML_WATCH: return "ML WATCH";
    case AG_FSM_SHOW_SYS_FAULT: return "SYS FAULT";
    case AG_FSM_SHOW_ML_ALARM: return "ML ALARM";
    case AG_FSM_SHOW_CO: return "CO";
    default: return "?";
    }
}
