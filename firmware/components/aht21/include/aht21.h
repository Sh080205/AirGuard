#pragma once

/* FW-4: AHT21 bus access over the ESP-IDF v5.x i2c_master driver
 * [ขึ้นกับเวอร์ชัน ESP-IDF]. Frame decoding stays in aht21_parse.h (pure C).
 * Source: ASAIR AHT21 datasheet V1.0 (May 2021) §7.4, CLAUDE.md §5.
 *
 * Non-blocking split of the measurement: aht21_trigger() starts it and
 * aht21_read_frame() fetches the result later (caller waits >= 80 ms,
 * §7.4 step 3). Nothing here sleeps, so a 1 Hz loop can trigger in one cycle
 * and read in the next one.
 *
 * One task owns all calls (no internal locking); the i2c_master driver itself
 * serializes access to a bus shared with other devices (ENS160 in FW-5).
 */

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "aht21_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CLAUDE.md §5: AHT21 I2C address 0x38 (7-bit). */
#define AHT21_I2C_ADDR 0x38

/* Add the sensor to an existing bus. scl_hz must be 10 kHz..400 kHz
 * (datasheet §4.4, CLAUDE.md §5); anything else -> ESP_ERR_INVALID_ARG.
 * Idempotent after success. */
esp_err_t aht21_attach(i2c_master_bus_handle_t bus, uint32_t scl_hz);

/* §7.4 step 1: one read transaction, 1 byte. On the wire the address byte is
 * (0x38 << 1) | 1 = 0x71, which is the "0x71" the datasheet says to send.
 * Caller must have waited >= 100 ms after power-on. */
esp_err_t aht21_read_status(uint8_t *status);

/* §7.4 step 2: write 0xAC 0x33 0x00 (trigger measurement). */
esp_err_t aht21_trigger(void);

/* §7.4 step 4: read 7 bytes (state, 5 data, CRC). The driver ACKs bytes 1-6
 * and NACKs the last one [ขึ้นกับเวอร์ชัน ESP-IDF: i2c_master_receive in
 * v5.5.5 builds ACK x (n-1) + NACK x 1]. Decode with aht21_parse(). */
esp_err_t aht21_read_frame(uint8_t raw[AHT21_FRAME_LEN]);

#ifdef __cplusplus
}
#endif
