#include "tasks_io.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ag_fsm.h"
#include "board_pins.h"
#include "co_alert.h"
#include "io_compose.h"
#include "ml_mailbox.h"
#include "tasks_co.h"

/* ค่าคาดหมาย ต้องยืนยันด้วยการวัด: debounce ชั้นที่สองหลัง RC บนบอร์ด */
#define DEBOUNCE_MS              50U
/* ag_fsm.h: state changes driven by timeouts are not in next_change_us,
 * so the FSM must be stepped at least every 100 ms */
#define IO_MAX_WAIT_MS          100U
#define STACK_REPORT_MS      10000U
#define IO_QUEUE_LENGTH         16U

/* ESP-IDF รับ stack เป็น bytes
 * วัดจริง 14 ต.ค. (ก่อน FW-5b/FW-6): HWM เหลือ 1968–1996 B จาก 4096
 *   ⇒ ใช้จริงราว 2.1 KB (state/locals + GPIO/queue/ESP_LOG)
 * FW-5b เพิ่ม co_alert_t + co_status_t + co_alert_out_t ≈ 100 B
 * FW-6 เพิ่ม ag_fsm_t 128 B + ag_fsm_in_t 64 + out_t 32 ×2 + hold 16
 *   (sizeof บน gcc x86-64 · Xtensa จัด int64 แบบ 8 ไบต์เหมือนกัน)
 *   + ใน ag_fsm_step: waves_t 4 × 64 = 256 B + locals ≈ 100 B
 *   + สตริงสาเหตุ FAULT 48 B ⇒ รวม FW-5b/FW-6 ≈ 0.8 KB
 * 4096 จะเหลือราว 1.2 KB = 29 % ใกล้เป้า 25 % (docs/tasks/README.md) เกินไป
 * → 5120 B (เหลือคาดหมาย ≈ 2.2 KB = 43 %) · ค่าคาดหมาย ต้องยืนยันด้วย HWM
 * ที่ log ทุก 10 s บนบอร์ด */
#define IO_STACK_BYTES        5120U
#define IO_TASK_PRIORITY         5U

/* queue เดียวส่งเลขขาบวกจาก ISR และคำสั่งลบจาก io_set_indication()
 * ทำให้ io task เป็นเจ้าของ FSM, debounce, counter และ output เพียงผู้เดียว
 * จึงไม่ต้องแชร์ตัวแปรสถานะแบบ volatile ระหว่าง ISR/task หรือระหว่าง task */
enum {
    COMMAND_IND_RUNNING   = -4,
    COMMAND_IND_WAIT_TIME = -5,
    COMMAND_IND_MARKED    = -6,
    /* FW-5b: the CO task published a snapshot that changes what is shown;
     * the snapshot itself is read from the CO mailbox, not sent here */
    COMMAND_CO_CHANGED    = -7,
};

typedef struct {
    int pin;
    bool pending;
    TickType_t confirm_at;
    uint32_t count;
} button_state_t;

typedef struct {
    io_indication_t indication;
    /* FW-5b: CO path display. co_status is the latest snapshot peeked from
     * the CO mailbox (a copy: the io task never shares it); co_alert decides
     * LED_R/buzzer and overrides everything below while active. */
    co_alert_t co_alert;
    co_status_t co_status;
    bool co_have;
    /* FW-6: everything below CO. Owned by this task only. */
    ag_fsm_t fsm;
    ag_fsm_out_t fsm_prev;     /* last step, for transition logs */
    ag_fsm_hold_t mode_hold;
} io_state_t;

static const char *TAG = "io";
/* สร้างก่อนเปิด ISR/สร้าง task และไม่เปลี่ยน handle ตลอดอายุการทำงาน */
static QueueHandle_t s_event_queue;
/* FW-4: ตั้งใน io_init ก่อนสร้าง io task และไม่เปลี่ยนอีก → task อ่านได้
 * โดยไม่ต้อง lock (xTaskCreate เป็น happens-before ของ task ใหม่) */
static QueueHandle_t s_mark_queue;

/* FW-6: sensor health mailbox (sampler → io). Queue of length 1 written with
 * xQueueOverwrite and read with xQueuePeek, the same pattern as the CO
 * snapshot (tasks_co.h) and FW-7's ML result:
 * - the sampler never blocks on the io task (overwrite, timeout 0);
 * - the io task gets a copy of the whole struct taken under the queue's
 *   critical section, so it never mixes two publishes. `volatile` alone
 *   would not give that for a 28-byte struct read on the other core
 *   (CLAUDE.md §2); a hand-written portMUX getter would work too, but the
 *   queue keeps all three mailboxes alike.
 * Static storage: creation cannot fail for lack of heap. The handle is
 * written in io_init() before the io task and the sampler exist. */
