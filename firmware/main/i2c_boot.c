#include "i2c_boot.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "aht21.h"
#include "ens160.h"

static const char *TAG = "i2c_boot";

/* Half SCL period of the bit-banged bus clear: 5 µs = 100 kHz, the same
 * speed as the bus (tasks_sampler.c I2C_SCL_HZ; AHT21 10-400 kHz CLAUDE.md
 * §5, ENS160 <= 1 MHz). A preemption only makes one half period longer;
 * the clear does not depend on a minimum speed (static slave logic). */
#define I2C_BOOT_HALF_US 5U
/* Same value as tasks_oled.c OLED_I2C_ADDR (SSD1306/SH1106 module, D/C
 * strap low). Probed for the log only; a missing screen is normal. */
#define I2C_BOOT_OLED_ADDR 0x3C
/* i2c_master_probe() timeout; the driver itself uses a 20 ms SCL timeout
 * inside the probe (v5.5.5 i2c_master.c) [ขึ้นกับเวอร์ชัน ESP-IDF] */
#define I2C_BOOT_PROBE_TIMEOUT_MS 20
/* AHT21 needs >= 100 ms after power-on before the first command (CLAUDE.md
 * §5); a probe earlier could NACK on a healthy bus and look like case C.
 * esp_timer starts after power-on, so waiting until it reads 100 ms is at
 * least 100 ms of sensor power (same rule as tasks_sampler.c aht21_boot). */
#define I2C_BOOT_POWER_ON_WAIT_US (100LL * 1000)
/* i2c_master_bus_reset() starts the S3 hardware bus clear and returns
 * without waiting for it (proposal §2.2: i2c_ll_master_is_bus_clear_done()
 * is "not supported on esp32s3"). 9 clocks at 100 kHz take ~90 µs; wait
 * 1 ms before probing again. ค่าคาดหมาย ต้องยืนยันด้วยการวัด. */
#define I2C_BOOT_AFTER_RESET_US 1000U

static const uint16_t s_addr[I2C_BOOT_ADDR_N] = {
    AHT21_I2C_ADDR, ENS160_I2C_ADDR, I2C_BOOT_OLED_ADDR,
};

static i2c_boot_report_t s_rep = {
    .before = I2C_LINES_OK,
    .gpio_err = ESP_OK,
    .bus_reset_err = ESP_OK,
    .first = { I2C_PROBE_SKIPPED, I2C_PROBE_SKIPPED, I2C_PROBE_SKIPPED },
    .final = { I2C_PROBE_SKIPPED, I2C_PROBE_SKIPPED, I2C_PROBE_SKIPPED },
    .kase = I2C_CASE_OTHER,
};

typedef struct {
    gpio_num_t sda;
    gpio_num_t scl;
} pins_t;

static int rd_sda(void *ctx) { return gpio_get_level(((const pins_t *)ctx)->sda); }
static int rd_scl(void *ctx) { return gpio_get_level(((const pins_t *)ctx)->scl); }
static esp_err_t wr_sda(void *ctx, int level) { return gpio_set_level(((const pins_t *)ctx)->sda, (uint32_t)level); }
static esp_err_t wr_scl(void *ctx, int level) { return gpio_set_level(((const pins_t *)ctx)->scl, (uint32_t)level); }
static void half(void *ctx)
{
    (void)ctx;
    esp_rom_delay_us(I2C_BOOT_HALF_US);
}

/* Open-drain, no internal pulls: the board's 4.7 kΩ pull-ups (CLAUDE.md
 * §4) set the high level; the pin only ever pulls low or lets go. The output
 * latch is set to 1 (released) BEFORE the pin becomes an output, so the
 * switch-over itself never pulls a line low. */
