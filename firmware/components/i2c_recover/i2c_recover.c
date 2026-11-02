#include "i2c_recover.h"

#include <stddef.h>

void i2c_recover_sample(const i2c_recover_io_t *io, uint8_t n, i2c_lines_t *out)
{
    if (io == NULL || out == NULL) {
        return;
    }
    if (n == 0U) {
        n = 1U;
    }
    out->samples = n;
    out->sda_low = 0U;
    out->scl_low = 0U;
    for (uint8_t i = 0U; i < n; ++i) {
        if (i != 0U) {
            io->delay_half(io->ctx);
        }
        if (io->read_sda(io->ctx) == 0) {
            ++out->sda_low;
        }
        if (io->read_scl(io->ctx) == 0) {
            ++out->scl_low;
        }
    }
}

i2c_lines_state_t i2c_lines_state(const i2c_lines_t *lines)
{
    if (lines == NULL || lines->samples == 0U) {
        return I2C_LINES_OK;
    }
    /* more than half low = held low (a stuck line reads the same every
     * time; a single odd sample is noise) */
    if ((unsigned)lines->scl_low * 2U > lines->samples) {
        return I2C_LINES_SCL_LOW;
    }
    if ((unsigned)lines->sda_low * 2U > lines->samples) {
        return I2C_LINES_SDA_LOW;
    }
    return I2C_LINES_OK;
}

/* Release SCL and wait for it to read high (clock stretching). */
static bool scl_release_wait(const i2c_recover_io_t *io, esp_err_t *err)
{
    *err = io->set_scl(io->ctx, 1);
    if (*err != ESP_OK) {
        return false;
    }
    for (unsigned i = 0U; i <= I2C_RECOVER_SCL_WAIT_HALVES; ++i) {
        io->delay_half(io->ctx);
        if (io->read_scl(io->ctx) != 0) {
            return true;
        }
    }
    return false;
}

/* error path: leave both lines released whatever happened */
static esp_err_t release_both(const i2c_recover_io_t *io, esp_err_t first_err)
{
    (void)io->set_scl(io->ctx, 1);
    (void)io->set_sda(io->ctx, 1);
    return first_err;
}

esp_err_t i2c_recover_run(const i2c_recover_io_t *io, i2c_recover_result_t *out)
{
    if (io == NULL || out == NULL || io->read_sda == NULL || io->read_scl == NULL ||
        io->set_sda == NULL || io->set_scl == NULL || io->delay_half == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out->outcome = I2C_RECOVER_IDLE;
    out->clocks = 0U;
    out->sda_high_after = false;
    out->scl_high_after = false;

    /* 1. release both */
    esp_err_t err = io->set_sda(io->ctx, 1);
    if (err != ESP_OK) {
        return release_both(io, err);
    }
    if (!scl_release_wait(io, &err)) {
        if (err != ESP_OK) {
            return release_both(io, err);
        }
        /* 2. SCL held by someone else: do not drive anything more */
        out->outcome = I2C_RECOVER_SCL_STUCK;
        out->sda_high_after = io->read_sda(io->ctx) != 0;
        return ESP_OK;
    }
    const bool sda_was_low = io->read_sda(io->ctx) == 0;

    /* 3. clocks until SDA is released, at most 9 */
    while (io->read_sda(io->ctx) == 0 && out->clocks < I2C_RECOVER_MAX_CLOCKS) {
        err = io->set_scl(io->ctx, 0);
        if (err != ESP_OK) {
            return release_both(io, err);
        }
        io->delay_half(io->ctx);
        ++out->clocks;
        if (!scl_release_wait(io, &err)) {
            if (err != ESP_OK) {
                return release_both(io, err);
            }
            out->outcome = I2C_RECOVER_SCL_STUCK;
            out->sda_high_after = io->read_sda(io->ctx) != 0;
            return ESP_OK;
        }
    }

    /* 4. STOP: SCL low, SDA low, SCL high, SDA high */
    err = io->set_scl(io->ctx, 0);
    if (err != ESP_OK) {
        return release_both(io, err);
    }
    io->delay_half(io->ctx);
    err = io->set_sda(io->ctx, 0);
    if (err != ESP_OK) {
        return release_both(io, err);
    }
    io->delay_half(io->ctx);
    if (!scl_release_wait(io, &err)) {
        if (err != ESP_OK) {
            return release_both(io, err);
        }
        (void)io->set_sda(io->ctx, 1);
        out->outcome = I2C_RECOVER_SCL_STUCK;
        out->sda_high_after = io->read_sda(io->ctx) != 0;
        return ESP_OK;
    }
    err = io->set_sda(io->ctx, 1);
    if (err != ESP_OK) {
        return release_both(io, err);
    }
    io->delay_half(io->ctx);

    out->sda_high_after = io->read_sda(io->ctx) != 0;
    out->scl_high_after = io->read_scl(io->ctx) != 0;
    if (!sda_was_low) {
        out->outcome = I2C_RECOVER_IDLE;
    } else if (out->sda_high_after) {
        out->outcome = I2C_RECOVER_RECOVERED;
    } else {
        out->outcome = I2C_RECOVER_SDA_STILL_LOW;
    }
    return ESP_OK;
}

i2c_probe_t i2c_probe_from_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return I2C_PROBE_OK;
    case ESP_ERR_NOT_FOUND:
        return I2C_PROBE_NACK;
    case ESP_ERR_TIMEOUT:
        return I2C_PROBE_TIMEOUT;
    default:
        return I2C_PROBE_ERROR;
    }
}