static QueueHandle_t s_health_mailbox;
static StaticQueue_t s_health_mailbox_struct;
static uint8_t s_health_mailbox_storage[sizeof(io_sensor_health_t)];

/* deadline ถูกตั้งล่วงหน้าเพียง DEBOUNCE_MS: ผลลบ unsigned ที่มากกว่าช่วงนี้
 * หมายถึงเลย deadline แล้ว; รองรับ tick wrap โดยไม่เทียบ now >= confirm_at */
static TickType_t confirmation_wait(const button_state_t *button, TickType_t now)
{
    const TickType_t remaining = (TickType_t)(button->confirm_at - now);
    return remaining <= pdMS_TO_TICKS(DEBOUNCE_MS) ? remaining : 0;
}

/* ticks from now_us to deadline_us (rounded up), capped at bound;
 * INT64_MAX = no deadline */
static TickType_t ticks_until_us(int64_t deadline_us, int64_t now_us, TickType_t bound)
{
    if (deadline_us == INT64_MAX) {
        return bound;
    }
    const int64_t until_us = deadline_us - now_us;
    if (until_us <= 0) {
        return 0;
    }
    if (until_us >= (int64_t)IO_MAX_WAIT_MS * 1000) {
        return bound;
    }
    const TickType_t t = pdMS_TO_TICKS((uint32_t)((until_us + 999) / 1000));
    return t < bound ? t : bound;
}

/* FW-6 step 3: the ONE place where the io task reads the ML result
 * (FW-7 ml_mailbox, written by the sampler once per 1 Hz cycle).
 * false = mailbox not created: the FSM then never shows ML WATCH/ALARM and
 * raises no ML fault (ag_fsm.h ml_enabled). An unknown status maps to
 * MISSING so it can never raise WATCH/ALARM. */
static bool ml_peek(ag_fsm_in_t *in)
{
    ag_ml_result_t r;
    if (!ml_mailbox_peek(&r)) {
        return false;
    }
    switch (r.status) {
    case AG_ML_MODEL_FAULT: in->ml_status = AG_FSM_ML_MODEL_FAULT; break;
    case AG_ML_BELOW:       in->ml_status = AG_FSM_ML_BELOW; break;
    case AG_ML_ABOVE:       in->ml_status = AG_FSM_ML_ABOVE; break;
    case AG_ML_PERSISTENT:  in->ml_status = AG_FSM_ML_PERSISTENT; break;
    case AG_ML_MISSING:
    default:                in->ml_status = AG_FSM_ML_MISSING; break;
    }
    in->ml_streak = r.streak;
    in->ml_at_us = r.at_us;
    return true;
}

static ag_fsm_ind_t fsm_indication(io_indication_t indication)
{
    switch (indication) {
    case IO_IND_WAIT_TIME: return AG_FSM_IND_WAIT_TIME;
    case IO_IND_MARKED: return AG_FSM_IND_MARKED;
    case IO_IND_RUNNING:
    default: return AG_FSM_IND_RUNNING;
    }
}

/* Copies of every FSM input, taken once per loop. */
static ag_fsm_in_t fsm_input(const io_state_t *state, bool co_active)
{
    ag_fsm_in_t in = {
        .co_active = co_active,
        .ml_status = AG_FSM_ML_NONE,
        .monitored = 0,
        .aht_ok_us = -1, .ens_ok_us = -1, .pms_ok_us = -1,
        .indication = fsm_indication(state->indication),
    };
    in.ml_enabled = ml_peek(&in);
    io_sensor_health_t health;
    /* nothing published yet (sampler not started / not wired): monitor no
     * sensor rather than call every sensor broken */
    if (s_health_mailbox != NULL && xQueuePeek(s_health_mailbox, &health, 0) == pdTRUE) {
        in.monitored = health.monitored & AG_FSM_SENSOR_ALL;
        in.aht_ok_us = health.aht_ok_us;
        in.ens_ok_us = health.ens_ok_us;
        in.pms_ok_us = health.pms_ok_us;
    }
    return in;
}

