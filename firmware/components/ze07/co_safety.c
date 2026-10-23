#include "co_safety.h"

#include <string.h>

typedef struct {
    uint64_t sum_x10;
    uint32_t count;
} window_t;

static bool warming_up(const co_safety_t *s, int64_t now_us)
{
    return now_us - s->start_us < CO_WARMUP_US;
}

static int32_t minute_of(const co_safety_t *s, int64_t now_us)
{
    const int64_t m = (now_us - s->start_us) / CO_MINUTE_US;
    return m < 0 ? 0 : (int32_t)m;
}

static void add_sample(co_safety_t *s, int64_t now_us, uint16_t x10)
{
    const int32_t minute = minute_of(s, now_us);
    co_bucket_t *b = &s->bucket[(uint32_t)minute % CO_WINDOW_8H_MIN];
    if (b->minute != minute) {
        b->minute = minute;
        b->sum_x10 = 0;
        b->count = 0;
    }
    if (b->count == UINT16_MAX) {
        return;   /* > 1000 frames/s: cannot happen at 9600 baud */
    }
    b->sum_x10 += x10;   /* <= 65535 * 65535 < 2^32 */
    ++b->count;
}

/* Buckets of the last `minutes` minutes including the current one. */
static window_t window(const co_safety_t *s, int64_t now_us, uint32_t minutes)
{
    const int32_t cur = minute_of(s, now_us);
    window_t w = { 0, 0 };
    for (uint32_t i = 0; i < CO_WINDOW_8H_MIN; ++i) {
        const co_bucket_t *b = &s->bucket[i];
        if (b->minute < 0 || b->minute > cur || cur - b->minute >= (int32_t)minutes) {
            continue;
        }
        w.sum_x10 += b->sum_x10;
        w.count += b->count;
    }
    return w;
}

/* mean >= limit, exact in integers: sum / count >= limit <=> sum >= limit * count */
static bool mean_at_least(window_t w, uint32_t limit_x10)
{
    return w.count > 0 && w.sum_x10 >= (uint64_t)limit_x10 * w.count;
}

static uint32_t mean_of(window_t w)
{
    return w.count > 0 ? (uint32_t)(w.sum_x10 / w.count) : 0U;
}

/* Hysteresis exit shared by ALARM and the 8-h flag: `clear` must hold on
 * every valid sample for CO_CLEAR_HOLD_US; any sample that is not clear
 * restarts the wait. */
static bool hold_clear(bool clear, bool *clearing, int64_t *since_us, int64_t now_us)
{
    if (!clear) {
        *clearing = false;
        return false;
    }
    if (!*clearing) {
        *clearing = true;
        *since_us = now_us;
        return false;
    }
    return now_us - *since_us >= CO_CLEAR_HOLD_US;
}

void co_safety_init(co_safety_t *s, int64_t now_us)
{
    memset(s, 0, sizeof(*s));
    s->start_us = now_us;
    for (uint32_t i = 0; i < CO_WINDOW_8H_MIN; ++i) {
        s->bucket[i].minute = -1;
    }
}

void co_safety_on_frame(co_safety_t *s, int64_t now_us, uint16_t conc_x10)
{
    ++s->frames_ok;
    s->have_frame = true;
    s->last_valid_us = now_us;
    s->last_x10 = conc_x10;

    if (s->good_streak < UINT32_MAX) {
        ++s->good_streak;
    }
    if (s->fault && !s->hw_fault && s->good_streak >= CO_FAULT_EXIT_FRAMES) {
        s->fault = false;
    }

    if (!warming_up(s, now_us)) {
        add_sample(s, now_us, conc_x10);
    }
    const window_t w1 = window(s, now_us, CO_WINDOW_1H_MIN);
    const window_t w8 = window(s, now_us, CO_WINDOW_8H_MIN);
    s->mean_1h_x10 = mean_of(w1);
    s->mean_8h_x10 = mean_of(w8);

    uint8_t reasons = 0;
    if (conc_x10 >= CO_INSTANT_ON_X10) {
        reasons |= CO_REASON_INSTANT;
    }
    if (mean_at_least(w1, CO_MEAN_1H_ON_X10)) {
        reasons |= CO_REASON_MEAN_1H;
    }
    s->reasons = reasons;

    if (reasons != 0) {
        if (!s->alarm) {
            s->alarm = true;   /* immediately: no persistence on entry */
            ++s->alarms_entered;
        }
        s->alarm_clearing = false;
    } else if (s->alarm) {
        const bool clear = conc_x10 < CO_INSTANT_OFF_X10 &&
                           !mean_at_least(w1, CO_MEAN_1H_OFF_X10);
        if (hold_clear(clear, &s->alarm_clearing, &s->alarm_clear_since_us, now_us)) {
            s->alarm = false;
            s->alarm_clearing = false;
        }
    }

    if (mean_at_least(w8, CO_MEAN_8H_ON_X10)) {
        s->elevated = true;
        s->elevated_clearing = false;
    } else if (s->elevated) {
        const bool clear = !mean_at_least(w8, CO_MEAN_8H_OFF_X10);
        if (hold_clear(clear, &s->elevated_clearing, &s->elevated_clear_since_us, now_us)) {
            s->elevated = false;
            s->elevated_clearing = false;
        }
    }
}

void co_safety_on_bad_frame(co_safety_t *s)
{
    ++s->frames_bad;
    s->good_streak = 0;
}

void co_safety_set_hw_fault(co_safety_t *s)
{
    s->hw_fault = true;
    if (!s->fault) {
        s->fault = true;
        ++s->faults_entered;
    }
}

void co_safety_tick(co_safety_t *s, int64_t now_us)
{
    const int64_t ref = s->have_frame ? s->last_valid_us : s->start_us;
    if (s->hw_fault || now_us - ref > CO_SILENCE_US) {
        if (!s->fault) {
            s->fault = true;
            ++s->faults_entered;
        }
        s->good_streak = 0;
        /* no valid samples: the exit wait cannot continue across the gap */
        s->alarm_clearing = false;
        s->elevated_clearing = false;
    }
}

void co_safety_status(const co_safety_t *s, int64_t now_us, co_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->frame_us = s->last_valid_us;
    out->conc_x10 = s->last_x10;
    out->mean_1h_x10 = (uint16_t)(s->mean_1h_x10 > UINT16_MAX ? UINT16_MAX : s->mean_1h_x10);
    out->mean_8h_x10 = (uint16_t)(s->mean_8h_x10 > UINT16_MAX ? UINT16_MAX : s->mean_8h_x10);
    out->have_frame = s->have_frame;
    out->warmup = warming_up(s, now_us);
    out->alarm = s->alarm;
    out->fault = s->fault;
    out->elevated_8h = s->elevated;
    out->alarm_reasons = s->reasons;
    if (s->hw_fault) {
        out->fault_reasons = CO_FAULT_HW;
    } else if (s->fault) {
        /* also while recovering (fewer than CO_FAULT_EXIT_FRAMES good
         * frames since the silence) */
        out->fault_reasons = CO_FAULT_SILENT;
    }
}

bool co_status_stale(const co_status_t *st, int64_t now_us)
{
    return now_us - st->updated_us > CO_SILENCE_US;
}

bool co_status_csv_value(const co_status_t *st, int64_t now_us,
                         int64_t max_age_us, uint16_t *conc_x10)
{
    if (!st->have_frame || st->warmup || st->fault ||
        now_us < st->frame_us || now_us - st->frame_us > max_age_us) {
        return false;
    }
    *conc_x10 = st->conc_x10;
    return true;
}
