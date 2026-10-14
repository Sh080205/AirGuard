#include "tasks_storage.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/queue.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"

#include "board_pins.h"
#include "logutil.h"
#include "task_config.h"

/* FW-4 §8: AG_YYYYMMDD_HHMMSS.csv (22 chars) is not 8.3. Long file names
 * must be on, set in sdkconfig.defaults [ขึ้นกับเวอร์ชัน ESP-IDF]. An old
 * firmware/sdkconfig keeps LFN off — delete it so the defaults apply. */
#if !defined(CONFIG_FATFS_LFN_HEAP) && !defined(CONFIG_FATFS_LFN_STACK)
#error "FATFS long file names are off: delete firmware/sdkconfig and rebuild (see sdkconfig.defaults)"
#endif

#define MOUNT_POINT "/sdcard"

/* SPI2 (FSPI) is the general-purpose SPI host on ESP32-S3; pins come from
 * the GPIO matrix (board_pins.h, CLAUDE.md §4). */
#define SD_SPI_HOST SPI2_HOST

/* SD clock in SPI mode. Need is tiny (~100 B/s); 10 MHz writes a 512 B
 * sector in ~0.4 ms and leaves margin for the J6 header/module wiring,
 * whose pull-ups and level shifting are unknown [ต้องดู schematic โมดูล].
 * Half of SDMMC_FREQ_DEFAULT (20 MHz). ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define SD_FREQ_KHZ 10000

/* fsync every N rows. Trade-off:
 * - fflush() each row only hands the bytes to FatFS; the data sector and
 *   the directory entry (file size) reach the card on f_sync (fsync) or
 *   when a 512 B sector fills (~6 rows of ~60-99 B). A power cut loses
 *   everything since the last fsync: at most N s of rows (+ rows still in
 *   the queue).
 * - Each fsync rewrites the partial data sector + directory sector (+ FAT
 *   when a cluster is added). N = 1 → ~2 sector writes/s, ~170k/day on
 *   the same sectors; N = 10 → ~17k/day. The card's FTL spreads wear, but
 *   fewer small rewrites also mean fewer long FTL stalls.
 * N = 10: losing <= 10 s of a multi-day 1 Hz dataset is negligible, and
 * wear drops 10x versus syncing every row.
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด (fsync max latency in the 60 s summary) */
#define FSYNC_EVERY_N_ROWS 10

/* Remount attempt period while the card is missing or failed. Each attempt
 * on an empty slot costs card-init timeouts on this task only.
 * ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define REMOUNT_INTERVAL_MS 5000

/* Max wait for a row before the loop re-checks events/report time. */
#define ROW_WAIT_MS 500

#define REPORT_INTERVAL_US (60LL * 1000 * 1000)

/* "/sdcard/" + 22-char name + NUL = 31; 48 gives slack. */
#define PATH_LEN 48
/* Longest data row is 99 characters (csv_row.h) + NUL. */
#define ROW_TEXT_LEN 128

/* Serial CSV mirror (14 Oct 2569 · docs/bringup/2026-10-14.md P5: the SD
 * socket was unusable). Every row is also printed on the console as
 * "AGCSV,<row>" so a PC can log the data; firmware/tools/serial_logger.py
 * turns those lines back into AG_*.csv in the CLAUDE.md §7 format.
 * The printing is done here, in the slow-I/O task, never in the 1 Hz
 * sampler: if the console stalls, the row queue absorbs it and the sampler
 * only counts drops (CLAUDE.md §2). The time each print takes is measured
 * and reported every 60 s. 0 = plain FW-4 behaviour. */
#ifndef STORAGE_SERIAL_MIRROR
#define STORAGE_SERIAL_MIRROR 1
#endif
#define SERIAL_MIRROR_PREFIX "AGCSV,"
#define SERIAL_EVENT_PREFIX "AGEV,"     /* MARK rows, EV_*.csv format */
/* SD mounted earlier but gone now: rows are taken out only to print them
 * once this many are queued (logutil_mirror_should_drain), so the queue
 * still rides out a remount for the card and the sampler never drops. */
#define MIRROR_HIGH_WATER (STORAGE_ROW_QUEUE_LEN - 4U)

_Static_assert(STORAGE_ROW_QUEUE_LEN >= 30, "FW-4 §7: queue must hold >= 30 s of rows");
_Static_assert(MIRROR_HIGH_WATER >= 30, "FW-4 §7: mirror drain must leave >= 30 s of SD buffer");
_Static_assert(sizeof(csv_sample_t) <= 96, "csv_sample_t grew; recheck queue RAM in tasks_storage.h");