i2c_case_t i2c_case_classify(i2c_lines_state_t before, i2c_probe_t aht21, i2c_probe_t ens160)
{
    if (before == I2C_LINES_SCL_LOW) {
        return I2C_CASE_B;
    }
    if (before == I2C_LINES_SDA_LOW) {
        return I2C_CASE_A;
    }
    if (aht21 == I2C_PROBE_OK && ens160 == I2C_PROBE_OK) {
        return I2C_CASE_NORMAL;
    }
    if (aht21 == I2C_PROBE_NACK && ens160 == I2C_PROBE_NACK) {
        return I2C_CASE_C;
    }
    return I2C_CASE_OTHER;
}

bool i2c_probe_wants_reset(i2c_probe_t aht21, i2c_probe_t ens160)
{
    return aht21 != I2C_PROBE_OK || ens160 != I2C_PROBE_OK;
}

const char *i2c_lines_state_name(i2c_lines_state_t s)
{
    switch (s) {
    case I2C_LINES_OK:
        return "high";
    case I2C_LINES_SDA_LOW:
        return "SDA_LOW";
    case I2C_LINES_SCL_LOW:
        return "SCL_LOW";
    default:
        return "?";
    }
}

const char *i2c_recover_outcome_name(i2c_recover_outcome_t o)
{
    switch (o) {
    case I2C_RECOVER_IDLE:
        return "idle";
    case I2C_RECOVER_RECOVERED:
        return "recovered";
    case I2C_RECOVER_SDA_STILL_LOW:
        return "SDA_STILL_LOW";
    case I2C_RECOVER_SCL_STUCK:
        return "SCL_STUCK";
    default:
        return "?";
    }
}

const char *i2c_probe_name(i2c_probe_t p)
{
    switch (p) {
    case I2C_PROBE_OK:
        return "OK";
    case I2C_PROBE_NACK:
        return "NACK";
    case I2C_PROBE_TIMEOUT:
        return "TIMEOUT";
    case I2C_PROBE_ERROR:
        return "ERROR";
    case I2C_PROBE_SKIPPED:
        return "-";
    default:
        return "?";
    }
}

const char *i2c_case_name(i2c_case_t c)
{
    switch (c) {
    case I2C_CASE_NORMAL:
        return "N";
    case I2C_CASE_A:
        return "A";
    case I2C_CASE_B:
        return "B";
    case I2C_CASE_C:
        return "C";
    case I2C_CASE_OTHER:
        return "X";
    default:
        return "?";
    }
}
