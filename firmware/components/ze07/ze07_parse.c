#include "ze07.h"

/* Byte offsets in one frame, manual p.4 Table 4 (see ze07.h). */
enum {
    GAS_OFFSET = 1,
    UNIT_OFFSET = 2,
    DECIMALS_OFFSET = 3,
    CONC_OFFSET = 4,
    FULL_RANGE_OFFSET = 6,
    CHECKSUM_OFFSET = 8
};

static uint16_t read_be16(const uint8_t *bytes)
{
    /* Table 4: high byte precedes low byte. */
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

uint8_t ze07_checksum(const uint8_t *frame)
{
    /* Manual p.6 §3. uint8_t arithmetic wraps mod 256 like the manual's
     * unsigned char example code. */
    uint8_t sum = 0;
    for (size_t i = 1; i < CHECKSUM_OFFSET; ++i) {
        sum = (uint8_t)(sum + frame[i]);
    }
    return (uint8_t)(~sum + 1U);
}

esp_err_t ze07_parse(const uint8_t *buf, size_t len,
                     ze07_reading_t *out, size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (buf == NULL || out == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t start = 0; start < len; ++start) {
        if (buf[start] != ZE07_START_BYTE) {
            continue;
        }
        *consumed = start;
        if (len - start < ZE07_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *frame = buf + start;
        *consumed = start + 1;
        if (ze07_checksum(frame) != frame[CHECKSUM_OFFSET]) {
            return ESP_ERR_INVALID_CRC;
        }
        if (frame[GAS_OFFSET] != ZE07_GAS_CO ||
            frame[UNIT_OFFSET] != ZE07_UNIT_PPM ||
            frame[DECIMALS_OFFSET] != ZE07_DECIMALS ||
            read_be16(frame + FULL_RANGE_OFFSET) != ZE07_FULL_RANGE_X10) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        out->conc_x10 = read_be16(frame + CONC_OFFSET);
        *consumed = start + ZE07_FRAME_SIZE;
        return ESP_OK;
    }
    *consumed = len;
    return ESP_ERR_NOT_FOUND;
}
