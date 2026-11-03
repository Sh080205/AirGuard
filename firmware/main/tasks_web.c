#include "tasks_web.h"

#if WEB_ENABLE

#include <inttypes.h>
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ag_fsm.h"
#include "ag_mahal.h"
#include "co_safety.h"
#include "ml_mailbox.h"
#include "task_config.h"
#include "tasks_co.h"
#include "time_sync.h"
#include "web_json.h"
#include "web_mailbox.h"

/* One message per second: the sampler makes one row per second, so a
 * faster push would only repeat it (design §3). */
#define WEB_PERIOD_MS 1000U
/* 60 s summary, like the sampler/storage/co summaries. */
#define WEB_SUMMARY_US (60LL * 1000 * 1000)
/* The page sends nothing the device acts on (read-only); anything it does
 * send is read and dropped. A frame larger than this closes the socket. */
#define WEB_WS_RX_MAX 128U
/* /history.json is written in chunks of about this size (stack-free: the
 * buffer is static and only the httpd task, one request at a time, uses
 * it). */
#define WEB_HIST_CHUNK 1024U
/* Lock waits: the web task gives up on one history entry rather than miss
 * its period; the history request waits longer (a person is waiting). */
#define WEB_HIST_LOCK_TASK_MS 100U
#define WEB_HIST_LOCK_HTTP_MS 1000U

/* The page, embedded by main/CMakeLists.txt (EMBED_TXTFILES adds a NUL,
 * which is not sent). [ขึ้นกับเวอร์ชัน ESP-IDF] symbol names from the file
 * name, ESP-IDF build system "Embedding Binary Data". */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static const char *TAG = "web";

/* Set in web_start() before the web task exists and never changed: the
 * web task and the httpd task read it without a lock. */
static httpd_handle_t s_server;

/* 10-minute history (design §2.4). Writer: web task, once per second.
 * Reader: httpd task (/history.json). Guarded by a FreeRTOS mutex, not a
 * critical section: the reader copies ~12 KB, which must not run with
 * interrupts off, and both sides are tasks (no ISR), so a mutex with
 * priority inheritance is the right tool. */
static web_hist_t s_hist;
static SemaphoreHandle_t s_hist_lock;

/* Counters shared by the web task (reads them in the summary) and the
 * httpd task (updates them while sending). Single 32-bit values with no
 * other field that must change together, so C11 atomics are enough: they
 * give atomicity and ordering across the two cores, which `volatile` does
 * not (CLAUDE.md §2); a mutex would add a blocking point for nothing. */
static atomic_uint_fast32_t s_frames_sent;
static atomic_uint_fast32_t s_send_err;
static atomic_uint_fast32_t s_ws_clients;      /* at the last send */
static atomic_uint_fast32_t s_ws_opened;
static atomic_uint_fast32_t s_history_served;
static atomic_uint_fast32_t s_httpd_hwm = UINT32_MAX;   /* min seen */

/* Owned by the web task only. */
static uint32_t s_seq;
static uint32_t s_queue_fail;
static uint32_t s_hist_skipped;
static uint32_t s_json_fail;
static char s_json[WEB_JSON_LIVE_CAP];

typedef struct {
    size_t len;
    char text[];
} ws_msg_t;

/* httpd task only: record its stack high-water mark (bytes) for the
 * summary. compare_exchange keeps the minimum without a lock. */
static void note_httpd_stack(void)
{
    const uint32_t now = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    uint_fast32_t seen = atomic_load(&s_httpd_hwm);
    while (now < seen && !atomic_compare_exchange_weak(&s_httpd_hwm, &seen, now)) {
    }
}

/* ---------------------------------------------------------- httpd side --
 * Everything below runs in the httpd task. */

static esp_err_t index_get(httpd_req_t *req)
{
    esp_err_t err = httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (err == ESP_OK) {
        const ssize_t len = (ssize_t)(index_html_end - index_html_start) - 1;
        err = httpd_resp_send(req, index_html_start, len);
    }
    note_httpd_stack();
    return err;
}

