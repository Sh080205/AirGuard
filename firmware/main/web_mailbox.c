#include "web_mailbox.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

/* Written once by web_mailbox_init() before the writer/reader tasks exist
 * and never changed afterwards, so the tasks read the handles without a
 * lock. Not pre-filled: "no publish yet" stays visible as a failed peek. */
static QueueHandle_t s_row;
static StaticQueue_t s_row_struct;
static uint8_t s_row_storage[sizeof(web_row_t)];

static QueueHandle_t s_show;
static StaticQueue_t s_show_struct;
static uint8_t s_show_storage[sizeof(web_show_t)];

esp_err_t web_mailbox_init(void)
{
    if (s_row != NULL || s_show != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_row = xQueueCreateStatic(1, sizeof(web_row_t), s_row_storage, &s_row_struct);
    s_show = xQueueCreateStatic(1, sizeof(web_show_t), s_show_storage, &s_show_struct);
    return (s_row != NULL && s_show != NULL) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void web_row_publish(const web_row_t *r)
{
    if (s_row != NULL && r != NULL) {
        (void)xQueueOverwrite(s_row, r);   /* length 1: always pdPASS */
    }
}

void web_show_publish(const web_show_t *s)
{
    if (s_show != NULL && s != NULL) {
        (void)xQueueOverwrite(s_show, s);
    }
}

bool web_row_peek(web_row_t *out)
{
    return s_row != NULL && out != NULL && xQueuePeek(s_row, out, 0) == pdTRUE;
}

bool web_show_peek(web_show_t *out)
{
    return s_show != NULL && out != NULL && xQueuePeek(s_show, out, 0) == pdTRUE;
}