static const char *TAG = "storage";

static StaticQueue_t s_row_q_struct;
static uint8_t s_row_q_storage[STORAGE_ROW_QUEUE_LEN * sizeof(csv_sample_t)];
static QueueHandle_t s_row_q;
static StaticQueue_t s_ev_q_struct;
static uint8_t s_ev_q_storage[STORAGE_EVENT_QUEUE_LEN * sizeof(storage_event_t)];
static QueueHandle_t s_ev_q;

/* Everything below is owned by the storage task only. */
typedef struct {
    sdmmc_card_t *card;
    bool mounted;
    bool ever_mounted;           /* mounted at least once since boot */
    int64_t next_mount_us;
    FILE *file;
    struct tm file_day;
    char path[PATH_LEN];
    char ev_path[PATH_LEN];
    int rows_since_sync;
    /* summary counters */
    uint32_t rows_written;
    uint32_t write_errors;
    uint32_t mounts_ok;
    uint32_t mounts_failed;
    uint32_t events_written;
    logutil_stats_t write_us;    /* fwrite + fflush (+ fsync) per row, window */
    logutil_stats_t fsync_us;    /* fsync alone, window */
    int64_t write_us_max_boot;
    /* serial mirror counters (since boot) and print time (window) */
    uint32_t rows_mirrored;      /* printed as AGCSV */
    uint32_t rows_serial_only;   /* taken out while unmounted: never on SD */
    uint32_t mirror_errors;      /* printf/fflush failed: row not on serial */
    logutil_stats_t mirror_us;
    int64_t next_report_us;
} storage_state_t;

static esp_err_t sd_mount(storage_state_t *st)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SD_SPI_HOST;
    slot.gpio_cs = BOARD_PIN_SD_CS;

    const esp_vfs_fat_mount_config_t mount_cfg = {
        /* never format: the card holds the only copy of collected data */
        .format_if_mount_failed = false,
        /* data CSV + EV file open at the same time, +1 spare */
        .max_files = 3,
        .allocation_unit_size = 0,
        /* real ff_disk_status: helps notice a card pulled out without
         * unmount (esp_vfs_fat.h) at a small cost per access; our I/O is
         * ~100 B/s so the cost does not matter [ขึ้นกับเวอร์ชัน ESP-IDF] */
        .disk_status_check_enable = true,
        .use_one_fat = false,
    };
    /* [ขึ้นกับเวอร์ชัน ESP-IDF] esp_vfs_fat_sdspi_mount signature in v5.5 */
    return esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount_cfg, &st->card);
}

static void close_file(storage_state_t *st, bool sync)
{
    if (st->file == NULL) {
        return;
    }
    if (sync && fsync(fileno(st->file)) != 0) {
        ESP_LOGE(TAG, "fsync %s: errno %d", st->path, errno);
    }
    if (fclose(st->file) != 0) {
        ESP_LOGE(TAG, "fclose %s: errno %d", st->path, errno);
    }
    st->file = NULL;
    st->rows_since_sync = 0;
}

/* Any write error: assume the card is gone or corrupt. Close without sync
 * (it would fail too), unmount, and let the loop retry the mount later.
 * The row that failed stays pending in the caller and is written to the
 * next file after remount. */
static void sd_fail(storage_state_t *st, const char *what)
{
    ++st->write_errors;
    ESP_LOGE(TAG, "%s failed (errno %d); unmounting, retry every %d ms",
             what, errno, REMOUNT_INTERVAL_MS);
    close_file(st, false);
    st->ev_path[0] = '\0';
    if (st->mounted) {
        const esp_err_t err = esp_vfs_fat_sdcard_unmount(MOUNT_POINT, st->card);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "unmount: %s", esp_err_to_name(err));
        }
    }
    st->card = NULL;
    st->mounted = false;
    st->next_mount_us = esp_timer_get_time() + (int64_t)REMOUNT_INTERVAL_MS * 1000;
}

static bool file_is_empty(const char *path)
{
    struct stat sb;
    return stat(path, &sb) != 0 || sb.st_size == 0;
}

/* Write text, flush and fsync; used for headers and event rows that must
 * reach the card immediately. */
