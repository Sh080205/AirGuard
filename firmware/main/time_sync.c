#include "time_sync.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

/* AIRGUARD_HAVE_SECRETS comes from main/CMakeLists.txt (file glob with
 * CONFIGURE_DEPENDS), not __has_include: ninja does not track a header
 * that did not exist, so adding secrets.h later would not rebuild this. */
#ifdef AIRGUARD_HAVE_SECRETS
#include "secrets.h"
#define HAVE_WIFI_SECRETS 1
#else
#define HAVE_WIFI_SECRETS 0
#endif

/* POSIX TZ: "ICT-7" = name ICT, 7 h east of UTC (POSIX sign is inverted).
 * Thailand has no DST. CLAUDE.md §7 requires +07:00 in every timestamp;
 * csv_row prints the fixed "+07:00", so localtime_r must give UTC+7.
 * time_sync_start() checks this at boot with epoch 0 -> 07:00. */
#define TZ_THAILAND "ICT-7"

/* Retry period after a Wi-Fi disconnect / failed connect. 10 s keeps the
 * radio from hammering a hotspot that is off, yet reconnects soon after the
 * phone comes back. ค่าคาดหมาย ต้องยืนยันด้วยการวัด */
#define TIME_SYNC_RECONNECT_MS 10000U

/* SNTP server: public NTP pool. Phone hotspots usually do not hand out an NTP
 * server over DHCP, so server_from_dhcp stays false. */
#define SNTP_SERVER "pool.ntp.org"

static const char *TAG = "time";

/* ตัวแปรร่วมระหว่าง tcpip task (SNTP callback), console task (settime)
 * และ sampler task (อ่าน) → atomic_bool:
 * - เป็นค่า bool เดียว ไม่มีข้อมูลอื่นที่ต้องเปลี่ยนพร้อมกัน (ตัวเวลาเองอยู่ใน
 *   system clock ซึ่ง newlib ป้องกันด้วย lock ของมันเอง)
 * - เปลี่ยนทางเดียว false→true จึงไม่มี read-modify-write ที่ต้อง critical section
 * - atomic ให้ทั้ง atomicity และ memory ordering ข้ามสองคอร์ ซึ่ง volatile ไม่ให้
 * queue ไม่เหมาะเพราะนี่คือ "สถานะ" ที่อ่านซ้ำได้ ไม่ใช่ event ที่ถูกกินครั้งเดียว */
static atomic_bool s_time_valid;

#if HAVE_WIFI_SECRETS
static esp_timer_handle_t s_reconnect_timer;

static void on_sntp_sync(struct timeval *tv)
{
    atomic_store(&s_time_valid, true);
    struct tm local;
    if (localtime_r(&tv->tv_sec, &local) == NULL) {
        ESP_LOGI(TAG, "SNTP sync (localtime_r failed for the log line)");
        return;
    }
    ESP_LOGI(TAG, "SNTP sync: %04d-%02d-%02dT%02d:%02d:%02d+07:00",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min, local.tm_sec);
}

static void reconnect_cb(void *arg)
{
    (void)arg;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    }
}

static void schedule_reconnect(void)
{
    if (esp_timer_is_active(s_reconnect_timer)) {
        return;
    }
    const esp_err_t err = esp_timer_start_once(s_reconnect_timer,
                                               (uint64_t)TIME_SYNC_RECONNECT_MS * 1000U);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconnect timer: %s", esp_err_to_name(err));
    }
}

/* Runs in the default event loop task (sys_evt): keep it short, no waits. */
static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
            schedule_reconnect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d); retry in %u s",
                 ev != NULL ? (int)ev->reason : -1,
                 (unsigned)(TIME_SYNC_RECONNECT_MS / 1000U));
        schedule_reconnect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev != NULL) {
            ESP_LOGI(TAG, "Wi-Fi got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        }
        /* start (first time) or restart SNTP on every new IP
         * [ขึ้นกับเวอร์ชัน ESP-IDF] esp_netif_sntp_start() semantics in v5.5 */
        const esp_err_t err = esp_netif_sntp_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_sntp_start: %s", esp_err_to_name(err));
        }
    }
}

/* Every step is checked; the first failure stops Wi-Fi setup and returns the
 * error, the caller logs it and the logger keeps running (settime only). */
static esp_err_t wifi_sntp_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already created is fine */
        return err;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_cb,
        .name = "wifi_retry",
    };
    err = esp_timer_create(&timer_args, &s_reconnect_timer);
    if (err != ESP_OK) {
        return err;
    }

    /* SNTP is configured now but started on IP_EVENT_STA_GOT_IP.
     * [ขึ้นกับเวอร์ชัน ESP-IDF] esp_netif_sntp API since v5.1 */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER);
    sntp_cfg.start = false;
    sntp_cfg.wait_for_sync = false;
    sntp_cfg.sync_cb = on_sntp_sync;
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        return err;
    }

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t cfg = { 0 };
    _Static_assert(sizeof(AIRGUARD_WIFI_SSID) <= sizeof(cfg.sta.ssid),
                   "SSID longer than 31 characters (copied with its NUL)");
    _Static_assert(sizeof(AIRGUARD_WIFI_PASSWORD) <= sizeof(cfg.sta.password),
                   "Wi-Fi password longer than 63 characters");
    memcpy(cfg.sta.ssid, AIRGUARD_WIFI_SSID, sizeof(AIRGUARD_WIFI_SSID));
    memcpy(cfg.sta.password, AIRGUARD_WIFI_PASSWORD, sizeof(AIRGUARD_WIFI_PASSWORD));
    /* empty password = open hotspot; otherwise refuse anything weaker than WPA2 */
    cfg.sta.threshold.authmode = sizeof(AIRGUARD_WIFI_PASSWORD) > 1 ?
                                 WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    return esp_wifi_start();   /* → WIFI_EVENT_STA_START → connect */
}
#endif /* HAVE_WIFI_SECRETS */

void time_sync_start(void)
{
    if (setenv("TZ", TZ_THAILAND, 1) != 0) {
        ESP_LOGE(TAG, "setenv TZ failed");
    }
    tzset();
    const time_t zero = 0;
    struct tm check;
    if (localtime_r(&zero, &check) == NULL || check.tm_hour != 7 || check.tm_min != 0) {
        ESP_LOGE(TAG, "TZ check failed: epoch 0 is not 07:00 local; timestamps would be wrong");
    }

#if HAVE_WIFI_SECRETS
    const esp_err_t err = wifi_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi/SNTP start failed: %s; use console 'settime'",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA started, SSID \"%s\"; SNTP server %s",
                 AIRGUARD_WIFI_SSID, SNTP_SERVER);
    }
#else
    ESP_LOGW(TAG, "No main/secrets.h: Wi-Fi/SNTP disabled. "
                  "Set time with: settime 2026-10-06T09:00:00+07:00");
#endif
}

bool time_sync_is_valid(void)
{
    return atomic_load(&s_time_valid);
}

esp_err_t time_sync_set_manual(int64_t epoch)
{
    const struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }
    atomic_store(&s_time_valid, true);
    return ESP_OK;
}
