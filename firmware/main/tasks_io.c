#include "tasks_io.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"
#include "co_alert.h"
#include "tasks_co.h"

/* ค่าคาดหมาย ต้องยืนยันด้วยการวัด: debounce ชั้นที่สองหลัง RC บนบอร์ด */
#define DEBOUNCE_MS              50U
#define WATCH_HALF_PERIOD_MS   500U
#define ALARM_HALF_PERIOD_MS   100U
/* FW-4 indications on LED_G while NORMAL; reasons in tasks_io.h */
#define IND_PERIOD_MS         2000U
#define IND_WAIT_ON_MS         100U
#define IND_MARKED_ON_MS      1900U
#define STACK_REPORT_MS      10000U
#define IO_QUEUE_LENGTH         16U

/* ESP-IDF รับ stack เป็น bytes: state/locals เผื่อ 512 + call chain ของ
 * GPIO/queue/log เผื่อ 1536 + margin 2048 = 4096 bytes
 * ทั้งหมดเป็นค่าคาดหมาย ต้องยืนยันด้วยการวัด high-water mark บนบอร์ด */
#define IO_STACK_BYTES        4096U
#define IO_TASK_PRIORITY         5U

enum {
    OUTPUT_GREEN  = 1U << 0,
    OUTPUT_YELLOW = 1U << 1,
    OUTPUT_RED    = 1U << 2,
    OUTPUT_BUZZER = 1U << 3,
};

/* queue เดียวส่งเลขขาบวกจาก ISR และคำสั่งลบจาก io_set_mode()
 * ทำให้ io task เป็นเจ้าของ mode, debounce, counter และ output เพียงผู้เดียว
 * จึงไม่ต้องแชร์ตัวแปรสถานะแบบ volatile ระหว่าง ISR/task หรือระหว่าง task */
enum {
    COMMAND_NORMAL = -1,
    COMMAND_WATCH  = -2,
    COMMAND_ALARM  = -3,
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
    io_mode_t mode;
    io_indication_t indication;
    /* phase origin of the current blink pattern: reset on every mode or
     * indication change so a pattern always starts with its "on" part */
    TickType_t mode_started;
    /* FW-5b: CO path display. co_status is the latest snapshot peeked from
     * the CO mailbox (a copy: the io task never shares it); co_alert decides
     * LED_R/buzzer and overrides mode/indication while active. */
    co_alert_t co_alert;
    co_status_t co_status;
    bool co_have;
} io_state_t;

/* One blink pattern: outputs = always | (on-phase ? blink : 0).
 * period_ms == 0 means steady (always | blink). */
typedef struct {
    uint32_t period_ms;
    uint32_t on_ms;
    uint32_t blink;
    uint32_t always;
} pattern_t;

static const char *TAG = "io";
/* สร้างก่อนเปิด ISR/สร้าง task และไม่เปลี่ยน handle ตลอดอายุการทำงาน */
static QueueHandle_t s_event_queue;
/* FW-4: ตั้งใน io_init ก่อนสร้าง io task และไม่เปลี่ยนอีก → task อ่านได้
 * โดยไม่ต้อง lock (xTaskCreate เป็น happens-before ของ task ใหม่) */
static QueueHandle_t s_mark_queue;

/* deadline ถูกตั้งล่วงหน้าเพียง DEBOUNCE_MS: ผลลบ unsigned ที่มากกว่าช่วงนี้
 * หมายถึงเลย deadline แล้ว; รองรับ tick wrap โดยไม่เทียบ now >= confirm_at */
static TickType_t confirmation_wait(const button_state_t *button, TickType_t now)
{
    const TickType_t remaining = (TickType_t)(button->confirm_at - now);
    return remaining <= pdMS_TO_TICKS(DEBOUNCE_MS) ? remaining : 0;
}

/* ใช้ครึ่งคาบ 500/100 ms → คาบ 1000/200 ms; duty cycle 50% (FW-1)
 * FW-4 เพิ่มจังหวะ indication บน LED_G เฉพาะตอน NORMAL (เหตุผลใน tasks_io.h)
 * จังหวะเริ่มใหม่เมื่อเปลี่ยน mode/indication และไม่เลื่อนตามจำนวน event ใน queue
 * timing จริงเป็นค่าคาดหมาย ต้องยืนยันด้วยการวัดบนบอร์ด */
static pattern_t pattern_for_state(const io_state_t *state)
{
    switch (state->mode) {
    case WATCH:
        return (pattern_t){ 2U * WATCH_HALF_PERIOD_MS, WATCH_HALF_PERIOD_MS, OUTPUT_YELLOW, 0 };
    case ALARM:
        return (pattern_t){ 2U * ALARM_HALF_PERIOD_MS, ALARM_HALF_PERIOD_MS, OUTPUT_RED, OUTPUT_BUZZER };
    case NORMAL:
    default:
        break;
    }
    switch (state->indication) {
    case IO_IND_WAIT_TIME:
        return (pattern_t){ IND_PERIOD_MS, IND_WAIT_ON_MS, OUTPUT_GREEN, 0 };
    case IO_IND_MARKED:
        return (pattern_t){ IND_PERIOD_MS, IND_MARKED_ON_MS, OUTPUT_GREEN, 0 };
    case IO_IND_RUNNING:
    default:
        return (pattern_t){ 0, 0, OUTPUT_GREEN, 0 };
    }
}

static uint32_t output_for_state(const io_state_t *state, TickType_t now)
{
    const pattern_t p = pattern_for_state(state);
    if (p.period_ms == 0) {
        return p.always | p.blink;
    }
    const TickType_t phase = (TickType_t)(now - state->mode_started) % pdMS_TO_TICKS(p.period_ms);
    return p.always | (phase < pdMS_TO_TICKS(p.on_ms) ? p.blink : 0);
}