static void fault_reasons_text(uint8_t reasons, char *buf, size_t len)
{
    static const struct { uint8_t bit; const char *name; } NAMES[] = {
        { AG_FSM_FAULT_AHT21, "AHT21" },
        { AG_FSM_FAULT_ENS160, "ENS160" },
        { AG_FSM_FAULT_PMS7003, "PMS7003" },
        { AG_FSM_FAULT_ML_MODEL, "ML_MODEL" },
        { AG_FSM_FAULT_ML_STALE, "ML_STALE" },
    };
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]) && used < len; ++i) {
        if ((reasons & NAMES[i].bit) != 0U) {
            const int n = snprintf(buf + used, len - used, "%s%s", used ? "|" : "", NAMES[i].name);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
        }
    }
    if (used == 0) {
        snprintf(buf, len, "none");
    }
}

static const char *level_name(ag_fsm_level_t level)
{
    return level == AG_FSM_LEVEL_ALARM ? "ALARM" : level == AG_FSM_LEVEL_WATCH ? "WATCH" : "none";
}

/* One log line per transition, never per loop. */
static void log_fsm(io_state_t *state, const ag_fsm_out_t *o, bool first)
{
    const ag_fsm_out_t *p = &state->fsm_prev;
    if (o->changed) {
        ESP_LOGI(TAG, "Shown: %s%s", ag_fsm_show_name(o->shown), o->demo ? " (demo)" : "");
    }
    if (!first && o->ml_level != p->ml_level) {
        ESP_LOGW(TAG, "ML level: %s -> %s", level_name(p->ml_level), level_name(o->ml_level));
    }
    if (first ? o->fault_reasons != 0U || o->sys_fault :
        o->sys_fault != p->sys_fault || o->fault_reasons != p->fault_reasons) {
        char reasons[48];
        fault_reasons_text(o->fault_reasons, reasons, sizeof(reasons));
        if (o->sys_fault) {
            ESP_LOGE(TAG, "SYS FAULT%s: reasons=%s", (first || !p->sys_fault) ? " enter" : "", reasons);
        } else {
            ESP_LOGI(TAG, "SYS FAULT exit (reasons=%s)", reasons);
        }
    }
    state->fsm_prev = *o;
}

/* MARK → logger: queue (ไม่ใช่ volatile flag) เพราะทุกการกดต้องนับครบ
 * การกด 2 ครั้งในรอบ 1 s เดียวกันต้องถึง logger เป็น 2 event (เปิด+ปิด)
 * flag ตัวเดียวจะรวมเป็นครั้งเดียว และ queue ของ FreeRTOS ให้ memory ordering
 * ข้ามคอร์มาให้แล้ว · timeout 0: io task ต้องไม่ถูก logger ที่ช้าบล็อก */
static void post_mark(void)
{
    if (s_mark_queue == NULL) {
        return;
    }
    const io_mark_event_t ev = { .pressed_us = esp_timer_get_time() };
    if (xQueueSend(s_mark_queue, &ev, 0) != pdTRUE) {
        ESP_LOGW(TAG, "MARK queue full; press dropped");
    }
}

static esp_err_t write_outputs(uint32_t outputs)
{
    const int pins[] = { BOARD_PIN_LED_G, BOARD_PIN_LED_Y, BOARD_PIN_LED_R, BOARD_PIN_BUZZ };
    const uint32_t bits[] = { IO_OUT_GREEN, IO_OUT_YELLOW, IO_OUT_RED, IO_OUT_BUZZER };
    esp_err_t first_error = ESP_OK;
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); ++i) {
        const esp_err_t err = gpio_set_level(pins[i], (outputs & bits[i]) != 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "gpio_set_level pin %d: %s", pins[i], esp_err_to_name(err));
            if (first_error == ESP_OK) {
                first_error = err;
            }
        }
    }
    return first_error;
}

static void change_indication(io_state_t *state, io_indication_t indication)
{
    if (state->indication == indication) {
        return;
    }
    /* the FSM restarts the LED_G pattern phase when it sees the change */
    state->indication = indication;
    const char *name = indication == IO_IND_WAIT_TIME ? "WAIT_TIME" :
                       indication == IO_IND_MARKED ? "MARKED" : "RUNNING";
    ESP_LOGI(TAG, "Indication: %s", name);
}

