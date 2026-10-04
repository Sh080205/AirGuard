#include "console.h"

#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "logutil.h"
#include "task_config.h"
#include "time_sync.h"

/* [ขึ้นกับเวอร์ชัน ESP-IDF] primary console UART number from sdkconfig
 * (CONFIG_ESP_CONSOLE_UART_DEFAULT → UART0 on GPIO43/44, CLAUDE.md §4) */
#define CONSOLE_UART CONFIG_ESP_CONSOLE_UART_NUM

/* RX ring must be larger than the 128-byte hardware FIFO (driver rule);
 * a command line is < 64 bytes, so 256 B holds several pasted lines. */
#define CONSOLE_RX_BUFFER 256
_Static_assert(CONSOLE_RX_BUFFER > UART_HW_FIFO_LEN(CONSOLE_UART),
               "UART RX ring buffer must exceed hardware FIFO");

/* "settime " (8) + 25-char ISO time = 33; 64 leaves room for typos/spaces. */
#define CMD_LINE_MAX 64

/* Reject clocks before this year: a typo such as 2016 would otherwise
 * produce a dataset with plausible-looking but wrong dates. The project
 * collects data in 2026 (CLAUDE.md §10). */
#define MIN_VALID_YEAR 2026

static const char *TAG = "console";

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\t') {
        ++lead;
    }
    if (lead > 0) {
        memmove(s, s + lead, n - lead + 1);
    }
}

static void handle_settime(const char *arg)
{
    int64_t epoch = 0;
    if (!logutil_parse_iso8601(arg, &epoch)) {
        ESP_LOGE(TAG, "settime: bad time \"%s\"; want YYYY-MM-DDTHH:MM:SS+07:00", arg);
        return;
    }
    const int64_t min_epoch = logutil_days_from_civil(MIN_VALID_YEAR, 1, 1) * 86400 - 7 * 3600;
    if (epoch < min_epoch) {
        ESP_LOGE(TAG, "settime: year before %d rejected", MIN_VALID_YEAR);
        return;
    }
    const esp_err_t err = time_sync_set_manual(epoch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "settime: settimeofday failed: %s", esp_err_to_name(err));
        return;
    }
    const time_t t = (time_t)epoch;
    struct tm local;
    if (localtime_r(&t, &local) == NULL) {
        ESP_LOGI(TAG, "settime OK (epoch %lld)", (long long)epoch);
        return;
    }
    ESP_LOGI(TAG, "settime OK: %04d-%02d-%02dT%02d:%02d:%02d+07:00 (SNTP will override if it syncs)",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

static void handle_line(char *line)
{
    trim(line);
    if (line[0] == '\0') {
        return;
    }
    if (strncmp(line, "settime", 7) == 0 && (line[7] == ' ' || line[7] == '\0')) {
        char *arg = line + 7;
        trim(arg);
        handle_settime(arg);
    } else if (strcmp(line, "help") == 0) {
        ESP_LOGI(TAG, "commands: settime YYYY-MM-DDTHH:MM:SS+07:00 | help");
    } else {
        ESP_LOGW(TAG, "unknown command \"%s\" (try: help)", line);
    }
}

static void console_task(void *arg)
{
    (void)arg;
    char line[CMD_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        /* blocks until one byte arrives; this task has nothing else to do */
        const int n = uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY);
        if (n < 0) {
            ESP_LOGE(TAG, "uart_read_bytes failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (n == 0) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (overflow) {
                ESP_LOGW(TAG, "line longer than %d characters ignored", CMD_LINE_MAX - 1);
            } else {
                line[len] = '\0';
                handle_line(line);
            }
            len = 0;
            overflow = false;
        } else if (c == 0x08 || c == 0x7f) {   /* backspace / delete */
            if (len > 0) {
                --len;
            }
        } else if (len < CMD_LINE_MAX - 1) {
            line[len++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

esp_err_t console_start(TaskHandle_t *out_task)
{
    /* RX only (TX ring 0): log output keeps using the default console path */
    esp_err_t err = uart_driver_install(CONSOLE_UART, CONSOLE_RX_BUFFER, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    const BaseType_t ok = xTaskCreate(console_task, "console", CONSOLE_STACK_BYTES,
                                      NULL, CONSOLE_TASK_PRIORITY, out_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
