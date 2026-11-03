#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "console.h"
#include "ml_mailbox.h"
#include "tasks_co.h"
#include "tasks_io.h"
#include "tasks_oled.h"
#include "tasks_sampler.h"
#include "tasks_storage.h"
#include "tasks_web.h"
#include "time_sync.h"
#include "web_mailbox.h"

/* MARK presses waiting for the sampler. A press is applied at the next 1 s
 * cycle, so more than a couple can only queue up if the sampler is stuck;
 * 8 is generous. ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define MARK_QUEUE_LEN 8U

static const char *TAG = "airguard";

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "EFUSE";
    case ESP_RST_PWR_GLITCH: return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP: return "CPU_LOCKUP";
    case ESP_RST_UNKNOWN:
    default: return "UNKNOWN";
    }
}

static void nvs_init(void)
{
    /* Wi-Fi keeps PHY calibration in NVS. A full or version-changed NVS
     * partition is erased and re-created (ESP-IDF Wi-Fi example pattern). */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS: %s; erasing", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    /* FW-4 §7: once per boot; BROWNOUT here points at Wi-Fi TX current */
    const esp_reset_reason_t reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reset reason: %s (%d)", reset_reason_name(reason), (int)reason);

    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    /* v5.5: API นี้รายงานขนาดตาม image configuration ไม่ใช่ physical chip size */
    ESP_LOGI(TAG, "Flash (image configured): %" PRIu32 " bytes (%" PRIu32 " MiB)",
             flash_size, flash_size / (1024U * 1024U));
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM (available): %zu bytes (%zu MiB)",
             psram_size, psram_size / (1024U * 1024U));

    QueueHandle_t mark_queue = xQueueCreate(MARK_QUEUE_LEN, sizeof(io_mark_event_t));
    ESP_ERROR_CHECK(mark_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    /* FW-5b: the CO path starts first, right after the io task that shows
     * it, and before NVS/Wi-Fi/console/SD/sensors, so none of their init
     * problems can delay or stop it (CLAUDE.md §2). The mailbox must exist
     * before io_init() creates the io task that reads it. */
    ESP_ERROR_CHECK(co_mailbox_init());
    /* FW-7: the ML result mailbox, also before io_init() because FW-6
     * reads it from the io task. Static queue, no allocation; an error is
     * only logged, never aborts (the ML score must not stop the CO path). */
    const esp_err_t ml_err = ml_mailbox_init();
    if (ml_err != ESP_OK) {
        ESP_LOGE(TAG, "ML mailbox init: %s - FW-6 gets no ML result", esp_err_to_name(ml_err));
    }
    /* FW-8: dashboard mailboxes, before io_init()/sampler_start() whose
     * tasks write them. Static queues; an error is only logged (the page
     * then shows nulls), never aborts. */
    const esp_err_t web_mb_err = web_mailbox_init();
    if (web_mb_err != ESP_OK) {
        ESP_LOGE(TAG, "web mailbox init: %s - dashboard gets no data", esp_err_to_name(web_mb_err));
    }
    io_init(mark_queue);
    ESP_ERROR_CHECK(io_set_mode(NORMAL));
    ESP_ERROR_CHECK(io_set_indication(IO_IND_WAIT_TIME));
    const esp_err_t co_err = co_start();
    if (co_err != ESP_OK) {
        /* the CO task (if created) publishes FAULT; io shows it */
        ESP_LOGE(TAG, "CO path start: %s - shown as CO FAULT", esp_err_to_name(co_err));
    }

    nvs_init();

    /* never aborts on Wi-Fi problems; settime stays available */
    time_sync_start();

    TaskHandle_t console_task = NULL;
    const esp_err_t console_err = console_start(&console_task);
    if (console_err != ESP_OK) {
        ESP_LOGE(TAG, "console start failed: %s (settime unavailable)",
                 esp_err_to_name(console_err));
    }

    /* SPI bus + queues only; a missing/broken card is handled inside the
     * storage task and never stops the rest of the system */
    TaskHandle_t storage_task = NULL;
    /* FW-5b: a storage/sampler init error no longer aborts: abort would
     * reboot in a loop and take the CO path down with it. The logger part
     * is then broken (logged here every boot); CO keeps running. */
    const esp_err_t storage_err = storage_start(&storage_task);
    if (storage_err != ESP_OK) {
        ESP_LOGE(TAG, "storage start failed: %s - rows will not be written; CO path keeps running",
                 esp_err_to_name(storage_err));
    }

    /* OLED: the row mailbox exists before the sampler task that writes it.
     * Static queue; a failure only means no screen. */
    const esp_err_t oled_mb_err = oled_mailbox_init();
    if (oled_mb_err != ESP_OK && oled_mb_err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGE(TAG, "OLED mailbox init: %s - no screen", esp_err_to_name(oled_mb_err));
    }

    const sampler_config_t sampler_cfg = {
        .mark_queue = mark_queue,
        .storage_task = storage_task,
        .console_task = console_task,
    };
    const esp_err_t sampler_err = sampler_start(&sampler_cfg);
    if (sampler_err != ESP_OK) {
        ESP_LOGE(TAG, "sampler start failed: %s - no sensor rows; CO path keeps running",
                 esp_err_to_name(sampler_err));
    }

    /* OLED: on the bus the sampler created. Never aborts: the screen
     * is optional; the sampler, CO path and logging do not depend on it. */
    const esp_err_t oled_err = oled_start(sampler_i2c_bus());
    if (oled_err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "OLED: not built (OLED_ENABLE=0)");
    } else if (oled_err != ESP_OK) {
        ESP_LOGE(TAG, "OLED start: %s - no screen; sampler, CO path and logging keep running",
                 esp_err_to_name(oled_err));
    }

    /* FW-8: last, after everything it mirrors; needs Wi-Fi from
     * time_sync_start(). A failure only loses the page (logged here). */
    const esp_err_t web_err = web_start();
    if (web_err != ESP_OK) {
        ESP_LOGW(TAG, "dashboard not started: %s - device keeps running", esp_err_to_name(web_err));
    }

    /* app_main ใช้ stack ที่ IDF จัดให้และจบที่นี่ */
    ESP_LOGI(TAG, "main stack high-water mark: %u bytes (before exit)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