static esp_err_t pins_open_drain(const pins_t *p)
{
    esp_err_t err = gpio_set_level(p->sda, 1);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(p->scl, 1);
    if (err != ESP_OK) {
        return err;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << p->sda) | (1ULL << p->scl),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

void i2c_boot_recover(int sda_gpio, int scl_gpio)
{
    static pins_t pins;
    pins.sda = (gpio_num_t)sda_gpio;
    pins.scl = (gpio_num_t)scl_gpio;
    const i2c_recover_io_t io = {
        .ctx = &pins, .read_sda = rd_sda, .read_scl = rd_scl,
        .set_sda = wr_sda, .set_scl = wr_scl, .delay_half = half,
    };
    const int64_t t0 = esp_timer_get_time();
    s_rep.recover_ran = true;

    esp_err_t err = pins_open_drain(&pins);
    if (err != ESP_OK) {
        s_rep.gpio_err = err;
        ESP_LOGE(TAG, "SDA/SCL open-drain config failed: %s; bus clear skipped",
                 esp_err_to_name(err));
        return;
    }
    /* D1: both outputs released, so a low line is held by someone else */
    i2c_recover_sample(&io, I2C_RECOVER_SAMPLES, &s_rep.lines);
    s_rep.before = i2c_lines_state(&s_rep.lines);

    /* R1 (also on a healthy bus: then it is only a STOP, harmless) */
    err = i2c_recover_run(&io, &s_rep.r1);
    s_rep.r1_us = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        s_rep.gpio_err = err;
        ESP_LOGE(TAG, "bus clear: GPIO write failed: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "D1 lines before: SDA low %u/%u SCL low %u/%u (%s) | R1 %s clocks=%u "
             "after: SDA=%d SCL=%d | %" PRId64 " us",
             s_rep.lines.sda_low, s_rep.lines.samples, s_rep.lines.scl_low, s_rep.lines.samples,
             i2c_lines_state_name(s_rep.before), i2c_recover_outcome_name(s_rep.r1.outcome),
             s_rep.r1.clocks, (int)s_rep.r1.sda_high_after, (int)s_rep.r1.scl_high_after,
             s_rep.r1_us);
    if (s_rep.r1.outcome == I2C_RECOVER_SDA_STILL_LOW) {
        ESP_LOGE(TAG, "SDA still low after %u clocks: a device holds SDA; only a power "
                      "cycle of the sensors helps (unplug USB)", I2C_RECOVER_MAX_CLOCKS);
    } else if (s_rep.r1.outcome == I2C_RECOVER_SCL_STUCK) {
        ESP_LOGE(TAG, "SCL held low (case B): bus clear cannot help; unplug USB to "
                      "power-cycle the sensors");
    }
}

static void probe_all(i2c_master_bus_handle_t bus, i2c_probe_t out[I2C_BOOT_ADDR_N])
{
    for (int i = 0; i < I2C_BOOT_ADDR_N; ++i) {
        out[i] = i2c_probe_from_err(i2c_master_probe(bus, s_addr[i], I2C_BOOT_PROBE_TIMEOUT_MS));
    }
}

void i2c_boot_probe(i2c_master_bus_handle_t bus)
{
    if (bus == NULL) {
        return;
    }
    const int64_t since_boot = esp_timer_get_time();
    if (since_boot < I2C_BOOT_POWER_ON_WAIT_US) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((I2C_BOOT_POWER_ON_WAIT_US - since_boot) / 1000) + 1U));
    }
    s_rep.probe_ran = true;
    probe_all(bus, s_rep.first);
    for (int i = 0; i < I2C_BOOT_ADDR_N; ++i) {
        s_rep.final[i] = s_rep.first[i];
    }
    if (i2c_probe_wants_reset(s_rep.first[0], s_rep.first[1])) {
        /* proposal §4.2 case D: lines fine, our side not; the driver never
         * clears the bus when it is created (§2.2), so do it once here */
        s_rep.bus_reset = true;
        s_rep.bus_reset_err = i2c_master_bus_reset(bus);
        if (s_rep.bus_reset_err != ESP_OK) {
            ESP_LOGE(TAG, "i2c_master_bus_reset: %s", esp_err_to_name(s_rep.bus_reset_err));
        } else {
            esp_rom_delay_us(I2C_BOOT_AFTER_RESET_US);
            probe_all(bus, s_rep.final);
        }
    }
    s_rep.kase = i2c_case_classify(s_rep.before, s_rep.final[0], s_rep.final[1]);
    i2c_boot_log_line();
}

const i2c_boot_report_t *i2c_boot_report(void)
{
    return &s_rep;
}

void i2c_boot_log_line(void)
{
    const i2c_boot_report_t *r = &s_rep;
    const bool bad = r->kase != I2C_CASE_NORMAL;
    const esp_log_level_t level = bad ? ESP_LOG_WARN : ESP_LOG_INFO;
    ESP_LOG_LEVEL(level, TAG,
                  "boot bus: case=%s lines=%s(SDA %u/%u SCL %u/%u) r1=%s clocks=%u gpio_err=%s | "
                  "probe 38=%s 53=%s 3C=%s | reset=%s%s | final 38=%s 53=%s 3C=%s",
                  i2c_case_name(r->kase), i2c_lines_state_name(r->before),
                  r->lines.sda_low, r->lines.samples, r->lines.scl_low, r->lines.samples,
                  r->recover_ran ? i2c_recover_outcome_name(r->r1.outcome) : "not run",
                  r->r1.clocks, esp_err_to_name(r->gpio_err),
                  i2c_probe_name(r->first[0]), i2c_probe_name(r->first[1]),
                  i2c_probe_name(r->first[2]),
                  r->bus_reset ? "yes" : "no",
                  r->bus_reset && r->bus_reset_err != ESP_OK ? "(FAILED)" : "",
                  i2c_probe_name(r->final[0]), i2c_probe_name(r->final[1]),
                  i2c_probe_name(r->final[2]));
}
