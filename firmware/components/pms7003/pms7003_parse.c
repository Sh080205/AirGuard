#define PMS7003_PARSE_ONLY
#include "pms7003.h"

/* Manual V2.5, Appendix I (pp. 13-14): header 42 4D, length = 2*13+2,
 * then high/low bytes of each word. Offsets below count from the first header
 * byte: length at 2, Data1-6 at 4,6,8,10,12,14, checksum at 30.
 */
enum {
    HEADER_FIRST = 0x42,
    HEADER_SECOND = 0x4d,
    LENGTH_OFFSET = 2,
    DATA_OFFSET = 4,
    EXPECTED_LENGTH = 2 * 13 + 2,
    CHECKSUM_OFFSET = 30
};

static uint16_t read_be16(const uint8_t *bytes)
{
    /* Appendix I: high 8 bits precede low 8 bits. */
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

esp_err_t pms7003_parse(const uint8_t *buf, size_t len,
                        pms7003_data_t *out, size_t *consumed)
{
    if (consumed != NULL) {
        *consumed = 0;
    }
    if (buf == NULL || out == NULL || consumed == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t start = 0; start < len; ++start) {
        if (buf[start] != HEADER_FIRST) {
            continue;
        }
        const size_t available = len - start;
        if (available == 1) {
            *consumed = start; /* Preserve a split header. */
            return ESP_ERR_NOT_FOUND;
        }
        if (buf[start + 1] != HEADER_SECOND) {
            continue;
        }
        *consumed = start;
        if (available < DATA_OFFSET) {
            return ESP_ERR_INVALID_SIZE;
        }
        const uint8_t *frame = buf + start;
        if (read_be16(frame + LENGTH_OFFSET) != EXPECTED_LENGTH) {
            *consumed = start + 1;
            return ESP_ERR_INVALID_RESPONSE;
        }
        if (available < PMS7003_FRAME_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }

        /* Appendix I, checksum row (p. 14): sum every byte from the first
         * start character through Data13 low byte, excluding checksum bytes.
         * The accumulator fits uint16_t: at most 30 * UINT8_MAX.
         */
        uint16_t checksum = 0;
        for (size_t i = 0; i < CHECKSUM_OFFSET; ++i) {
            checksum = (uint16_t)(checksum + frame[i]);
        }
        if (checksum != read_be16(frame + CHECKSUM_OFFSET)) {
            *consumed = start + 1;
            return ESP_ERR_INVALID_CRC;
        }

        /* Appendix I Data1-6, in table order. Data7-13 are checksum-covered
         * but are not PM mass concentrations, so are not exposed by FW-2.
         */
        const pms7003_data_t parsed = {
            .pm1_0_cf1 = read_be16(frame + 4),
            .pm2_5_cf1 = read_be16(frame + 6),
            .pm10_cf1 = read_be16(frame + 8),
            .pm1_0_atmospheric = read_be16(frame + 10),
            .pm2_5_atmospheric = read_be16(frame + 12),
            .pm10_atmospheric = read_be16(frame + 14),
        };
        *out = parsed;
        *consumed = start + PMS7003_FRAME_SIZE;
        return ESP_OK;
    }
    *consumed = len;
    return ESP_ERR_NOT_FOUND;
}
