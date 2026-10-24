#include "co_alert.h"

#include <string.h>

static int64_t min64(int64_t a, int64_t b)
{
    return a < b ? a : b;
}

void co_alert_init(co_alert_t *a)
{
    memset(a, 0, sizeof(*a));
}

co_alert_out_t co_alert_eval(co_alert_t *a, const co_status_t *st, int64_t now_us)
{
    co_alert_out_t out;
    memset(&out, 0, sizeof(out));
    out.next_change_us = INT64_MAX;
    if (st == NULL) {
        return out;
    }

    const bool stale = co_status_stale(st, now_us);
    const bool fault = st->fault || stale;
    const bool alarm = st->alarm;

    if (alarm && !a->prev_alarm) {
        a->alarm_origin_us = now_us;   /* pattern starts with "on" */
        a->alarm_muted = false;
        out.alarm_entered = true;
    }
    if (!alarm) {
        a->alarm_muted = false;
    }
    if (fault && !a->prev_fault) {
        a->fault_origin_us = now_us;
        a->fault_muted = false;
        out.fault_entered = true;
    }
    if (!fault) {
        a->fault_muted = false;
    }
    a->prev_alarm = alarm;
    a->prev_fault = fault;
    a->last_reasons = alarm ? st->alarm_reasons : 0U;

    if (a->alarm_muted &&
        (now_us >= a->mute_until_us || (st->alarm_reasons & (uint8_t)~a->muted_reasons) != 0)) {
        a->alarm_muted = false;
    }

    out.alarm = alarm;
    out.fault = fault;
    if (!stale) {
        /* re-evaluate when this snapshot turns stale */
        out.next_change_us = st->updated_us + CO_SILENCE_US + 1;
    }

    if (alarm) {
        out.active = true;
        const int64_t p = (now_us - a->alarm_origin_us) % CO_ALARM_PERIOD_US;
        out.red = p < CO_ALARM_ON_US;
        out.buzzer = !a->alarm_muted;
        out.muted = a->alarm_muted;
        const int64_t edge = p < CO_ALARM_ON_US ? CO_ALARM_ON_US : CO_ALARM_PERIOD_US;
        out.next_change_us = min64(out.next_change_us, now_us + (edge - p));
        if (a->alarm_muted) {
            out.next_change_us = min64(out.next_change_us, a->mute_until_us);
        }
        return out;
    }

    if (fault) {
        out.active = true;
        const int64_t p = (now_us - a->fault_origin_us) % CO_FAULT_PERIOD_US;
        out.red = p < CO_FAULT_BLINK_US ||
                  (p >= 2 * CO_FAULT_BLINK_US && p < 3 * CO_FAULT_BLINK_US);
        int64_t edge;
        if (p < CO_FAULT_BLINK_US) {
            edge = CO_FAULT_BLINK_US;
        } else if (p < 2 * CO_FAULT_BLINK_US) {
            edge = 2 * CO_FAULT_BLINK_US;
        } else if (p < 3 * CO_FAULT_BLINK_US) {
            edge = 3 * CO_FAULT_BLINK_US;
        } else {
            edge = CO_FAULT_PERIOD_US;
        }
        out.next_change_us = min64(out.next_change_us, now_us + (edge - p));

        out.muted = a->fault_muted;
        if (!a->fault_muted) {
            const int64_t c = (now_us - a->fault_origin_us) % CO_FAULT_CHIRP_PERIOD_US;
            out.buzzer = c < CO_FAULT_CHIRP_US;
            const int64_t cedge = c < CO_FAULT_CHIRP_US ? CO_FAULT_CHIRP_US : CO_FAULT_CHIRP_PERIOD_US;
            out.next_change_us = min64(out.next_change_us, now_us + (cedge - c));
        }
    }
    return out;
}

bool co_alert_ack(co_alert_t *a, int64_t now_us)
{
    if (a->prev_alarm) {
        a->alarm_muted = true;
        a->mute_until_us = now_us + CO_ACK_MUTE_US;
        a->muted_reasons = a->last_reasons;
        return true;
    }
    if (a->prev_fault) {
        a->fault_muted = true;
        return true;
    }
    return false;
}
