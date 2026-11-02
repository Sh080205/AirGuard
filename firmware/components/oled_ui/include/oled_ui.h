#pragma once

/* OLED: what the 128x64 status screen shows and the bytes sent to the
 * controller. Pure C (no FreeRTOS, no I2C, no float, no malloc) so
 * firmware/test_host compiles it with gcc; main/tasks_oled.c owns the bus
 * traffic and the task (docs/tasks/reports/OLED.md, step 1 approved 2 Nov).
 *
 * The controller is NOT confirmed (docs/bringup/2026-10-23.md O0: "0.96"
 * 128x64 SSD1306 ... resolution and chip not confirmed"). Only commands
 * that SSD1306 and SH1106 share are used for drawing (page addressing:
 * 0xB0|page, 0x00|col_lo, 0x10|col_hi); the differences (column offset and
 * init list) live in oled_init_cmds() / oled_col_offset() only, picked by
 * one oled_chip_t value.
 *
 * Frame buffer layout = controller RAM layout: 8 pages of 8 pixel rows,
 * byte [page][x], bit 0 = top row of the page.
 *
 * Sent in chunks of half a page (64 columns): one chunk is
 * addr + 0x40 + 64 data bytes = 66 bytes = 594 bits ≈ 5.9 ms at 100 kHz, so
 * the shared I2C bus is never held longer than that by the screen (the
 * sampler's AHT21/ENS160 transfers wait at most one chunk inside their
 * 20 ms timeout). ค่าคาดหมาย ต้องยืนยันด้วยการวัด. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OLED_W              128U
#define OLED_H               64U
#define OLED_PAGES            8U
#define OLED_CHUNK_COLS      64U
#define OLED_CHUNKS_PER_PAGE (OLED_W / OLED_CHUNK_COLS)
#define OLED_CHUNKS          (OLED_PAGES * OLED_CHUNKS_PER_PAGE)   /* 16 */
#define OLED_ALL_CHUNKS      ((uint16_t)0xFFFFU)

/* 5x7 font in a 6x8 cell: 21 characters x 8 text rows */
#define OLED_TEXT_COLS       21U
#define OLED_TEXT_ROWS        8U
#define OLED_LINE_CAP        (OLED_TEXT_COLS + 1U)

/* I2C control byte (first byte after the address): Co = 0, D/C# = 0 means
 * "all following bytes are commands", D/C# = 1 "all following bytes are
 * display data". Same in SSD1306 and SH1106 [ต้องเปิด datasheet ยืนยัน
 * หัวข้อ I2C-bus interface ของทั้งสองชิป]. */
#define OLED_CTRL_CMD        0x00U
#define OLED_CTRL_DATA       0x40U
#define OLED_CMD_DISPLAY_ON  0xAFU

typedef enum {
    OLED_CHIP_SSD1306,
    OLED_CHIP_SH1106,
} oled_chip_t;

typedef struct {
    uint8_t px[OLED_PAGES][OLED_W];
} oled_fb_t;

/* ---- controller commands ---- */

/* RAM column of screen column 0: SSD1306 0, SH1106 2 (132-column RAM,
 * 128 shown) [ต้องเปิด datasheet SH1106 ยืนยัน]. */
unsigned oled_col_offset(oled_chip_t chip);

/* Init list WITHOUT display-on, starting with OLED_CTRL_CMD, ready for one
 * I2C write. rotate180 = false: segment remap 0xA1 + COM scan 0xC8 (the
 * usual "upright" setting of these modules); true: 0xA0 + 0xC0. Which one is
 * upright in the lid is checked on the board (OLED.md step 3).
 * Returns the length, or 0 when cap is too small. */
size_t oled_init_cmds(oled_chip_t chip, bool rotate180, uint8_t *buf, size_t cap);

/* {OLED_CTRL_CMD, 0xB0|page, 0x00|lo, 0x10|hi} for screen column col
 * (offset added). Returns 4, or 0 for page/col out of range. */
size_t oled_pos_cmds(oled_chip_t chip, unsigned page, unsigned col, uint8_t out[4]);

/* ---- frame buffer ---- */

void oled_fb_clear(oled_fb_t *fb);
/* Text at pixel column x on page `page`. scale 1: 6x8 cell on one page;
 * scale 2: 12x16 cell on pages page and page+1. Characters outside
 * 0x20..0x7E draw as '?'. Clipped at the right edge and the last page. */
void oled_fb_text(oled_fb_t *fb, unsigned x, unsigned page, const char *s, unsigned scale);
/* 1-pixel frame on the outer edge + "TL" in the top-left corner: shows
 * the column offset (a missing left edge = SH1106 drawn as SSD1306) and
 * the orientation on the board. */