static bool write_synced(FILE *f, const char *text, size_t len)
{
    return fwrite(text, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
}

/* New data file named after the first row it will hold: once per boot,
 * at local midnight, and after every remount (FW-4 §2). Appends when the
 * name already exists (same second after a quick reset); the header is
 * written only into an empty file. */
static bool open_data_file(storage_state_t *st, const struct tm *t)
{
    char name[32];
    if (csv_filename(t, "AG", name, sizeof(name)) == 0) {
        ESP_LOGE(TAG, "csv_filename failed");
        return false;
    }
    snprintf(st->path, sizeof(st->path), MOUNT_POINT "/%s", name);
    const bool empty = file_is_empty(st->path);
    st->file = fopen(st->path, "a");
    if (st->file == NULL) {
        return false;
    }
    if (empty) {
        char header[ROW_TEXT_LEN];
        const size_t n = csv_header(header, sizeof(header));
        if (n == 0 || !write_synced(st->file, header, n)) {
            return false;
        }
    }
    /* EV file pairs with the data file by the same timestamp suffix */
    if (csv_filename(t, "EV", name, sizeof(name)) == 0) {
        return false;
    }
    snprintf(st->ev_path, sizeof(st->ev_path), MOUNT_POINT "/%s", name);
    st->file_day = *t;
    st->rows_since_sync = 0;
    ESP_LOGI(TAG, "logging to %s%s", st->path, empty ? "" : " (appending)");
    return true;
}

/* Print one formatted line (ends in '\n') as "<prefix><text>" with a single
 * printf, so the line is not split by log output from other tasks, then
 * flush. stdout goes to UART0 and USB-Serial/JTAG (secondary console).
 * true = printed. */
static bool mirror_line(storage_state_t *st, const char *prefix, const char *text)
{
#if STORAGE_SERIAL_MIRROR
    const int64_t t0 = esp_timer_get_time();
    const bool ok = printf("%s%s", prefix, text) >= 0 && fflush(stdout) == 0;
    if (!ok) {
        ++st->mirror_errors;
    }
    logutil_stats_add(&st->mirror_us, esp_timer_get_time() - t0);
    return ok;
#else
    (void)st;
    (void)prefix;
    (void)text;
    return false;
#endif
}

static void mirror_row(storage_state_t *st, const char *text)
{
    if (mirror_line(st, SERIAL_MIRROR_PREFIX, text)) {
        ++st->rows_mirrored;
    }
}

/* Events are printed when they leave their queue, like rows. */
static void mirror_event(storage_state_t *st, const storage_event_t *ev)
{
#if STORAGE_SERIAL_MIRROR
    char text[ROW_TEXT_LEN];
    if (csv_event_row(&ev->start, &ev->end, text, sizeof(text)) != 0) {
        (void)mirror_line(st, SERIAL_EVENT_PREFIX, text);
    }
#else
    (void)st;
    (void)ev;
#endif
}

/* Format a row for both outputs. 0 = rejected (logged, row dropped). */
static size_t format_row(const csv_sample_t *row, char *text, size_t cap)
{
    const size_t len = csv_format_row(row, text, cap);
    if (len == 0) {
        ESP_LOGE(TAG, "csv_format_row rejected a row; dropped");
    }
    return len;
}

/* While unmounted: take rows out of the queue only to print them, as
 * logutil_mirror_should_drain decides. These rows never reach the SD card. */
static void drain_to_serial(storage_state_t *st)
{
    while (logutil_mirror_should_drain(STORAGE_SERIAL_MIRROR != 0, st->ever_mounted,
                                       storage_rows_waiting(), MIRROR_HIGH_WATER)) {
        csv_sample_t row;
        if (xQueueReceive(s_row_q, &row, 0) != pdTRUE) {
            break;
        }
        char text[ROW_TEXT_LEN];
        if (format_row(&row, text, sizeof(text)) == 0) {
            continue;
        }
        mirror_row(st, text);
        ++st->rows_serial_only;
    }
    /* Events: same rule (high water = all 4 slots but one). Without a card
     * since boot they go straight to serial; after a mount they wait for the
     * card unless the queue is about to overflow. */
    while (logutil_mirror_should_drain(STORAGE_SERIAL_MIRROR != 0, st->ever_mounted,
                                       (unsigned)uxQueueMessagesWaiting(s_ev_q),
                                       STORAGE_EVENT_QUEUE_LEN - 1U)) {
        storage_event_t ev;
        if (xQueueReceive(s_ev_q, &ev, 0) != pdTRUE) {
            break;
        }
        mirror_event(st, &ev);
    }
}

/* text/len: the row already formatted by format_row (len > 0).
 * true = row written; false = SD failure, keep the row for after remount. */
static bool write_row(storage_state_t *st, const csv_sample_t *row,
                      const char *text, size_t len)
{
    if (st->file != NULL && !logutil_same_day(&st->file_day, &row->local_time)) {
        ESP_LOGI(TAG, "midnight: closing %s", st->path);
        close_file(st, true);
    }
    if (st->file == NULL && !open_data_file(st, &row->local_time)) {
        sd_fail(st, "open data file");
        return false;
    }

    const int64_t t0 = esp_timer_get_time();
    if (fwrite(text, 1, len, st->file) != len || fflush(st->file) != 0) {
        sd_fail(st, "fwrite/fflush");
        return false;
    }
    if (++st->rows_since_sync >= FSYNC_EVERY_N_ROWS) {
        const int64_t ts = esp_timer_get_time();
        if (fsync(fileno(st->file)) != 0) {
            sd_fail(st, "fsync");
            return false;
        }
        logutil_stats_add(&st->fsync_us, esp_timer_get_time() - ts);
        st->rows_since_sync = 0;
    }
    const int64_t dt = esp_timer_get_time() - t0;
    logutil_stats_add(&st->write_us, dt);
    if (dt > st->write_us_max_boot) {
        st->write_us_max_boot = dt;
    }
    ++st->rows_written;
    return true;
}

static bool write_event(storage_state_t *st, const storage_event_t *ev)
{
    char text[ROW_TEXT_LEN];
    const size_t len = csv_event_row(&ev->start, &ev->end, text, sizeof(text));
    if (len == 0) {
        ESP_LOGE(TAG, "csv_event_row rejected an event; dropped");
        return true;
    }
    if (st->ev_path[0] == '\0') {
        /* no data file yet in this mount: name the EV file after the event */
        char name[32];
        if (csv_filename(&ev->start, "EV", name, sizeof(name)) == 0) {
            return true;
        }
        snprintf(st->ev_path, sizeof(st->ev_path), MOUNT_POINT "/%s", name);
    }
    const bool empty = file_is_empty(st->ev_path);
    FILE *f = fopen(st->ev_path, "a");
    if (f == NULL) {
        sd_fail(st, "open EV file");
        return false;
    }
    bool ok = true;
    if (empty) {
        char header[48];
        const size_t n = csv_event_header(header, sizeof(header));
        ok = n != 0 && write_synced(f, header, n);
    }
    ok = ok && write_synced(f, text, len);
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        sd_fail(st, "write EV file");
        return false;
    }
    ++st->events_written;
    ESP_LOGI(TAG, "event written to %s", st->ev_path);
    return true;
}

