#pragma once

/* Pure C helpers for the AHT21 measurement frame.
 * Source: ASAIR AHT21 datasheet V1.0 (May 2021). No I2C here; the bus
 * sequence (0x71 status, 0xAC 0x33 0x00 trigger, waits) belongs to FW-4.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bytes read after the trigger: state + 5 data bytes + CRC.
 * Datasheet p. 13, figure "Read temperature and humidity data";
 * §7.4 step 4: "After receiving six bytes, the next byte is CRC". */
#define AHT21_FRAME_LEN 7

/* CRC8 per §7.4 step 4: polynomial 1+x^4+x^5+x^8, initial value 0xFF.
 * MSB first, no final XOR (the datasheet names none). */
uint8_t aht21_crc8(const uint8_t *data, size_t len);

/* raw[0] state, raw[1..5] data, raw[6] CRC over raw[0..5] (p. 13, §7.4).
 * ESP_ERR_INVALID_ARG  NULL pointer
 * ESP_ERR_INVALID_CRC  CRC mismatch (checked first: a corrupt state byte
 *                      cannot be trusted to report busy)
 * ESP_ERR_INVALID_STATE state bit7 = 1, sensor still busy (§7.4 step 3)
 * Outputs are written only on ESP_OK. */
esp_err_t aht21_parse(const uint8_t raw[AHT21_FRAME_LEN],
                      float *temp_c, float *rh_pct);

/* §7.4 step 1: if (status & 0x18) != 0x18 the 0x1B/0x1C/0x1E registers
 * must be initialised. That init routine is not in the datasheet and is not
 * part of FW-3. */
bool aht21_needs_init(uint8_t status);

#ifdef __cplusplus
}
#endif
