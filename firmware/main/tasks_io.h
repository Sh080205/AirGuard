#ifndef AIRGUARD_TASKS_IO_H
#define AIRGUARD_TASKS_IO_H

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "ag_fsm.h"

/* FW-6 step 3: what the LEDs/buzzer show is decided in the io task, first
 * true line wins:
 *   CO path (FW-5b co_alert: CO ALARM / CO FAULT)  - nothing can hide it
 *   ag_fsm (components/fsm): ML ALARM > SYS FAULT > ML WATCH > NORMAL
 * MODE held 3 s = 60 s demo of the FSM patterns (CO still wins). The old
 * MODE cycling and "ACK -> NORMAL" demo are gone. */
typedef enum {
    NORMAL,
    WATCH,
    ALARM,
} io_mode_t;

/* FW-4: logger status shown only while the FSM shows NORMAL. WATCH/ALARM/
 * FAULT patterns always win because they carry the air-quality/safety state.
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

/* FW-6: WATCH/ALARM now come from the FSM only, so no other task can force
 * them (they would hide the real state). Kept so main.c's existing
 * io_set_mode(NORMAL) stays valid: NORMAL -> ESP_OK and does nothing,
 * WATCH/ALARM -> ESP_ERR_NOT_SUPPORTED. */
esp_err_t io_set_mode(io_mode_t mode);

/* ส่งคำสั่งจาก task เท่านั้น; คืน ESP_ERR_TIMEOUT เมื่อ queue เต็ม
 * ESP_OK หมายถึงเข้าคิวแล้ว โดย io task จะเป็นผู้เปลี่ยน GPIO */
esp_err_t io_set_indication(io_indication_t indication);

/* FW-6: sensor health for SYS FAULT, published by the sampler (the owner of
 * the I²C bus and UART1). Times are esp_timer_get_time() of the last
 * success, -1 = never:
 *   aht_ok_us  AHT21 measurement read with a good CRC
 *   ens_ok_us  ENS160 answered on I²C (any OPMODE/validity, warm-up and
 *              initial start-up included: those are normal, DS §10.1)
 *   pms_ok_us  PMS7003 frame with a good checksum (also the frames dropped
 *              in the 30 s fan warm-up)
 * monitored: AG_FSM_SENSOR_* bits for the sensors this build reads (e.g.
 * no ENS160 bit in an ENS160_ENABLE=0 build).
 * Last-success times, not counters: if the sampler stops, the times stop
 * too and the FSM shows SYS FAULT with no extra code.
 *
 * Called by the sampler once per cycle (publish_health in tasks_sampler.c).
 * Until the first call the io task monitors no sensor; a sampler that never
 * starts still shows as SYS FAULT ML_STALE through the ML mailbox. */
typedef struct {
    uint8_t monitored;
    int64_t aht_ok_us;
    int64_t ens_ok_us;
    int64_t pms_ok_us;
} io_sensor_health_t;

/* Never blocks (mailbox overwrite). ESP_ERR_INVALID_STATE before io_init(). */
esp_err_t io_publish_sensor_health(const io_sensor_health_t *health);

/* FW-5b: called by the CO task when its snapshot changes what is shown
 * (ALARM/FAULT/reasons). Only wakes the io task, which then re-reads the
 * CO mailbox; same queue and semantics as io_set_mode(). A lost wake-up is
 * caught by the io task's own <= 100 ms wake. */
esp_err_t io_notify_co(void);

#endif
