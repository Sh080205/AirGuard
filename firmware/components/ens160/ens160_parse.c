#include "ens160_parse.h"

#include "logutil.h"

uint16_t ens160_u16le(const uint8_t b[2])
{
    /* DS §16.1: registers are little endian, LSB at the lower address */
    return (uint16_t)((unsigned)b[0] | ((unsigned)b[1] << 8));
}

esp_err_t ens160_parse_data(const uint8_t raw[ENS160_DATA_LEN], ens160_data_t *out)
{
    if (raw == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t st = raw[0];
    out->status = st;
    out->running = (st & ENS160_STATUS_STATAS) != 0;
    out->error = (st & ENS160_STATUS_STATER) != 0;
    out->validity = (uint8_t)((st & ENS160_STATUS_VALIDITY_MASK) >> ENS160_STATUS_VALIDITY_SHIFT);
    out->new_data = (st & ENS160_STATUS_NEWDAT) != 0;
    out->aqi = (uint8_t)(raw[1] & 0x07u);   /* DS Table 27: bits 0:2 */
    out->tvoc_ppb = ens160_u16le(&raw[2]);
    out->eco2_ppm = ens160_u16le(&raw[4]);
    return ESP_OK;
}

ens160_verdict_t ens160_verdict(const ens160_data_t *d)
{
    if (d == NULL || d->error) {
        return ENS160_VERDICT_ERROR;
    }
    if (!d->running) {
        return ENS160_VERDICT_NOT_RUNNING;
    }
    switch (d->validity) {
    case ENS160_VALIDITY_NORMAL: break;
    case ENS160_VALIDITY_WARMUP: return ENS160_VERDICT_WARMUP;
    case ENS160_VALIDITY_STARTUP: return ENS160_VERDICT_STARTUP;
    default: return ENS160_VERDICT_INVALID;
    }
    if (d->tvoc_ppb > ENS160_TVOC_MAX_PPB ||
        d->eco2_ppm < ENS160_ECO2_MIN_PPM || d->eco2_ppm > ENS160_ECO2_MAX_PPM) {
        return ENS160_VERDICT_RANGE;
    }
    return ENS160_VERDICT_OK;
}

esp_err_t ens160_encode_temp(float temp_c, uint16_t *out)
{
    /* written as !(in range) so NaN is rejected too */
    if (out == NULL || !(temp_c >= ENS160_COMP_T_MIN_C && temp_c <= ENS160_COMP_T_MAX_C)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* -40..85 °C -> 14921.6..22921.6: positive and < 65536, so +0.5 and
     * truncation is round-to-nearest without libm */
    const double v = ((double)temp_c + 273.15) * 64.0;
    *out = (uint16_t)(v + 0.5);
    return ESP_OK;
}

esp_err_t ens160_encode_rh(float rh_pct, uint16_t *out)
{
    if (out == NULL || !(rh_pct >= 0.0f && rh_pct <= 100.0f)) {
        return ESP_ERR_INVALID_ARG;
    }
    const double v = (double)rh_pct * 512.0;   /* 0..51200 */
    *out = (uint16_t)(v + 0.5);
    return ESP_OK;
}

float ens160_decode_temp(uint16_t raw)
{
    return (float)((double)raw / 64.0 - 273.15);
}

float ens160_decode_rh(uint16_t raw)
{
    return (float)((double)raw / 512.0);
}

uint8_t ens160_misr_update(uint8_t misr, uint8_t data)
{
    const uint8_t x = (uint8_t)((misr << 1) ^ data);
    return (misr & 0x80u) == 0 ? x : (uint8_t)(x ^ ENS160_MISR_POLY);
}

uint8_t ens160_misr_bytes(uint8_t misr, const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return misr;
    }
    for (size_t i = 0; i < len; ++i) {
        misr = ens160_misr_update(misr, data[i]);
    }
    return misr;
}

ens160_fill_t ens160_hold_step(ens160_hold_t *h, bool read_ok, const ens160_data_t *d,
                               int64_t now_us, int64_t max_age_us,
                               uint16_t *tvoc_ppb, uint16_t *eco2_ppm)
{
    if (h == NULL || tvoc_ppb == NULL || eco2_ppm == NULL) {
        return ENS160_FILL_EMPTY;
    }
    if (read_ok) {
        if (d == NULL || ens160_verdict(d) != ENS160_VERDICT_OK) {
            h->have = false;
            return ENS160_FILL_EMPTY;
        }
        if (d->new_data) {
            h->have = true;
            h->tvoc_ppb = d->tvoc_ppb;
            h->eco2_ppm = d->eco2_ppm;
            h->data_us = now_us;
            *tvoc_ppb = h->tvoc_ppb;
            *eco2_ppm = h->eco2_ppm;
            return ENS160_FILL_NEW;
        }
    }
    if (!logutil_frame_fresh(h->have, h->data_us, now_us, max_age_us)) {
        return ENS160_FILL_EMPTY;
    }
    *tvoc_ppb = h->tvoc_ppb;
    *eco2_ppm = h->eco2_ppm;
    return ENS160_FILL_REUSED;
}