static esp_err_t history_get(httpd_req_t *req)
{
    /* Copy under the lock, format after it, so a slow phone never holds
     * the lock the web task needs every second. PSRAM first (8 MB, mostly
     * free), internal RAM as fallback. */
    web_hist_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (copy == NULL) {
        copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_8BIT);
    }
    if (copy == NULL) {
        ESP_LOGW(TAG, "/history.json: no memory for the copy");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    }
    if (xSemaphoreTake(s_hist_lock, pdMS_TO_TICKS(WEB_HIST_LOCK_HTTP_MS)) != pdTRUE) {
        free(copy);
        ESP_LOGW(TAG, "/history.json: history busy");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
    }
    memcpy(copy, &s_hist, sizeof(*copy));
    (void)xSemaphoreGive(s_hist_lock);   /* we hold it: cannot fail */

    static char chunk[WEB_HIST_CHUNK];
    esp_err_t err = httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        err = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    size_t used = 0;
    chunk[used++] = '[';
    const unsigned n = web_hist_count(copy);
    for (unsigned i = 0; i < n && err == ESP_OK; ++i) {
        char entry[WEB_JSON_HIST_CAP];
        const size_t len = web_json_hist_entry(web_hist_at(copy, i), entry, sizeof(entry));
        if (len == 0) {
            continue;   /* cannot happen with WEB_JSON_HIST_CAP; skip, never half */
        }
        if (used + len + 2 > sizeof(chunk)) {
            err = httpd_resp_send_chunk(req, chunk, (ssize_t)used);
            used = 0;
        }
        if (i > 0) {
            chunk[used++] = ',';
        }
        memcpy(chunk + used, entry, len);
        used += len;
    }
    if (err == ESP_OK) {
        chunk[used++] = ']';
        err = httpd_resp_send_chunk(req, chunk, (ssize_t)used);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);   /* end of chunked body */
    }
    free(copy);
    if (err == ESP_OK) {
        atomic_fetch_add(&s_history_served, 1);
    } else {
        ESP_LOGW(TAG, "/history.json: %s", esp_err_to_name(err));
    }
    note_httpd_stack();
    return err;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* handshake done; the client now gets the 1 Hz messages */
        atomic_fetch_add(&s_ws_opened, 1);
        return ESP_OK;
    }
    /* Read-only page: read whatever the client sent and drop it. First
     * call with max_len 0 only fills ws.len (ESP-IDF ws example). */
    uint8_t buf[WEB_WS_RX_MAX];
    httpd_ws_frame_t ws;
    memset(&ws, 0, sizeof(ws));
    esp_err_t err = httpd_ws_recv_frame(req, &ws, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (ws.len > sizeof(buf)) {
        return ESP_ERR_INVALID_SIZE;   /* the server closes this socket */
    }
    if (ws.len > 0) {
        ws.payload = buf;
        err = httpd_ws_recv_frame(req, &ws, ws.len);
    }
    note_httpd_stack();
    return err;
}

/* Queued with httpd_queue_work by the web task. The ESP-IDF docs say
 * httpd_ws_send_frame_async "should rarely be called directly, with an
 * exception of asynchronous send using httpd_queue_work", so every send
 * happens here, in the httpd task. */
static void ws_send_all(void *arg)
{
    ws_msg_t *m = arg;
    int fds[WEB_MAX_SOCKETS];
    size_t count = WEB_MAX_SOCKETS;   /* in: array size · out: sessions */
    uint32_t clients = 0;
    if (httpd_get_client_list(s_server, &count, fds) == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (httpd_ws_get_fd_info(s_server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) {
                continue;   /* plain HTTP session (page or history load) */
            }
            ++clients;
            httpd_ws_frame_t ws = {
                .final = true,
                .type = HTTPD_WS_TYPE_TEXT,
                .payload = (uint8_t *)m->text,
                .len = m->len,
            };
            if (httpd_ws_send_frame_async(s_server, fds[i], &ws) == ESP_OK) {
                atomic_fetch_add(&s_frames_sent, 1);
            } else {
                atomic_fetch_add(&s_send_err, 1);
            }
        }
    } else {
        atomic_fetch_add(&s_send_err, 1);
    }
    atomic_store(&s_ws_clients, clients);
    free(m);
    note_httpd_stack();
}

/* ------------------------------------------------------------ web task -- */

static void build_snapshot(web_snapshot_t *s, int64_t now_us)
{
    memset(s, 0, sizeof(*s));
    s->seq = ++s_seq;

    web_row_t row;
    if (web_row_peek(&row)) {
        s->row = row.row;
        s->time_valid = row.time_valid;
        s->uptime_s = row.at_us / 1000000;
    } else {
        s->uptime_s = now_us / 1000000;   /* sampler not running: values null */
    }

    web_show_t show;
    if (web_show_peek(&show)) {
        s->shown = ag_fsm_show_name(show.shown);
        s->demo = show.demo;
        s->muted = show.muted;
        s->fault_reasons = show.fault_reasons;
    }

    ag_ml_result_t ml;
    if (ml_mailbox_peek(&ml)) {
        s->ml.present = true;
        s->ml.status = ag_ml_status_name(ml.status);
        s->ml.d2 = ml.d2;
        s->ml.threshold_d2 = ag_mahal_threshold_d2();
        s->ml.streak = ml.streak;
        s->ml.model = ag_mahal_model_version();
    }

    co_status_t co;
    if (co_peek_status(&co)) {
        s->co.present = true;
        s->co.alarm = co.alarm;
        s->co.fault = co.fault;
        s->co.warmup = co.warmup;
        s->co.stale = co_status_stale(&co, now_us);
        /* warm-up samples are not in the means (co_safety.h), so a mean is
         * shown only once a frame arrived after warm-up */
        s->co.has_mean_1h = co.have_frame && !co.warmup;
        s->co.mean_1h_x10 = co.mean_1h_x10;
    }
}

