#ifndef AIRGUARD_PMS7003_H
#define AIRGUARD_PMS7003_H

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Plantower PMS7003 series data manual V2.5 (2016-06-01), Appendix I,
 * pp. 13-14: 32 bytes including header, length, data and checksum.
 * Source: https://www.es.co.th/Schemetic/PDF/PMS7003.PDF
 */
#define PMS7003_FRAME_SIZE 32U

/* Appendix I, Data1-6: PM mass concentrations in ug/m^3; keep both sets. */
typedef struct {
    uint16_t pm1_0_cf1;
    uint16_t pm2_5_cf1;
    uint16_t pm10_cf1;
    uint16_t pm1_0_atmospheric;
    uint16_t pm2_5_atmospheric;
    uint16_t pm10_atmospheric;
} pms7003_data_t;

/* No FreeRTOS/driver dependency in the parser translation unit or host tests.
 * Define PMS7003_PARSE_ONLY before including this header for pure C callers.
 * buf, out and consumed must be non-NULL and refer to non-overlapping storage.
 * out changes only on ESP_OK. consumed is initialized to zero when non-NULL.
 * ESP_OK: consume prefix + frame; INVALID_SIZE: consume prefix only;
 * NOT_FOUND: consume len except a trailing first header byte;
 * INVALID_CRC/INVALID_RESPONSE: consume through first header byte.
 */
esp_err_t pms7003_parse(const uint8_t *buf, size_t len,
                        pms7003_data_t *out, size_t *consumed);

#ifndef PMS7003_PARSE_ONLY
#include "freertos/FreeRTOS.h"

/* Single task owns init/read and UART1; serialize all calls externally.
 * No task/ISR is created here; the ESP-IDF UART driver owns its ISR/ring buffer.
 * Init is idempotent after success, sets normal SET/RESET levels, and does not
 * wait for warm-up. Manual V2.5, Circuit Attentions item 4 (p. 10): allow at
 * least 30 seconds after waking from sleep before treating data as stable.
 * The caller owns this warm-up policy; read returns raw validated frames.
 * Output result (p. 4): active mode is the power-on default. This driver assumes
 * that default and sends no mode commands or reset pulses.
 */
esp_err_t pms7003_init(void);

/* Finite total tick budget, including resynchronization. portMAX_DELAY is
 * rejected with INVALID_ARG. Zero polls at most one internal-buffer-sized
 * UART chunk plus already buffered data. Partial frames survive timeout.
 * Corrupt frames are skipped; no valid frame by deadline => ESP_ERR_TIMEOUT.
 * out is unchanged on failure; UART read errors return ESP_FAIL.
 * Frames are returned in arrival order (FIFO): each call yields the OLDEST
 * valid frame still buffered, not the newest. To get the latest value, drain:
 * call with timeout 0 until ESP_ERR_TIMEOUT and keep the last ESP_OK result
 * (no ESP_OK => no new frame since the previous drain).
 */
esp_err_t pms7003_read(pms7003_data_t *out, TickType_t timeout);
#endif

#endif