static void maybe_report(storage_state_t *st)
{
    const int64_t now = esp_timer_get_time();
    if (now < st->next_report_us) {
        return;
    }
    st->next_report_us = now + REPORT_INTERVAL_US;
    ESP_LOGI(TAG, "60s: sd=%s rows_written=%" PRIu32 " events=%" PRIu32
             " write_errors=%" PRIu32 " mounts ok/fail=%" PRIu32 "/%" PRIu32
             " queue_now=%u",
             st->mounted ? "mounted" : "NOT MOUNTED", st->rows_written,
             st->events_written, st->write_errors, st->mounts_ok,
             st->mounts_failed, storage_rows_waiting());
    ESP_LOGI(TAG, "60s: row write(fwrite+fflush[+fsync]) avg=%" PRId64 " max=%" PRId64
             " us (n=%" PRIu32 ", boot max %" PRId64 " us) | fsync max=%" PRId64
             " us (n=%" PRIu32 ") | stack HWM %u B",
             logutil_stats_avg(&st->write_us), st->write_us.max, st->write_us.count,
             st->write_us_max_boot, st->fsync_us.max, st->fsync_us.count,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
#if STORAGE_SERIAL_MIRROR
    ESP_LOGI(TAG, "60s: serial mirror rows=%" PRIu32 " (serial only %" PRIu32 ")"
             " errors=%" PRIu32 " | print avg=%" PRId64 " max=%" PRId64 " us (n=%" PRIu32 ")",
             st->rows_mirrored, st->rows_serial_only, st->mirror_errors,
             logutil_stats_avg(&st->mirror_us), st->mirror_us.max, st->mirror_us.count);
    logutil_stats_reset(&st->mirror_us);
#endif
    logutil_stats_reset(&st->write_us);
    logutil_stats_reset(&st->fsync_us);
}

static void storage_task(void *arg)
{
    (void)arg;
    static storage_state_t st;   /* static: keeps ~200 B off the task stack */
    st.next_report_us = esp_timer_get_time() + REPORT_INTERVAL_US;
    csv_sample_t row;
    char row_text[ROW_TEXT_LEN];   /* formatted once, for serial and SD */
    size_t row_len = 0;
    bool have_row = false;         /* dequeued and printed, not yet on SD */
    storage_event_t ev;
    bool have_ev = false;

    for (;;) {
        if (!st.mounted) {
            const int64_t now = esp_timer_get_time();
            if (now >= st.next_mount_us) {
                const esp_err_t err = sd_mount(&st);
                if (err == ESP_OK) {
                    st.mounted = true;
                    st.ever_mounted = true;
                    ++st.mounts_ok;
                    ESP_LOGI(TAG, "SD mounted at " MOUNT_POINT);
                } else {
                    st.card = NULL;
                    ++st.mounts_failed;
                    st.next_mount_us = now + (int64_t)REMOUNT_INTERVAL_MS * 1000;
                    /* rows stay in the queue (up to STORAGE_ROW_QUEUE_LEN s);
                     * after that the sampler counts drops — unless the
                     * serial mirror drains them (drain_to_serial below) */
                    ESP_LOGE(TAG, "SD mount failed: %s; rows waiting %u",
                             esp_err_to_name(err), storage_rows_waiting());
                }
            }
            if (!st.mounted) {
                /* a row kept after an SD failure stays pending for the SD
                 * (already printed); only queued rows may be drained */
                drain_to_serial(&st);
                maybe_report(&st);
                vTaskDelay(pdMS_TO_TICKS(ROW_WAIT_MS));
                continue;
            }
        }

        if (!have_ev) {
            have_ev = xQueueReceive(s_ev_q, &ev, 0) == pdTRUE;
            if (have_ev) {
                mirror_event(&st, &ev);   /* once; an SD retry does not print again */
            }
        }
        if (have_ev && write_event(&st, &ev)) {
            have_ev = false;
        }
        if (st.mounted) {
            if (!have_row &&
                xQueueReceive(s_row_q, &row, pdMS_TO_TICKS(ROW_WAIT_MS)) == pdTRUE) {
                row_len = format_row(&row, row_text, sizeof(row_text));
                if (row_len != 0) {
                    /* printed once here; an SD retry does not print again */
                    mirror_row(&st, row_text);
                    have_row = true;
                }
            }
            if (have_row && write_row(&st, &row, row_text, row_len)) {
                have_row = false;
            }
        }
        maybe_report(&st);
    }
}

esp_err_t storage_start(TaskHandle_t *out_task)
{
    s_row_q = xQueueCreateStatic(STORAGE_ROW_QUEUE_LEN, sizeof(csv_sample_t),
                                 s_row_q_storage, &s_row_q_struct);
    s_ev_q = xQueueCreateStatic(STORAGE_EVENT_QUEUE_LEN, sizeof(storage_event_t),
                                s_ev_q_storage, &s_ev_q_struct);
    if (s_row_q == NULL || s_ev_q == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const spi_bus_config_t bus = {
        .mosi_io_num = BOARD_PIN_SD_MOSI,
        .miso_io_num = BOARD_PIN_SD_MISO,
        .sclk_io_num = BOARD_PIN_SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,   /* ESP-IDF sd_card example value; > one 512 B sector */
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    /* SD in SPI mode needs DO (MISO) pulled up while the card is not
     * driving it. No board pull-up is documented for J6 (docs/02), so turn
     * on the internal ~45 kΩ one; harmless if the module already has one.
     * [ต้องดู schematic โมดูล microSD] */
    err = gpio_pullup_en(BOARD_PIN_SD_MISO);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "row queue %u x %u B = %u B static; fsync every %d rows; serial mirror %s",
             (unsigned)STORAGE_ROW_QUEUE_LEN, (unsigned)sizeof(csv_sample_t),
             (unsigned)sizeof(s_row_q_storage), FSYNC_EVERY_N_ROWS,
             STORAGE_SERIAL_MIRROR ? "ON (" SERIAL_MIRROR_PREFIX "...)" : "off");

    const BaseType_t ok = xTaskCreate(storage_task, "storage", STORAGE_STACK_BYTES,
                                      NULL, STORAGE_TASK_PRIORITY, out_task);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

bool storage_submit_row(const csv_sample_t *row)
{
    return s_row_q != NULL && xQueueSend(s_row_q, row, 0) == pdTRUE;
}

bool storage_submit_event(const storage_event_t *ev)
{
    return s_ev_q != NULL && xQueueSend(s_ev_q, ev, 0) == pdTRUE;
}

unsigned storage_rows_waiting(void)
{
    return s_row_q != NULL ? (unsigned)uxQueueMessagesWaiting(s_row_q) : 0U;
}
