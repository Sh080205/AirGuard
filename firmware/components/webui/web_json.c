#include "web_json.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ag_fsm.h"

/* Same append-only writer as csv_row.c: any overflow turns the whole
 * result into "" + 0, so the web task never sends half an object. */
typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool ok;
} out_t;

static void out_init(out_t *o, char *buf, size_t cap)
{
    o->buf = buf;
    o->cap = cap;
    o->len = 0;
    o->ok = (buf != NULL && cap > 0);
    if (o->ok) {
        buf[0] = '\0';
    }
}

static void put(out_t *o, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void put(out_t *o, const char *fmt, ...)
{
    if (!o->ok) {
        return;
    }
    const size_t room = o->cap - o->len;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(o->buf + o->len, room, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= room) {
        o->ok = false;
        return;
    }
    o->len += (size_t)n;
}

static size_t out_finish(out_t *o)
{
    if (!o->ok) {
        if (o->buf != NULL && o->cap > 0) {
            o->buf[0] = '\0';
        }
        return 0;
    }
    return o->len;
}

/* JSON string (RFC 8259 §7): quote, backslash and control characters are
 * escaped; other bytes are copied (UTF-8 passes through). NULL -> null.
 * Longer than WEB_JSON_STR_MAX -> the whole output fails, which keeps the
 * documented worst-case size true. */
static void put_str(out_t *o, const char *s)
{
    if (s == NULL) {
        put(o, "null");
        return;
    }
    if (strlen(s) > WEB_JSON_STR_MAX) {
        o->ok = false;
        return;
    }
    put(o, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p) {
        if (*p == '"' || *p == '\\') {
            put(o, "\\%c", *p);
        } else if (*p < 0x20U) {
            put(o, "\\u%04x", (unsigned)*p);
        } else {
            put(o, "%c", *p);
        }
    }
    put(o, "\"");
}

static void put_bool(out_t *o, bool v)
{
    put(o, "%s", v ? "true" : "false");
}

/* One decimal exactly as csv_row.c writes temp_c/humidity_pct/co_ppm
 * (csv_round_tenths, no "-0.0"); a value the CSV leaves empty -> null. */
static void put_tenths_value(out_t *o, long t)
{
    const long mag = (t < 0) ? -t : t;
    put(o, "%s%ld.%ld", (t < 0) ? "-" : "", mag / 10, mag % 10);
}

static void put_tenths(out_t *o, bool has, float v)
{
    long t = 0;
    if (!has || !csv_round_tenths(v, &t)) {
        put(o, "null");
        return;
    }
    put_tenths_value(o, t);
}

/* D² and its threshold: "%.4f" like the AGML line (ag_mahal.h). A value
 * that is not finite is null; |v| >= 1e9 (never a real D², only possible
 * far outside every sensor range) uses %.4e so the width stays bounded. */
static void put_d2(out_t *o, float v)
{
    if (!isfinite(v)) {
        put(o, "null");
    } else if (fabsf(v) < 1e9f) {
        put(o, "%.4f", (double)v);
    } else {
        put(o, "%.4e", (double)v);
    }
}

static bool tm_valid(const struct tm *t)
{
    return t->tm_year >= -1900 && t->tm_year <= 9999 - 1900 &&
           t->tm_mon >= 0 && t->tm_mon <= 11 &&
           t->tm_mday >= 1 && t->tm_mday <= 31 &&
           t->tm_hour >= 0 && t->tm_hour <= 23 &&
           t->tm_min >= 0 && t->tm_min <= 59 &&
           t->tm_sec >= 0 && t->tm_sec <= 60;
}

/* ag_fsm.h AG_FSM_FAULT_* in bit order. */
static const char *const FAULT_NAMES[] = {
    "AHT21", "ENS160", "PMS7003", "ML_MODEL", "ML_STALE",
};
_Static_assert(AG_FSM_FAULT_AHT21 == 1U << 0 && AG_FSM_FAULT_ENS160 == 1U << 1 &&
               AG_FSM_FAULT_PMS7003 == 1U << 2 && AG_FSM_FAULT_ML_MODEL == 1U << 3 &&
               AG_FSM_FAULT_ML_STALE == 1U << 4,
               "FAULT_NAMES must follow the AG_FSM_FAULT_* bits");

/* main/env_alert.h ENV_R_* in bit order (tasks_web.c asserts it: this
 * component cannot include main/). */
static const char *const ENV_NAMES[] = {
    "PM_WATCH", "PM_ALARM", "RH_LOW", "RH_HIGH",
};

size_t web_json_live(const web_snapshot_t *s, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    if (s == NULL) {
        o.ok = false;
        return out_finish(&o);
    }
    const csv_sample_t *r = &s->row;

    put(&o, "{\"v\":%d,\"seq\":%lu,\"up_s\":%lld,\"t\":", WEB_JSON_VERSION,
        (unsigned long)s->seq, (long long)s->uptime_s);
    if (s->time_valid && tm_valid(&r->local_time)) {
        put(&o, "\"%04d-%02d-%02dT%02d:%02d:%02d+07:00\"",
            r->local_time.tm_year + 1900, r->local_time.tm_mon + 1,
            r->local_time.tm_mday, r->local_time.tm_hour,
            r->local_time.tm_min, r->local_time.tm_sec);
    } else {
        put(&o, "null");
    }

    if (r->has_pm) {
        put(&o, ",\"pm1_0\":%u,\"pm2_5\":%u,\"pm10\":%u", (unsigned)r->pm1_0,
            (unsigned)r->pm2_5, (unsigned)r->pm10);
    } else {
        put(&o, ",\"pm1_0\":null,\"pm2_5\":null,\"pm10\":null");
    }
    put(&o, ",\"temp_c\":");
    put_tenths(&o, r->has_th, r->temp_c);
    put(&o, ",\"humidity_pct\":");
    put_tenths(&o, r->has_th, r->humidity_pct);
    if (r->has_voc) {
        put(&o, ",\"tvoc_ppb\":%u,\"eco2_ppm\":%u", (unsigned)r->tvoc_ppb,
            (unsigned)r->eco2_ppm);
    } else {
        put(&o, ",\"tvoc_ppb\":null,\"eco2_ppm\":null");
    }
    put(&o, ",\"co_ppm\":");
    put_tenths(&o, r->has_co, r->co_ppm);
    put(&o, ",\"marked\":");
    put_bool(&o, r->marked);

    put(&o, ",\"shown\":");
    put_str(&o, s->shown);
    put(&o, ",\"demo\":");
    put_bool(&o, s->demo);
    put(&o, ",\"muted\":");
    put_bool(&o, s->muted);
    put(&o, ",\"sys_fault\":[");
    bool first = true;
    for (unsigned i = 0; i < sizeof(FAULT_NAMES) / sizeof(FAULT_NAMES[0]); ++i) {
        if (s->fault_reasons & (1U << i)) {
            put(&o, "%s\"%s\"", first ? "" : ",", FAULT_NAMES[i]);
            first = false;
        }
    }
    put(&o, "]");
    put(&o, ",\"env\":[");
    first = true;
    for (unsigned i = 0; i < sizeof(ENV_NAMES) / sizeof(ENV_NAMES[0]); ++i) {
        if (s->env_reasons & (1U << i)) {
            put(&o, "%s\"%s\"", first ? "" : ",", ENV_NAMES[i]);
            first = false;
        }
    }
    put(&o, "]");

    put(&o, ",\"ml\":");
    if (s->ml.present) {
        put(&o, "{\"status\":");
        put_str(&o, s->ml.status);
        put(&o, ",\"d2\":");
        put_d2(&o, s->ml.d2);
        put(&o, ",\"thr\":");
        put_d2(&o, s->ml.threshold_d2);
        put(&o, ",\"streak\":%u,\"model\":", (unsigned)s->ml.streak);
        put_str(&o, s->ml.model);
        put(&o, "}");
    } else {
        put(&o, "null");
    }

    put(&o, ",\"co\":");
    if (s->co.present) {
        put(&o, "{\"alarm\":");
        put_bool(&o, s->co.alarm);
        put(&o, ",\"fault\":");
        put_bool(&o, s->co.fault);
        put(&o, ",\"warmup\":");
        put_bool(&o, s->co.warmup);
        put(&o, ",\"stale\":");
        put_bool(&o, s->co.stale);
        put(&o, ",\"mean_1h\":");
        if (s->co.has_mean_1h) {
            put_tenths_value(&o, (long)s->co.mean_1h_x10);
        } else {
            put(&o, "null");
        }
        put(&o, "}");
    } else {
        put(&o, "null");
    }
    put(&o, "}");
    return out_finish(&o);
}

/* ---------------------------------------------------------------- history */
void web_hist_init(web_hist_t *h)
{
    if (h != NULL) {
        memset(h, 0, sizeof(*h));
    }
}

static bool tenths_i16(bool has, float v, int16_t *out)
{
    long t = 0;
    if (!has || !csv_round_tenths(v, &t) || t < INT16_MIN || t > INT16_MAX) {
        return false;
    }
    *out = (int16_t)t;
    return true;
}

void web_hist_push(web_hist_t *h, const web_snapshot_t *s)
{
    if (h == NULL || s == NULL) {
        return;
    }
    web_hist_entry_t *e = &h->e[h->head];
    memset(e, 0, sizeof(*e));
    e->uptime_s = s->uptime_s < 0 ? 0U :
                  s->uptime_s > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)s->uptime_s;
    if (s->row.has_pm) {
        e->pm2_5 = s->row.pm2_5;
        e->flags |= WEB_HIST_PM;
    }
    if (tenths_i16(s->row.has_th, s->row.temp_c, &e->temp_x10)) {
        e->flags |= WEB_HIST_TEMP;
    }
    if (tenths_i16(s->row.has_th, s->row.humidity_pct, &e->rh_x10)) {
        e->flags |= WEB_HIST_RH;
    }
    e->d2 = s->ml.present ? s->ml.d2 : NAN;
    e->shown = s->shown;
    h->head = (h->head + 1U) % WEB_HIST_LEN;
    if (h->count < WEB_HIST_LEN) {
        ++h->count;
    }
}

