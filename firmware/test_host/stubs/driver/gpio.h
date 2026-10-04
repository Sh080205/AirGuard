#ifndef TEST_GPIO_H
#define TEST_GPIO_H
#include <stdint.h>
#include "esp_err.h"
/* Symbolic host substitutes, not peripheral register values. */
enum { GPIO_MODE_OUTPUT, GPIO_PULLUP_DISABLE, GPIO_PULLDOWN_DISABLE,
       GPIO_INTR_DISABLE };
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_set_level(int pin, uint32_t level);
esp_err_t gpio_config(const gpio_config_t *config);
#endif
