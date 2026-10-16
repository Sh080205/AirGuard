#include "ens160.h"

#include <stdbool.h>
#include <stddef.h>

/* DS Table 12: fast-mode plus, SCL up to 1000 kHz. */
#define ENS160_SCL_MAX_HZ 1000000U

/* Per-transaction timeout, same reasoning as AHT21_XFER_TIMEOUT_MS: the
 * longest transfer here (register byte + 6 data bytes + 2 address bytes) is
 * about 9 bytes x 9 bits / 100 kHz = 0.8 ms; 20 ms bounds how long the 1 Hz
 * sampler can block on a stuck bus. ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define ENS160_XFER_TIMEOUT_MS 20

static i2c_master_dev_handle_t s_dev;

/* Software mirror of DATA_MISR (DS §16.2.14). Every read from 0x20..0x37
 * goes through read_checked(), which re-reads the hardware value after each
 * burst, so the mirror is resynchronised every time (also after a mismatch).
 * Owned by the single calling task. */
static uint8_t s_misr;
static bool s_misr_known;

esp_err_t ens160_attach(i2c_master_bus_handle_t bus, uint32_t scl_hz)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    if (bus == NULL || scl_hz == 0 || scl_hz > ENS160_SCL_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ENS160_I2C_ADDR,
        .scl_speed_hz = scl_hz,
        .scl_wait_us = 0,               /* driver default */
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

/* DS §14.1.3 / Figure 8: write the register address, then (repeated) START
 * and read; the figure allows Sr or P+S between the two. */
static esp_err_t read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, ENS160_XFER_TIMEOUT_MS);
}

/* DS §14.1.4 / Figure 11: address, register, data bytes, one transaction. */
static esp_err_t write_regs(uint8_t reg, const uint8_t *data, size_t len)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[1 + 4];
    if (len > sizeof(buf) - 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    buf[0] = reg;
    for (size_t i = 0; i < len; ++i) {
        buf[1 + i] = data[i];
    }
    return i2c_master_transmit(s_dev, buf, 1 + len, ENS160_XFER_TIMEOUT_MS);
}

esp_err_t ens160_read_part_id(uint16_t *part_id)
{
    if (part_id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[2];
    const esp_err_t err = read_regs(ENS160_REG_PART_ID, b, sizeof(b));
    if (err == ESP_OK) {
        *part_id = ens160_u16le(b);
    }
    return err;
}

esp_err_t ens160_set_opmode(uint8_t mode)
{
    esp_err_t err = write_regs(ENS160_REG_OPMODE, &mode, 1);
    if (err != ESP_OK || mode == ENS160_OPMODE_DEEP_SLEEP) {
        /* DS §16.2.2: in DEEP SLEEP the sensor "will respond to an OPMODE
         * write" only, so a readback is not guaranteed to answer */
        return err;
    }
    uint8_t back = 0;
    err = read_regs(ENS160_REG_OPMODE, &back, 1);
    if (err != ESP_OK) {
        return err;
    }
    return back == mode ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

esp_err_t ens160_read_opmode(uint8_t *mode)
{
    if (mode == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* OPMODE (0x10) is outside 0x20..0x37, so this read does not touch
     * DATA_MISR (DS §16.2.14) */
    return read_regs(ENS160_REG_OPMODE, mode, 1);
}

esp_err_t ens160_sync_misr(void)
{
    s_misr_known = false;
    uint8_t hw = 0;
    const esp_err_t err = read_regs(ENS160_REG_DATA_MISR, &hw, 1);
    if (err == ESP_OK) {
        s_misr = hw;
        s_misr_known = true;
    }
    return err;
}

static esp_err_t read_checked(uint8_t reg, uint8_t *buf, size_t len,
                              uint8_t *misr_expected, uint8_t *misr_read)
{
    if (!s_misr_known) {
        const esp_err_t err = ens160_sync_misr();
        if (err != ESP_OK) {
            return err;
        }
    }
    esp_err_t err = read_regs(reg, buf, len);
    if (err != ESP_OK) {
        /* bytes may or may not have been clocked out: mirror unknown */
        s_misr_known = false;
        return err;
    }
    const uint8_t expected = ens160_misr_bytes(s_misr, buf, len);
    uint8_t hw = 0;
    err = read_regs(ENS160_REG_DATA_MISR, &hw, 1);
    if (err != ESP_OK) {
        s_misr_known = false;
        return err;
    }
    s_misr = hw;            /* resync whatever the result */
    s_misr_known = true;
    if (misr_expected != NULL) {
        *misr_expected = expected;
    }
    if (misr_read != NULL) {
        *misr_read = hw;
    }
#if ENS160_MISR_CHECK
    if (hw != expected) {
        return ESP_ERR_INVALID_CRC;
    }
#endif
    return ESP_OK;
}

esp_err_t ens160_read_data(uint8_t raw[ENS160_DATA_LEN],
                           uint8_t *misr_expected, uint8_t *misr_read)
{
    if (raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return read_checked(ENS160_REG_DEVICE_STATUS, raw, ENS160_DATA_LEN,
                        misr_expected, misr_read);
}

esp_err_t ens160_write_compensation(uint16_t temp_raw, uint16_t rh_raw)
{
    /* TEMP_IN 0x13/0x14 then RH_IN 0x15/0x16: contiguous, so one
     * auto-increment write keeps LSB-before-MSB for both registers */
    const uint8_t data[4] = {
        (uint8_t)temp_raw, (uint8_t)(temp_raw >> 8),
        (uint8_t)rh_raw, (uint8_t)(rh_raw >> 8),
    };
    return write_regs(ENS160_REG_TEMP_IN, data, sizeof(data));
}

esp_err_t ens160_read_used_th(uint16_t *temp_raw, uint16_t *rh_raw)
{
    if (temp_raw == NULL || rh_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t b[4];
    const esp_err_t err = read_checked(ENS160_REG_DATA_T, b, sizeof(b), NULL, NULL);
    if (err == ESP_OK) {
        *temp_raw = ens160_u16le(&b[0]);
        *rh_raw = ens160_u16le(&b[2]);
    }
    return err;
}