unsigned web_hist_count(const web_hist_t *h)
{
    return h != NULL ? h->count : 0U;
}

const web_hist_entry_t *web_hist_at(const web_hist_t *h, unsigned i)
{
    if (h == NULL || i >= h->count) {
        return NULL;
    }
    /* oldest = head - count (mod LEN) */
    const unsigned idx = (h->head + WEB_HIST_LEN - h->count + i) % WEB_HIST_LEN;
    return &h->e[idx];
}

size_t web_json_hist_entry(const web_hist_entry_t *e, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    if (e == NULL) {
        o.ok = false;
        return out_finish(&o);
    }
    put(&o, "[%lu,", (unsigned long)e->uptime_s);
    if (e->flags & WEB_HIST_PM) {
        put(&o, "%u,", (unsigned)e->pm2_5);
    } else {
        put(&o, "null,");
    }
    if (e->flags & WEB_HIST_TEMP) {
        put_tenths_value(&o, e->temp_x10);
    } else {
        put(&o, "null");
    }
    put(&o, ",");
    if (e->flags & WEB_HIST_RH) {
        put_tenths_value(&o, e->rh_x10);
    } else {
        put(&o, "null");
    }
    put(&o, ",");
    put_d2(&o, e->d2);
    put(&o, ",");
    put_str(&o, e->shown);
    put(&o, "]");
    return out_finish(&o);
}
