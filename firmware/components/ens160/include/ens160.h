#pragma once

/* FW-5a: ENS160 bus access over the ESP-IDF v5.x i2c_master driver
 * [ขึ้นกับเวอร์ชัน ESP-IDF]. Decoding/conversion lives in ens160_parse.h
 * (pure C, host-tested).
 * Source: ScioSense ENS160 Datasheet Version 1.3, SC-001224-DS-9,
 * 2023-03-29 ("DS").
 *
 * Every call is one or two short I2C transactions and never sleeps: the
 * datasheet gives no wait time after an OPMODE write, so the mode is
 * verified by reading OPMODE back instead of waiting a guessed time.
 *
 * One task owns all calls (no internal locking); the i2c_master driver
 * serializes access to the bus shared with the AHT21 (aht21.h).
 */

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "ens160_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CLAUDE.md §5 / DS §14.1.1: 0x53 when MISO/ADDR is high at power-up. */
#define ENS160_I2C_ADDR 0x53

/* Compile-time switch for the DATA_MISR check (DS §16.2.14, "optional").
 * 1 = a burst whose checksum does not match returns ESP_ERR_INVALID_CRC.
 * Set -DENS160_MISR_CHECK=0 only if the board shows misr_err on every read
 * (= our reading of the DS algorithm is wrong, not a bus fault). */
#ifndef ENS160_MISR_CHECK
#define ENS160_MISR_CHECK 1
#endif

/* Add the sensor to an existing bus. DS §14.1.1 / Table 12: standard,
 * fast and fast-plus mode, SCL <= 1000 kHz. scl_hz 0 or > 1 MHz ->
 * ESP_ERR_INVALID_ARG. Idempotent after success. */
esp_err_t ens160_attach(i2c_master_bus_handle_t bus, uint32_t scl_hz);

/* PART_ID (0x00, 2 bytes LE). Compare with ENS160_PART_ID. */
esp_err_t ens160_read_part_id(uint16_t *part_id);

/* Write OPMODE (0x10) and read it back. Readback differs ->
 * ESP_ERR_INVALID_RESPONSE. DS Table 18: the new mode starts at once.
 * DEEP SLEEP is written without readback (DS §16.2.2: limited functionality,
 * only an OPMODE write is answered). */
esp_err_t ens160_set_opmode(uint8_t mode);

/* Read DATA_MISR (0x38) into the driver's mirror. Call once after attach;
 * ens160_read_data() resynchronises by itself after any failure. */
esp_err_t ens160_sync_misr(void);

/* Burst read 0x20..0x25 (ENS160_DATA_LEN bytes) then DATA_MISR.
 * ESP_ERR_INVALID_CRC: checksum mismatch, raw[] must not be used;
 * *misr_expected / *misr_read (optional, may be NULL) say why.
 * Other errors come from the I2C driver. */
esp_err_t ens160_read_data(uint8_t raw[ENS160_DATA_LEN],
                           uint8_t *misr_expected, uint8_t *misr_read);

/* TEMP_IN + RH_IN in one auto-increment write starting at 0x13:
 * T LSB, T MSB, RH LSB, RH MSB. DS §16.2.5-6: LSB first, the update is
 * taken on the MSB write. Values from ens160_encode_temp/_rh. */
esp_err_t ens160_write_compensation(uint16_t temp_raw, uint16_t rh_raw);

/* DATA_T + DATA_RH (0x30..0x33), MISR-checked like ens160_read_data():
 * the T/RH the sensor actually uses (DS §16.2.12-13). For the log. */
esp_err_t ens160_read_used_th(uint16_t *temp_raw, uint16_t *rh_raw);

#ifdef __cplusplus
}
#endif
