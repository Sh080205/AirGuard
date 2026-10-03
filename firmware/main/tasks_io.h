#ifndef AIRGUARD_TASKS_IO_H
#define AIRGUARD_TASKS_IO_H

#include "esp_err.h"

typedef enum {
    NORMAL,
    WATCH,
    ALARM,
} io_mode_t;

/* เรียกครั้งเดียวจาก app_main; init ล้มเหลวจะหยุดผ่าน ESP_ERROR_CHECK */
void io_init(void);

/* ส่งคำสั่งจาก task เท่านั้น; คืน ESP_ERR_TIMEOUT เมื่อ queue เต็ม
 * ESP_OK หมายถึงเข้าคิวแล้ว โดย io task จะเป็นผู้เปลี่ยน GPIO */
esp_err_t io_set_mode(io_mode_t mode);

#endif
