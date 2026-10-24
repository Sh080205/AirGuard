#ifndef AIRGUARD_TASKS_IO_H
#define AIRGUARD_TASKS_IO_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    NORMAL,
    WATCH,
    ALARM,
} io_mode_t;

/* FW-4: logger status shown only while mode == NORMAL. WATCH/ALARM patterns
 * always win because they carry the air-quality/safety state.
 * Patterns on LED_G (period / on-time), chosen so no two share both rhythm
 * and duty with NORMAL (steady), WATCH (1 s, 50 %) or ALARM (0.2 s, 50 %):
 *   IO_IND_RUNNING    steady on                  = plain NORMAL
 *   IO_IND_WAIT_TIME  2 s period, 100 ms on (5 %)  "short blip": no clock yet,
 *                     nothing is being written
 *   IO_IND_MARKED     2 s period, 1900 ms on (95 %) "brief dropout": logging
 *                     with label=marked
 * The 2 s period is twice WATCH and ten times ALARM, and the duty is far from
 * 50 %, so the difference is a rhythm, not a colour (LED1 colour issue,
 * CLAUDE.md §5). Timing is ค่าคาดหมาย ต้องยืนยันด้วยการวัด. */
typedef enum {
    IO_IND_RUNNING,
    IO_IND_WAIT_TIME,
    IO_IND_MARKED,
} io_indication_t;

/* One confirmed (debounced) press of BTN_MARK, sent by the io task. */
typedef struct {
    int64_t pressed_us;  /* esp_timer_get_time() at confirmation */
} io_mark_event_t;

/* เรียกครั้งเดียวจาก app_main; init ล้มเหลวจะหยุดผ่าน ESP_ERROR_CHECK
 * mark_queue: queue of io_mark_event_t owned by the caller (logger), or NULL.
 * The io task sends with timeout 0 and logs when the queue is full. */
void io_init(QueueHandle_t mark_queue);

/* ส่งคำสั่งจาก task เท่านั้น; คืน ESP_ERR_TIMEOUT เมื่อ queue เต็ม
 * ESP_OK หมายถึงเข้าคิวแล้ว โดย io task จะเป็นผู้เปลี่ยน GPIO */
esp_err_t io_set_mode(io_mode_t mode);

/* Same queue and semantics as io_set_mode(). */
esp_err_t io_set_indication(io_indication_t indication);

/* FW-5b: called by the CO task when its snapshot changes what is shown
 * (ALARM/FAULT/reasons). Only wakes the io task, which then re-reads the
 * CO mailbox; same queue and semantics as io_set_mode(). A lost wake-up is
 * caught by the io task's own <= 100 ms wake. */
esp_err_t io_notify_co(void);

#endif
