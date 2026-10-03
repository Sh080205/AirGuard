#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "tasks_io.h"

static const char *TAG = "airguard";

void app_main(void)
{
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_size));

    /* v5.5: API นี้รายงานขนาดตาม image configuration ไม่ใช่ physical chip size */
    ESP_LOGI(TAG, "Flash (image configured): %" PRIu32 " bytes (%" PRIu32 " MiB)",
             flash_size, flash_size / (1024U * 1024U));
    const size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "PSRAM (available): %zu bytes (%zu MiB)",
             psram_size, psram_size / (1024U * 1024U));

    io_init();
    ESP_ERROR_CHECK(io_set_mode(NORMAL));

    /* app_main ใช้ stack ที่ IDF จัดให้และจบที่นี่; task ถาวรมีเพียง io */
    ESP_LOGI(TAG, "main stack high-water mark: %u bytes (before exit)",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
