#ifndef TEST_FREERTOS_H
#define TEST_FREERTOS_H
#include <stdint.h>
/* Host model only: test unsigned tick wrap and finite deadlines. */
typedef uint32_t TickType_t;
#define portMAX_DELAY UINT32_MAX
#endif