static void handle_event(int event, io_state_t *state, button_state_t *buttons,
                         size_t button_count, TickType_t now)
{
    switch (event) {
    case COMMAND_IND_RUNNING: change_indication(state, IO_IND_RUNNING); return;
    case COMMAND_IND_WAIT_TIME: change_indication(state, IO_IND_WAIT_TIME); return;
    case COMMAND_IND_MARKED: change_indication(state, IO_IND_MARKED); return;
    case COMMAND_CO_CHANGED: return;   /* io_task re-reads the mailbox every loop */
    default: break;
    }

    for (size_t i = 0; i < button_count; ++i) {
        button_state_t *button = &buttons[i];
        if (event != button->pin) {
            continue;
        }
        /* อ่านทันทีหลัง edge ยังเจอ LOW จาก RC/contact เด้งตอนปล่อยปุ่มได้
         * จึงรอยืนยันหลัง DEBOUNCE_MS; edge ซ้ำขณะรอไม่เลื่อน deadline */
        if (!button->pending) {
            button->pending = true;
            button->confirm_at = now + pdMS_TO_TICKS(DEBOUNCE_MS);
        }
        return;
    }
}

static void mode_long_press(io_state_t *state, int64_t now_us)
{
    const bool was_demo = state->fsm_prev.demo;
    if (ag_fsm_mode_long_press(&state->fsm, now_us)) {
        ESP_LOGW(TAG, "MODE held 3 s: demo %s", was_demo ? "ended" : "started (60 s, CO still wins)");
    } else {
        ESP_LOGW(TAG, "MODE held 3 s: demo refused while CO is shown");
    }
}

static void confirm_buttons(io_state_t *state, button_state_t *buttons,
                            size_t button_count, TickType_t now, int64_t now_us)
{
    for (size_t i = 0; i < button_count; ++i) {
        button_state_t *button = &buttons[i];
        if (!button->pending || confirmation_wait(button, now) != 0) {
            continue;
        }
        button->pending = false;
        if (gpio_get_level(button->pin) != 0) {
            continue;
        }
        ++button->count;
        if (button->pin == BOARD_PIN_BTN_MARK) {
            ESP_LOGI(TAG, "MARK #%" PRIu32, button->count);
            post_mark();
        } else if (button->pin == BOARD_PIN_BTN_MODE) {
            /* FW-6 Q5: a short press does nothing (safe to bump while
             * logging); holding 3 s toggles the demo, see poll_mode_hold */
            ESP_LOGI(TAG, "MODE #%" PRIu32 " (hold 3 s for demo)", button->count);
            if (ag_fsm_hold_update(&state->mode_hold, true, now_us)) {
                mode_long_press(state, now_us);
            }
        } else if (button->pin == BOARD_PIN_BTN_ACK) {
            ESP_LOGI(TAG, "ACK #%" PRIu32, button->count);
            /* CO first (FW-5b): ACK only mutes the CO buzzer/chirp and never
             * clears a CO ALARM (co_alert.h). Only when CO took nothing does
             * the FSM get it: end the demo, or mute ML ALARM until it ends
             * (Q4). The old "ACK -> NORMAL" demo is gone. */
            switch (io_route_ack(&state->co_alert, state->co_have, &state->fsm, now_us)) {
            case IO_ACK_CO_MUTED:
                ESP_LOGW(TAG, "CO sound muted by ACK (LED keeps showing CO state)");
                break;
            case IO_ACK_FSM:
                ESP_LOGW(TAG, "ACK: %s", state->fsm_prev.demo ? "demo ended" :
                         "ML ALARM sound muted until it ends (LED keeps blinking)");
                break;
            case IO_ACK_NONE:
            default:
                break;
            }
        }
    }
}

/* While a MODE hold is running, look at the pin on every wake (<= 100 ms).
 * gpio_get_level() only reads the input register: no esp_err_t to check. */
static void poll_mode_hold(io_state_t *state, int64_t now_us)
{
    if (!state->mode_hold.down) {
        return;
    }
    const bool pressed = gpio_get_level(BOARD_PIN_BTN_MODE) == 0;
    if (ag_fsm_hold_update(&state->mode_hold, pressed, now_us)) {
        mode_long_press(state, now_us);
    }
}