static void log_summary(void)
{
    char url[40] = "no IP yet";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0) {
        snprintf(url, sizeof(url), "http://" IPSTR "/", IP2STR(&ip.ip));
    }
    const uint32_t httpd_hwm = (uint32_t)atomic_load(&s_httpd_hwm);
    ESP_LOGI(TAG, "%s | ws clients=%" PRIu32 " opened=%" PRIu32 " frames sent=%" PRIu32
             " err=%" PRIu32 " | queue_fail=%" PRIu32 " json_fail=%" PRIu32
             " hist_skipped=%" PRIu32 " history served=%" PRIu32 " | hist %u/%u"
             " | stack HWM web=%u httpd=%s%" PRIu32 " B",
             url, (uint32_t)atomic_load(&s_ws_clients), (uint32_t)atomic_load(&s_ws_opened),
             (uint32_t)atomic_load(&s_frames_sent), (uint32_t)atomic_load(&s_send_err),
             s_queue_fail, s_json_fail, s_hist_skipped,
             (uint32_t)atomic_load(&s_history_served), web_hist_count(&s_hist),
             (unsigned)WEB_HIST_LEN, (unsigned)uxTaskGetStackHighWaterMark(NULL),
             httpd_hwm == UINT32_MAX ? "n/a " : "",
             (uint32_t)(httpd_hwm == UINT32_MAX ? 0U : httpd_hwm));
}

static void web_task(void *arg)
{
    (void)arg;
    int64_t next_summary = esp_timer_get_time() + WEB_SUMMARY_US;
    TickType_t last_wake = xTaskGetTickCount();
    for (;;) {
        const int64_t now_us = esp_timer_get_time();
        web_snapshot_t snap;
        build_snapshot(&snap, now_us);

        /* the history is read by the page on load; it is a lower priority
         * than the live message, so a busy lock only skips one entry */
        if (xSemaphoreTake(s_hist_lock, pdMS_TO_TICKS(WEB_HIST_LOCK_TASK_MS)) == pdTRUE) {
            web_hist_push(&s_hist, &snap);
            (void)xSemaphoreGive(s_hist_lock);
        } else {
            ++s_hist_skipped;
        }

        const size_t len = web_json_live(&snap, s_json, sizeof(s_json));
        if (len == 0) {
            ++s_json_fail;   /* only possible with a string > WEB_JSON_STR_MAX */
        } else {
            ws_msg_t *m = malloc(sizeof(*m) + len + 1U);
            if (m == NULL) {
                ++s_queue_fail;
            } else {
                m->len = len;
                memcpy(m->text, s_json, len + 1U);
                if (httpd_queue_work(s_server, ws_send_all, m) != ESP_OK) {
                    free(m);   /* httpd control queue full: drop this second */
                    ++s_queue_fail;
                }
            }
        }

        if (now_us >= next_summary) {
            next_summary = now_us + WEB_SUMMARY_US;
            log_summary();
        }
        (void)xTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WEB_PERIOD_MS));
    }
}

esp_err_t web_start(void)
{
    if (!time_sync_wifi_started()) {
        ESP_LOGW(TAG, "no Wi-Fi (main/secrets.h missing or Wi-Fi start failed): dashboard off");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_hist_lock = xSemaphoreCreateMutex();
    if (s_hist_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    web_hist_init(&s_hist);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_priority = WEB_HTTPD_PRIORITY;
    cfg.stack_size = WEB_HTTPD_STACK_BYTES;
    cfg.core_id = tskNO_AFFINITY;
    cfg.max_open_sockets = WEB_MAX_SOCKETS;
    cfg.lru_purge_enable = true;   /* a new phone pushes out a stale socket */
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_get },
        { .uri = "/history.json", .method = HTTP_GET, .handler = history_get },
        { .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        err = httpd_register_uri_handler(s_server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s: %s", uris[i].uri, esp_err_to_name(err));
            (void)httpd_stop(s_server);   /* best effort; the error is returned */
            s_server = NULL;
            return err;
        }
    }

    if (xTaskCreate(web_task, "web", WEB_STACK_BYTES, NULL, WEB_TASK_PRIORITY, NULL) != pdPASS) {
        (void)httpd_stop(s_server);
        s_server = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "dashboard on port %u · page %u B · history %u x %u B = %u B · "
                  "IP is in the 'Wi-Fi got IP' line and the 60 s web summary",
             (unsigned)cfg.server_port,
             (unsigned)(index_html_end - index_html_start - 1),
             (unsigned)WEB_HIST_LEN, (unsigned)sizeof(web_hist_entry_t),
             (unsigned)sizeof(s_hist.e));
    return ESP_OK;
}

#else /* !WEB_ENABLE */

#include "esp_log.h"

esp_err_t web_start(void)
{
    ESP_LOGW("web", "built with WEB_ENABLE=0: no dashboard");
    return ESP_OK;
}

#endif
