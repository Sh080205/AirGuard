#include "csv_row.h"

#include <stdarg.h>
#include <stdio.h>

/* Values whose tenths reach this magnitude are written as empty fields.
 * It only keeps the integer conversion in range on every target (32-bit
 * long); it is not a sensor range. */
#define TENTHS_LIMIT 1e9

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
    /* vsnprintf never writes more than room bytes; n >= room means the
     * output was truncated. */
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

static bool tm_valid(const struct tm *t)
{
    /* Ranges from struct tm (C11 7.27.1) plus a four-digit year. */
    return t != NULL &&
           t->tm_year >= -1900 && t->tm_year <= 9999 - 1900 &&
           t->tm_mon >= 0 && t->tm_mon <= 11 &&
           t->tm_mday >= 1 && t->tm_mday <= 31 &&
           t->tm_hour >= 0 && t->tm_hour <= 23 &&
           t->tm_min >= 0 && t->tm_min <= 59 &&
           t->tm_sec >= 0 && t->tm_sec <= 60;
}

/* CLAUDE.md §7: ISO 8601 with a fixed +07:00 offset. */
static void put_iso(out_t *o, const struct tm *t)
{
    put(o, "%04d-%02d-%02dT%02d:%02d:%02d+07:00",
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);
}

/* One decimal place using integer printing only, so host and newlib give the
 * same text. v*10 is exact in double (24-bit mantissa times 10), so the
 * half-away-from-zero step sees the float that was actually stored. */
static void put_tenths(out_t *o, float v)
{
    const double x = (double)v * 10.0;
    if (!(x > -TENTHS_LIMIT && x < TENTHS_LIMIT)) {
        return; /* NaN, infinity or out of range: leave the field empty. */
    }
    const long t = (x >= 0.0) ? (long)(x + 0.5) : -(long)(-x + 0.5);
    const long mag = (t < 0) ? -t : t;
    put(o, "%s%ld.%ld", (t < 0) ? "-" : "", mag / 10, mag % 10);
}

size_t csv_header(char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    put(&o, "timestamp_iso,pm1_0,pm2_5,pm10,temp_c,humidity_pct,"
            "tvoc_ppb,eco2_ppm,co_ppm,label\n");
    return out_finish(&o);
}

size_t csv_format_row(const csv_sample_t *s, char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    if (s == NULL || !tm_valid(&s->local_time)) {
        o.ok = false;
        return out_finish(&o);
    }

    put_iso(&o, &s->local_time);
    put(&o, ",");
    if (s->has_pm) {
        put(&o, "%u,%u,%u", (unsigned)s->pm1_0, (unsigned)s->pm2_5,
            (unsigned)s->pm10);
    } else {
        put(&o, ",,");
    }
    put(&o, ",");
    if (s->has_th) {
        put_tenths(&o, s->temp_c);
        put(&o, ",");
        put_tenths(&o, s->humidity_pct);
    } else {
        put(&o, ",");
    }
    put(&o, ",");
    if (s->has_voc) {
        put(&o, "%u,%u", (unsigned)s->tvoc_ppb, (unsigned)s->eco2_ppm);
    } else {
        put(&o, ",");
    }
    put(&o, ",");
    if (s->has_co) {
        put_tenths(&o, s->co_ppm);
    }
    put(&o, ",%s\n", s->marked ? "marked" : "normal");
    return out_finish(&o);
}

size_t csv_filename(const struct tm *t, const char *prefix,
                    char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    if (!tm_valid(t) || prefix == NULL || prefix[0] == '\0') {
        o.ok = false;
        return out_finish(&o);
    }
    put(&o, "%s_%04d%02d%02d_%02d%02d%02d.csv", prefix,
        t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
        t->tm_hour, t->tm_min, t->tm_sec);
    return out_finish(&o);
}

size_t csv_event_row(const struct tm *start, const struct tm *end,
                     char *buf, size_t cap)
{
    out_t o;
    out_init(&o, buf, cap);
    if (!tm_valid(start) || !tm_valid(end)) {
        o.ok = false;
        return out_finish(&o);
    }
    put_iso(&o, start);
    put(&o, ",");
    put_iso(&o, end);
    put(&o, ",marked,\n");
    return out_finish(&o);
}
