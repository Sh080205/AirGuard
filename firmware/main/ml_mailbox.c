#include "ml_mailbox.h"

#include <math.h>
#include <stdint.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Written once by ml_mailbox_init() before the reader/writer tasks exist and
 * never changed afterwards, so they read the handle without a lock. */
static QueueHandle_t s_mailbox;
static StaticQueue_t s_mailbox_struct;
static uint8_t s_mailbox_storage[sizeof(ag_ml_result_t)];

esp_err_t ml_mailbox_init(void)
{
    if (s_mailbox != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_mailbox = xQueueCreateStatic(1, sizeof(ag_ml_result_t), s_mailbox_storage,
                                   &s_mailbox_struct);
    if (s_mailbox == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const ag_ml_result_t none = {
        .status = AG_ML_MISSING,
        .d2 = NAN,
        .streak = 0,
        .at_us = esp_timer_get_time(),
    };
    (void)xQueueOverwrite(s_mailbox, &none);   /* length 1: always pdPASS */
    return ESP_OK;
}

void ml_mailbox_publish(const ag_ml_result_t *r)
{
    if (s_mailbox != NULL && r != NULL) {
        (void)xQueueOverwrite(s_mailbox, r);
    }
}

bool ml_mailbox_peek(ag_ml_result_t *out)
{
    return s_mailbox != NULL && out != NULL && xQueuePeek(s_mailbox, out, 0) == pdTRUE;
}