void oled_fb_test_pattern(oled_fb_t *fb);

/* ---- what to send ----
 * chunk c = page c / 2, columns (c % 2) * 64 .. + 63.
 * valid: bit c set = sent->px of chunk c is known to be on the screen.
 * Returns the first chunk >= from that is not valid or differs, or -1. */
int oled_fb_next_dirty(const oled_fb_t *want, const oled_fb_t *sent, uint16_t valid,
                       unsigned from);
void oled_chunk_where(unsigned chunk, unsigned *page, unsigned *col);

/* ---- status screen ---- */

/* What the LEDs show (io task, FW-5b co_alert + FW-6 FSM), never decided
 * here: the screen must not disagree with the LEDs. */
typedef enum {
    OLED_SHOW_UNKNOWN,     /* no fresh state from the io task */
    OLED_SHOW_NORMAL,
    OLED_SHOW_WATCH,
    OLED_SHOW_FAULT,       /* SYS FAULT */
    OLED_SHOW_ML_ALARM,
    OLED_SHOW_CO_FAULT,
    OLED_SHOW_CO_ALARM,
} oled_show_t;

/* Same bits as AG_FSM_FAULT_* (checked with _Static_assert in tasks_oled.c) */
#define OLED_FAULT_AHT21     (1U << 0)
#define OLED_FAULT_ENS160    (1U << 1)
#define OLED_FAULT_PMS7003   (1U << 2)
#define OLED_FAULT_ML_MODEL  (1U << 3)
#define OLED_FAULT_ML_STALE  (1U << 4)

typedef enum {
    OLED_CO_OFF,           /* CO path not built (ZE07_ENABLE=0) */
    OLED_CO_NONE,          /* no snapshot */
    OLED_CO_WARMUP,
    OLED_CO_FAULT,
    OLED_CO_NO_VALUE,      /* running, but no frame young enough (CSV rule) */
    OLED_CO_VALUE,
} oled_co_t;

/* Mirror of ag_ml_status_t (FW-7), plus NONE / STALE */
typedef enum {
    OLED_ML_NONE,          /* nothing published yet */
    OLED_ML_STALE,         /* newest result older than the freshness limit */
    OLED_ML_MODEL_FAULT,
    OLED_ML_MISSING,
    OLED_ML_BELOW,
    OLED_ML_ABOVE,
    OLED_ML_PERSISTENT,
} oled_ml_t;

typedef struct {
    oled_show_t show;
    bool demo;
    bool muted;            /* a sound muted by ACK (CO or ML ALARM) */
    uint8_t fault_bits;    /* OLED_FAULT_*, shown while show == FAULT */

    oled_co_t co;
    uint16_t co_x10;       /* OLED_CO_VALUE: ppm x 10 (same value as co_ppm) */
    uint16_t co_mean1h_x10;

    bool row_seen;         /* the sampler published at least one row */
    bool row_fresh;        /* ... and the newest is young enough */
    bool has_pm;    uint16_t pm2_5;
    bool has_th;    long temp_x10, rh_x10;   /* csv_round_tenths of the row */
    bool has_voc;   uint16_t tvoc_ppb, eco2_ppm;
    bool time_valid;       /* the row has real time = rows are being written */
    uint8_t hh, mm, ss;
    bool marked;

    oled_ml_t ml;
    bool d2_valid;
    long d2_x100;          /* D² x 100, rounded; >= 0 */
} oled_ui_in_t;

typedef struct {
    char big[OLED_LINE_CAP];   /* pages 0-1, scale 2, <= 8 characters */
    char tag[2][5];            /* small, right of big: page 0 / page 1 */
    char line[6][OLED_LINE_CAP];   /* pages 2..7 */
} oled_ui_text_t;

/* Status screen text. Integer formatting only: one decimal for T/RH/CO,
 * two for D². Missing values are "--" (the CSV leaves them empty). */
void oled_ui_format(const oled_ui_in_t *in, oled_ui_text_t *out);
void oled_ui_draw(const oled_ui_text_t *t, oled_fb_t *fb);

/* Boot screen: CLAUDE.md §8 in English (the font has no Thai) + model
 * version. Does not replace the label on the device (§10). */
typedef struct {
    char line[OLED_TEXT_ROWS][OLED_LINE_CAP];
} oled_ui_lines_t;

void oled_ui_boot_lines(const char *model_version, oled_ui_lines_t *out);
void oled_ui_draw_lines(const oled_ui_lines_t *lines, oled_fb_t *fb);

#ifdef __cplusplus
}
#endif