static void IRAM_ATTR button_isr(void *arg)
{
    const int pin = (int)(intptr_t)arg;
    BaseType_t higher_priority_task_woken = pdFALSE;
    /* queue เต็มจะทิ้ง edge ใหม่: ISR ไม่รอและไม่ log ตามข้อกำหนด FW-1
     * ความจุนี้เป็นค่าคาดหมาย ต้องยืนยันด้วยการกดปุ่มจริงและตรวจจำนวน log */
    xQueueSendFromISR(s_event_queue, &pin, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void io_task(void *arg)
{
    (void)arg;
    io_state_t state = { .indication = IO_IND_RUNNING };
    co_alert_init(&state.co_alert);
    ag_fsm_init(&state.fsm);
    button_state_t buttons[] = {
        { .pin = BOARD_PIN_BTN_MARK },
        { .pin = BOARD_PIN_BTN_MODE },
        { .pin = BOARD_PIN_BTN_ACK },
    };
    TickType_t last_stack_report = xTaskGetTickCount();
    uint32_t previous_outputs = 0;
    bool outputs_valid = false;
    bool first = true;

    /* เลือก task แทน timer: ประมวลผลปุ่ม/log/GPIO ใน context เดียว และไม่มี
     * timer callback มาแชร์สถานะ; queue ปลุกทันทีเมื่อมี event
     * จังหวะไฟคิดจาก esp_timer เทียบกับจุดเริ่มของ pattern (co_alert/ag_fsm)
     * จึงไม่สะสม drift จากงานใน loop */
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        const int64_t now_us = esp_timer_get_time();
        confirm_buttons(&state, buttons, sizeof(buttons) / sizeof(buttons[0]), now, now_us);
        poll_mode_hold(&state, now_us);
        /* FW-5b: CO first. The snapshot is a level, so only the newest one
         * matters (mailbox peek, never blocks). While co.active the CO
         * pattern replaces every other output, so MODE/ACK/ML/logger states
         * cannot hide a CO ALARM or FAULT. */
        state.co_have = co_peek_status(&state.co_status);
        const co_alert_out_t co = co_alert_eval(&state.co_alert,
                                                state.co_have ? &state.co_status : NULL, now_us);
        if (co.alarm_entered) {
            /* latency from the frame that tripped it to this output write */
            ESP_LOGW(TAG, "CO ALARM shown %lld us after its frame",
                     (long long)(now_us - state.co_status.frame_us));
        }
        if (co.fault_entered) {
            ESP_LOGE(TAG, "CO FAULT shown (%s)", co_status_stale(&state.co_status, now_us) ?
                     "CO task stopped publishing" : "sensor");
        }
        /* FW-6: the FSM keeps tracking ML/SYS FAULT under CO (so the right
         * state shows the moment CO ends) but stands aside: its outputs
         * are ignored while co.active and it refuses ACK/demo then. */
        const ag_fsm_in_t fsm_in = fsm_input(&state, co.active);
        const ag_fsm_out_t fsm = ag_fsm_step(&state.fsm, &fsm_in, now_us);
        log_fsm(&state, &fsm, first);
        first = false;

        const uint32_t outputs = io_compose_outputs(&co, &fsm);
        if (!outputs_valid || outputs != previous_outputs) {
            const esp_err_t err = write_outputs(outputs);
            outputs_valid = err == ESP_OK;
            if (outputs_valid) {
                previous_outputs = outputs;
            } else {
                /* Runtime error: log รายขาแล้วลองดับทุก output และ retry
                 * รอบถัดไป (ภายในครึ่งคาบเร็วสุด) ไม่อ้างว่า GPIO สำเร็จ */
                const esp_err_t off_err = write_outputs(0);
                if (off_err != ESP_OK) {
                    ESP_LOGE(TAG, "Could not switch all outputs off: %s", esp_err_to_name(off_err));
                }
                ESP_LOGW(TAG, "Output update failed; retrying");
            }
        }

        if ((TickType_t)(now - last_stack_report) >= pdMS_TO_TICKS(STACK_REPORT_MS)) {
            /* [ขึ้นกับเวอร์ชัน ESP-IDF] v5.5 ESP32-S3 รายงานเป็น bytes */
            ESP_LOGI(TAG, "io stack high-water mark: %u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            last_stack_report = now;
        }

        /* Next wake: the earliest of the CO/FSM pattern edges, the MODE 3 s
         * mark, a pending debounce, the stack report, and IO_MAX_WAIT_MS. */
        const TickType_t bound = pdMS_TO_TICKS(IO_MAX_WAIT_MS);
        TickType_t wait_ticks = ticks_until_us(io_compose_next_change_us(&co, &fsm),
                                               now_us, bound);
        const TickType_t until_hold = ticks_until_us(ag_fsm_hold_deadline_us(&state.mode_hold),
                                                     now_us, bound);
        if (until_hold < wait_ticks) {
            wait_ticks = until_hold;
        }
        const TickType_t until_report = pdMS_TO_TICKS(STACK_REPORT_MS) -
                                       (TickType_t)(now - last_stack_report);
        if (until_report < wait_ticks) {
            wait_ticks = until_report;
        }
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
            if (buttons[i].pending) {
                const TickType_t until_confirm = confirmation_wait(&buttons[i], now);
                if (until_confirm < wait_ticks) {
                    wait_ticks = until_confirm;
                }
            }
        }
        /* deadline อ้าง now เดียวกับ output ด้านบน: ถ้า GPIO/log ใช้เวลาจน
         * ข้ามขอบคาบ ต้องวนใหม่ทันที ไม่รอขอบถัดไปจนพลาดการเปลี่ยนไฟ */
        const TickType_t work_ticks = (TickType_t)(xTaskGetTickCount() - now);
        wait_ticks = work_ticks < wait_ticks ? wait_ticks - work_ticks : 0;
        int event;
        if (xQueueReceive(s_event_queue, &event, wait_ticks) == pdTRUE) {
            handle_event(event, &state, buttons, sizeof(buttons) / sizeof(buttons[0]),
                         xTaskGetTickCount());
        }
    }
}

void io_init(QueueHandle_t mark_queue)
{
    ESP_ERROR_CHECK(s_event_queue == NULL ? ESP_OK : ESP_ERR_INVALID_STATE);
    s_mark_queue = mark_queue;
    s_event_queue = xQueueCreate(IO_QUEUE_LENGTH, sizeof(int));
    ESP_ERROR_CHECK(s_event_queue != NULL ? ESP_OK : ESP_ERR_NO_MEM);
    /* before the io task (and, in app_main, the sampler) is created */
    s_health_mailbox = xQueueCreateStatic(1, sizeof(io_sensor_health_t),
                                          s_health_mailbox_storage, &s_health_mailbox_struct);
    ESP_ERROR_CHECK(s_health_mailbox != NULL ? ESP_OK : ESP_ERR_INVALID_STATE);

    /* ตั้ง latch เป็น 0 ก่อนเปิด output เพื่อให้ LED/buzzer เริ่มต้นดับ */
    ESP_ERROR_CHECK(write_outputs(0));
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << BOARD_PIN_LED_G) | (1ULL << BOARD_PIN_LED_Y) |
                        (1ULL << BOARD_PIN_LED_R) | (1ULL << BOARD_PIN_BUZZ),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&output_config));

    /* ทุกฟิลด์ตรง CLAUDE.md §4; ไม่มี pull-up ภายนอก */
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BOARD_PIN_BTN_MARK) | (1ULL << BOARD_PIN_BTN_MODE) |
                        (1ULL << BOARD_PIN_BTN_ACK),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&button_config));
    /* handler อยู่ IRAM ตามโจทย์; flags=0 ไม่อ้างว่าบริการทั้งเส้นทาง
     * รองรับ interrupt ระหว่าง flash cache ถูกปิด */
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    const BaseType_t created = xTaskCreate(io_task, "io", IO_STACK_BYTES, NULL,
                                          IO_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOARD_PIN_BTN_MARK, button_isr,
                                       (void *)(intptr_t)BOARD_PIN_BTN_MARK));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOARD_PIN_BTN_MODE, button_isr,
                                       (void *)(intptr_t)BOARD_PIN_BTN_MODE));
    ESP_ERROR_CHECK(gpio_isr_handler_add(BOARD_PIN_BTN_ACK, button_isr,
                                       (void *)(intptr_t)BOARD_PIN_BTN_ACK));
}

esp_err_t io_set_mode(io_mode_t mode)
{
    switch (mode) {
    case NORMAL: return ESP_OK;   /* the FSM starts in NORMAL */
    case WATCH:
    case ALARM: return ESP_ERR_NOT_SUPPORTED;
    default: return ESP_ERR_INVALID_ARG;
    }
}

esp_err_t io_set_indication(io_indication_t indication)
{
    int command;
    switch (indication) {
    case IO_IND_RUNNING: command = COMMAND_IND_RUNNING; break;
    case IO_IND_WAIT_TIME: command = COMMAND_IND_WAIT_TIME; break;
    case IO_IND_MARKED: command = COMMAND_IND_MARKED; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_event_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t io_notify_co(void)
{
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int command = COMMAND_CO_CHANGED;
    return xQueueSend(s_event_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t io_publish_sensor_health(const io_sensor_health_t *health)
{
    if (health == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_health_mailbox == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    (void)xQueueOverwrite(s_health_mailbox, health);   /* length 1: always pdPASS */
    return ESP_OK;
}