/* ticks until output_for_state() can change next, capped at bound;
 * a steady pattern never changes by itself, so it returns bound */
static TickType_t ticks_until_edge(const io_state_t *state, TickType_t now, TickType_t bound)
{
    const pattern_t p = pattern_for_state(state);
    if (p.period_ms == 0) {
        return bound;
    }
    const TickType_t period = pdMS_TO_TICKS(p.period_ms);
    const TickType_t on = pdMS_TO_TICKS(p.on_ms);
    const TickType_t phase = (TickType_t)(now - state->mode_started) % period;
    const TickType_t until = phase < on ? on - phase : period - phase;
    return until < bound ? until : bound;
}

static void change_mode(io_state_t *state, io_mode_t mode, TickType_t now)
{
    state->mode = mode;
    state->mode_started = now;
    const char *name = mode == NORMAL ? "NORMAL" : mode == WATCH ? "WATCH" : "ALARM";
    ESP_LOGI(TAG, "Mode: %s", name);
}

static void change_indication(io_state_t *state, io_indication_t indication, TickType_t now)
{
    if (state->indication == indication) {
        return;
    }
    state->indication = indication;
    if (state->mode == NORMAL) {
        state->mode_started = now;
    }
    const char *name = indication == IO_IND_WAIT_TIME ? "WAIT_TIME" :
                       indication == IO_IND_MARKED ? "MARKED" : "RUNNING";
    ESP_LOGI(TAG, "Indication: %s", name);
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
    const uint32_t bits[] = { OUTPUT_GREEN, OUTPUT_YELLOW, OUTPUT_RED, OUTPUT_BUZZER };
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

static void handle_event(int event, io_state_t *state, button_state_t *buttons,
                         size_t button_count, TickType_t now)
{
    switch (event) {
    case COMMAND_NORMAL: change_mode(state, NORMAL, now); return;
    case COMMAND_WATCH: change_mode(state, WATCH, now); return;
    case COMMAND_ALARM: change_mode(state, ALARM, now); return;
    case COMMAND_IND_RUNNING: change_indication(state, IO_IND_RUNNING, now); return;
    case COMMAND_IND_WAIT_TIME: change_indication(state, IO_IND_WAIT_TIME, now); return;
    case COMMAND_IND_MARKED: change_indication(state, IO_IND_MARKED, now); return;
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

static void confirm_buttons(io_state_t *state, button_state_t *buttons,
                            size_t button_count, TickType_t now)
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
            ESP_LOGI(TAG, "MODE #%" PRIu32, button->count);
            const io_mode_t next = state->mode == NORMAL ? WATCH :
                                   state->mode == WATCH ? ALARM : NORMAL;
            change_mode(state, next, now);
        } else if (button->pin == BOARD_PIN_BTN_ACK) {
            ESP_LOGI(TAG, "ACK #%" PRIu32, button->count);
            /* FW-5b: ACK only mutes the CO buzzer/chirp; it never clears a
             * CO ALARM (co_alert.h). The demo mode still goes to NORMAL. */
            if (state->co_have && co_alert_ack(&state->co_alert, esp_timer_get_time())) {
                ESP_LOGW(TAG, "CO sound muted by ACK (LED keeps showing CO state)");
            }
            change_mode(state, NORMAL, now);
        }
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
    io_state_t state = { .mode = NORMAL, .indication = IO_IND_RUNNING,
                         .mode_started = xTaskGetTickCount() };
    co_alert_init(&state.co_alert);
    button_state_t buttons[] = {
        { .pin = BOARD_PIN_BTN_MARK },
        { .pin = BOARD_PIN_BTN_MODE },
        { .pin = BOARD_PIN_BTN_ACK },
    };
    TickType_t last_stack_report = xTaskGetTickCount();
    uint32_t previous_outputs = 0;
    bool outputs_valid = false;

    /* เลือก task แทน timer: ประมวลผลปุ่ม/log/GPIO ใน context เดียว และไม่มี
     * timer callback มาแชร์ mode; queue ปลุกทันทีเมื่อมี event
     * timeout ยึดขอบครึ่งคาบจาก mode_started จึงไม่สะสม drift จากงานใน loop */
    for (;;) {
        const TickType_t now = xTaskGetTickCount();
        confirm_buttons(&state, buttons, sizeof(buttons) / sizeof(buttons[0]), now);
        /* FW-5b: CO first. The snapshot is a level, so only the newest one
         * matters (mailbox peek, never blocks). While co.active the CO
         * pattern replaces every other output, so MODE/ACK/logger states
         * cannot hide a CO ALARM or FAULT. */
        const int64_t now_us = esp_timer_get_time();
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
        const uint32_t outputs = co.active ?
            ((co.red ? OUTPUT_RED : 0U) | (co.buzzer ? OUTPUT_BUZZER : 0U)) :
            output_for_state(&state, now);
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

        TickType_t wait_ticks = ticks_until_edge(&state, now,
                                                 pdMS_TO_TICKS(ALARM_HALF_PERIOD_MS));
        if (co.next_change_us != INT64_MAX) {
            const int64_t until_us = co.next_change_us - now_us;
            const TickType_t until_co = until_us <= 0 ? 0 :
                pdMS_TO_TICKS((uint32_t)((until_us + 999) / 1000));
            if (until_co < wait_ticks) {
                wait_ticks = until_co;
            }
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
    int command;
    switch (mode) {
    case NORMAL: command = COMMAND_NORMAL; break;
    case WATCH: command = COMMAND_WATCH; break;
    case ALARM: command = COMMAND_ALARM; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (s_event_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_event_queue, &command, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
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
