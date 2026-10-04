#include "aht21.h"

#include <stddef.h>

/* Datasheet §4.4 / CLAUDE.md §5: I2C clock while measuring 10-400 kHz. */
#define AHT21_SCL_MIN_HZ  10000U
#define AHT21_SCL_MAX_HZ 400000U

/* Per-transaction timeout. A 7-byte read at 100 kHz is about
 * 8 bytes x 9 bits / 100 kHz = 0.72 ms; 20 ms leaves room for clock
 * stretching and bus arbitration with other devices while still bounding how
 * long the 1 Hz sampler can block on a stuck bus.
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define AHT21_XFER_TIMEOUT_MS 20

static i2c_master_dev_handle_t s_dev;

esp_err_t aht21_attach(i2c_master_bus_handle_t bus, uint32_t scl_hz)
{
    if (s_dev != NULL) {
        return ESP_OK;
    }
    if (bus == NULL || scl_hz < AHT21_SCL_MIN_HZ || scl_hz > AHT21_SCL_MAX_HZ) {
        return ESP_ERR_INVALID_ARG;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AHT21_I2C_ADDR,
        .scl_speed_hz = scl_hz,
        .scl_wait_us = 0,               /* driver default */
    };
    return i2c_master_bus_add_device(bus, &cfg, &s_dev);
}

esp_err_t aht21_read_status(uint8_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(s_dev, status, 1, AHT21_XFER_TIMEOUT_MS);
}

esp_err_t aht21_trigger(void)
{
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    /* §7.4 step 2: command 0xAC with parameters 0x33, 0x00. */
    static const uint8_t cmd[3] = { 0xAC, 0x33, 0x00 };
    return i2c_master_transmit(s_dev, cmd, sizeof(cmd), AHT21_XFER_TIMEOUT_MS);
}

esp_err_t aht21_read_frame(uint8_t raw[AHT21_FRAME_LEN])
{
    if (raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_dev == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_receive(s_dev, raw, AHT21_FRAME_LEN, AHT21_XFER_TIMEOUT_MS);
}
