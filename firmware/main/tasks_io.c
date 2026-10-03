#include "tasks_io.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board_pins.h"

/* ค่าคาดหมาย ต้องยืนยันด้วยการวัด: debounce ชั้นที่สองหลัง RC บนบอร์ด */
#define DEBOUNCE_MS              50U
#define WATCH_HALF_PERIOD_MS   500U
#define ALARM_HALF_PERIOD_MS   100U
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
};

typedef struct {
    int pin;
    bool pending;
    TickType_t confirm_at;
    uint32_t count;
} button_state_t;

typedef struct {
    io_mode_t mode;
    TickType_t mode_started;
} io_state_t;

static const char *TAG = "io";
/* สร้างก่อนเปิด ISR/สร้าง task และไม่เปลี่ยน handle ตลอดอายุการทำงาน */
static QueueHandle_t s_event_queue;

/* deadline ถูกตั้งล่วงหน้าเพียง DEBOUNCE_MS: ผลลบ unsigned ที่มากกว่าช่วงนี้
 * หมายถึงเลย deadline แล้ว; รองรับ tick wrap โดยไม่เทียบ now >= confirm_at */
static TickType_t confirmation_wait(const button_state_t *button, TickType_t now)
{
    const TickType_t remaining = (TickType_t)(button->confirm_at - now);
    return remaining <= pdMS_TO_TICKS(DEBOUNCE_MS) ? remaining : 0;
}

/* Pure logic: ใช้ครึ่งคาบ 500/100 ms → คาบ 1000/200 ms; duty cycle 50%
 * จังหวะเริ่มใหม่เมื่อเปลี่ยน mode และไม่เลื่อนตามจำนวน event ใน queue
 * timing จริงเป็นค่าคาดหมาย ต้องยืนยันด้วยการวัดบนบอร์ด */
static uint32_t output_for_state(const io_state_t *state, TickType_t now)
{
    if (state->mode == NORMAL) {
        return OUTPUT_GREEN;
    }
    const TickType_t half_period = pdMS_TO_TICKS(
        state->mode == WATCH ? WATCH_HALF_PERIOD_MS : ALARM_HALF_PERIOD_MS);
    const bool on = (((TickType_t)(now - state->mode_started) / half_period) % 2U) == 0;
    if (state->mode == WATCH) {
        return on ? OUTPUT_YELLOW : 0;
    }
    return OUTPUT_BUZZER | (on ? OUTPUT_RED : 0);
}

static void change_mode(io_state_t *state, io_mode_t mode, TickType_t now)
{
    state->mode = mode;
    state->mode_started = now;
    const char *name = mode == NORMAL ? "NORMAL" : mode == WATCH ? "WATCH" : "ALARM";
    ESP_LOGI(TAG, "Mode: %s", name);
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
        } else if (button->pin == BOARD_PIN_BTN_MODE) {
            ESP_LOGI(TAG, "MODE #%" PRIu32, button->count);
            const io_mode_t next = state->mode == NORMAL ? WATCH :
                                   state->mode == WATCH ? ALARM : NORMAL;
            change_mode(state, next, now);
        } else if (button->pin == BOARD_PIN_BTN_ACK) {
            ESP_LOGI(TAG, "ACK #%" PRIu32, button->count);
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
    io_state_t state = { .mode = NORMAL, .mode_started = xTaskGetTickCount() };
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
        const uint32_t outputs = output_for_state(&state, now);
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

        TickType_t wait_ticks = pdMS_TO_TICKS(ALARM_HALF_PERIOD_MS);
        if (state.mode != NORMAL) {
            const TickType_t half_period = pdMS_TO_TICKS(
                state.mode == WATCH ? WATCH_HALF_PERIOD_MS : ALARM_HALF_PERIOD_MS);
            const TickType_t elapsed = (TickType_t)(now - state.mode_started);
            const TickType_t until_edge = half_period - (elapsed % half_period);
            if (until_edge < wait_ticks) {
                wait_ticks = until_edge;
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

void io_init(void)
{
    ESP_ERROR_CHECK(s_event_queue == NULL ? ESP_OK : ESP_ERR_INVALID_STATE);
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
