#include "aht21_parse.h"

/* AHT21 datasheet V1.0 (May 2021). Section and page numbers below refer to
 * that document only. */
enum {
    /* §7.4 step 4: CRC[7:0] = 1+x^4+x^5+x^8. Dropping the implicit x^8 term
     * leaves bits 5, 4 and 0 = 0x31. */
    CRC8_POLY = 0x31,
    /* §7.4 step 4: "The initial value of CRC is 0xFF". */
    CRC8_INIT = 0xff,
    /* Table 9 / §7.4 step 3: state Bit[7] = 1 means busy measuring. */
    STATE_BUSY = 0x80,
    /* §7.4 step 1: "If the status word and 0x18 are not equal to 0x18". */
    STATUS_INIT_MASK = 0x18,
    /* p. 13 figure: the six bytes before the CRC byte. */
    CRC_COVERED = AHT21_FRAME_LEN - 1
};

/* §8.1-8.2 divide the 20-bit signals by 2^20. */
#define AHT21_SIGNAL_FULL_SCALE 1048576.0f

uint8_t aht21_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = CRC8_INIT;
    if (data == NULL) {
        return crc;
    }
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ CRC8_POLY)
                                : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

bool aht21_needs_init(uint8_t status)
{
    return (status & STATUS_INIT_MASK) != STATUS_INIT_MASK;
}

esp_err_t aht21_parse(const uint8_t raw[AHT21_FRAME_LEN],
                      float *temp_c, float *rh_pct)
{
    if (raw == NULL || temp_c == NULL || rh_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (aht21_crc8(raw, CRC_COVERED) != raw[CRC_COVERED]) {
        return ESP_ERR_INVALID_CRC;
    }
    if (raw[0] & STATE_BUSY) {
        return ESP_ERR_INVALID_STATE;
    }

    /* p. 13 figure "Read temperature and humidity data", MSB first:
     *   raw[1]      humidity data          S_RH[19:12]
     *   raw[2]      humidity data          S_RH[11:4]
     *   raw[3] hi   humidity data          S_RH[3:0]
     *   raw[3] lo   temperature data       S_T[19:16]
     *   raw[4]      temperature data       S_T[15:8]
     *   raw[5]      temperature data       S_T[7:0] */
    const uint32_t s_rh = ((uint32_t)raw[1] << 12) | ((uint32_t)raw[2] << 4) |
                          ((uint32_t)raw[3] >> 4);
    const uint32_t s_t = (((uint32_t)raw[3] & 0x0fu) << 16) |
                         ((uint32_t)raw[4] << 8) | (uint32_t)raw[5];

    /* A 20-bit integer and a division by 2^20 are both exact in float, so
     * only the final scale/offset rounds.
     * §8.1: RH[%]  = (S_RH / 2^20) * 100
     * §8.2: T[degC] = (S_T / 2^20) * 200 - 50 */
    *rh_pct = ((float)s_rh / AHT21_SIGNAL_FULL_SCALE) * 100.0f;
    *temp_c = ((float)s_t / AHT21_SIGNAL_FULL_SCALE) * 200.0f - 50.0f;
    return ESP_OK;
}
