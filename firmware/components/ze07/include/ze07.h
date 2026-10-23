#ifndef AIRGUARD_ZE07_H
#define AIRGUARD_ZE07_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Winsen "Electrochemical CO Module (Model: ZE07-CO) User's Manual",
 * Version 1.7, valid from 2020-10-19 ("manual" below).
 * Source: https://www.winsen-sensor.com/d/files/manual/ze07-co.pdf
 *
 * FW-5b uses the initiative upload mode only (manual p.4: the power-on
 * default). The firmware never sends commands, so the module RXD pin
 * (rated 0-3.0 V, p.4) is not driven by the 3.3 V ESP32 TX (owner decision
 * 18 Oct 2026: J2.4 stays unconnected).
 *
 * Manual p.4 Table 4, one 9-byte frame:
 *   [0] 0xFF start   [1] gas type CO = 0x04   [2] unit ppm = 0x03
 *   [3] no. of decimal = 0x01   [4] conc. high   [5] conc. low
 *   [6] full range high   [7] full range low   [8] checksum
 * Concentration = (high * 256 + low) x 0.1 ppm, so the 16-bit value is the
 * concentration in units of 0.1 ppm, exactly the one decimal of co_ppm in
 * CLAUDE.md §7. Table 4 example: 0x0025 -> 3.7 ppm, full range 0x1388 ->
 * 500.0 ppm (p.3: detection range 0~500 ppm, resolution 0.1 ppm).
 */
#define ZE07_FRAME_SIZE 9U

/* Values the parser requires besides a correct checksum (Table 4). A frame
 * that differs means something the manual does not describe, so it is
 * rejected instead of guessed at; this also makes a false start byte 0xFF
 * inside data much less likely to pass after a resync. */
#define ZE07_START_BYTE      0xFFU
#define ZE07_GAS_CO          0x04U
#define ZE07_UNIT_PPM        0x03U
#define ZE07_DECIMALS        0x01U
#define ZE07_FULL_RANGE_X10  5000U

typedef struct {
    /* concentration in 0.1 ppm (no float in the parser).
     * Not clipped: a value >= ZE07_FULL_RANGE_X10 is accepted and means the
     * reading is at or above full scale; the safety logic treats it as above
     * every threshold (FW-5b step 1 §5.1), not as a broken frame. */
    uint16_t conc_x10;
} ze07_reading_t;

/* Manual p.6 §3: checksum = Not(Byte1 + ... + Byte7) + 1, 8-bit, i.e. the
 * two's complement of the sum of bytes 1..7 (start byte and checksum byte
 * excluded). frame must hold ZE07_FRAME_SIZE bytes. */
uint8_t ze07_checksum(const uint8_t *frame);

/* Find and validate the first frame in buf. Same contract style as
 * pms7003_parse(): buf, out and consumed must be non-NULL; out changes only
 * on ESP_OK; consumed is set to 0 first whenever it is non-NULL.
 *   ESP_OK                    consume garbage prefix + frame
 *   ESP_ERR_INVALID_SIZE      start byte found but fewer than 9 bytes from
 *                             it: consume the prefix only, keep the partial
 *                             frame for the next call
 *   ESP_ERR_INVALID_CRC       checksum wrong: consume through the start byte
 *   ESP_ERR_INVALID_RESPONSE  checksum right but gas/unit/decimal/full range
 *                             not as in Table 4: consume through the start
 *                             byte
 *   ESP_ERR_NOT_FOUND         no start byte: consume everything
 *   ESP_ERR_INVALID_ARG       NULL argument
 * Consuming only through the start byte after a bad frame means a real
 * frame that begins inside the rejected 9 bytes is still found next call. */
esp_err_t ze07_parse(const uint8_t *buf, size_t len,
                     ze07_reading_t *out, size_t *consumed);

#endif
