#include "oled_ui.h"

#include <stdio.h>
#include <string.h>

#include "oled_font.h"

#define CELL_W        6U    /* 5 font columns + 1 gap */
#define BIG_CELL_W   12U
#define TAG_X       104U    /* 4 small characters up to the right edge */

/* ---- controller commands ----
 * SSD1306: same settings as ESP-IDF v5.5.5 esp_lcd_panel_ssd1306.c
 * (multiplex 0x3F, COM pins 0x12, charge pump 0x8D 0x14, remap 0xA0/0xA1,
 * scan 0xC0/0xC8) except the addressing mode: page addressing (0x20 0x02)
 * instead of horizontal, because drawing uses only the page-mode commands
 * SH1106 also has. Page addressing is the SSD1306 reset value; sent anyway
 * [ต้องเปิด datasheet SSD1306 §10.1.3 ยืนยัน].
 * SH1106: only what differs from its reset values for this panel
 * (multiplex, remap, scan). DC-DC on (0xAD 0x8B), COM pads 0x12 and start
 * line 0 are left at reset [ต้องเปิด datasheet SH1106 หัวข้อ Command Table
 * ยืนยัน]; used only if the board shows an SH1106 (OLED.md step 3). */
unsigned oled_col_offset(oled_chip_t chip)
{
    return chip == OLED_CHIP_SH1106 ? 2U : 0U;
}

size_t oled_init_cmds(oled_chip_t chip, bool rotate180, uint8_t *buf, size_t cap)
{
    static const uint8_t SSD1306[] = {
        OLED_CTRL_CMD,
        0xAE,             /* display off while RAM is still random */
        0xA8, 0x3F,       /* multiplex ratio 64 */
        0xDA, 0x12,       /* COM pins: alternative, for 64 rows */
        0x20, 0x02,       /* page addressing mode */
        0x8D, 0x14,       /* charge pump on */
    };
    static const uint8_t SH1106[] = {
        OLED_CTRL_CMD,
        0xAE,
        0xA8, 0x3F,
    };
    const uint8_t *base = chip == OLED_CHIP_SH1106 ? SH1106 : SSD1306;
    const size_t n = chip == OLED_CHIP_SH1106 ? sizeof(SH1106) : sizeof(SSD1306);
    if (buf == NULL || cap < n + 2U) {
        return 0;
    }
    memcpy(buf, base, n);
    buf[n] = rotate180 ? 0xA0 : 0xA1;       /* segment remap */
    buf[n + 1U] = rotate180 ? 0xC0 : 0xC8;  /* COM scan direction */
    return n + 2U;
}

size_t oled_pos_cmds(oled_chip_t chip, unsigned page, unsigned col, uint8_t out[4])
{
    if (out == NULL || page >= OLED_PAGES || col >= OLED_W) {
        return 0;
    }
    const unsigned ram_col = col + oled_col_offset(chip);
    out[0] = OLED_CTRL_CMD;
    out[1] = (uint8_t)(0xB0U | page);
    out[2] = (uint8_t)(0x00U | (ram_col & 0x0FU));
    out[3] = (uint8_t)(0x10U | (ram_col >> 4));
    return 4;
}

/* ---- frame buffer ---- */

void oled_fb_clear(oled_fb_t *fb)
{
    memset(fb, 0, sizeof(*fb));
}

static const uint8_t *glyph(char c)
{
    unsigned u = (unsigned char)c;
    if (u < OLED_FONT_FIRST || u > OLED_FONT_LAST) {
        u = '?';
    }
    return oled_font5x7[u - OLED_FONT_FIRST];
}

/* 7 font rows -> 14 rows: bit i becomes bits 2i and 2i+1 */
static uint16_t stretch(uint8_t b)
{
    uint16_t e = 0;
    for (unsigned i = 0; i < 8U; ++i) {
        if ((b >> i) & 1U) {
            e |= (uint16_t)(3U << (2U * i));
        }
    }
    return e;
}

void oled_fb_text(oled_fb_t *fb, unsigned x, unsigned page, const char *s, unsigned scale)
{
    if (fb == NULL || s == NULL || page >= OLED_PAGES) {
        return;
    }
    for (; *s != '\0' && x < OLED_W; ++s) {
        const uint8_t *g = glyph(*s);
        for (unsigned k = 0; k < OLED_FONT_W; ++k) {
            if (scale == 2U) {
                const uint16_t e = stretch(g[k]);
                for (unsigned d = 0; d < 2U; ++d) {
                    const unsigned px = x + 2U * k + d;
                    if (px >= OLED_W) {
                        break;
                    }
                    fb->px[page][px] |= (uint8_t)(e & 0xFFU);
                    if (page + 1U < OLED_PAGES) {
                        fb->px[page + 1U][px] |= (uint8_t)(e >> 8);
                    }
                }
            } else if (x + k < OLED_W) {
                fb->px[page][x + k] |= g[k];
            }
        }
        x += scale == 2U ? BIG_CELL_W : CELL_W;
    }
}

void oled_fb_test_pattern(oled_fb_t *fb)
{
    oled_fb_clear(fb);
    for (unsigned x = 0; x < OLED_W; ++x) {
        fb->px[0][x] |= 0x01U;                  /* top row */
        fb->px[OLED_PAGES - 1U][x] |= 0x80U;    /* bottom row */
    }
    for (unsigned p = 0; p < OLED_PAGES; ++p) {
        fb->px[p][0] = 0xFFU;                   /* left column */
        fb->px[p][OLED_W - 1U] = 0xFFU;         /* right column */
    }
    oled_fb_text(fb, 3, 1, "TL", 1);
    oled_fb_text(fb, OLED_W - 3U - 2U * CELL_W, 1, "TR", 1);
    oled_fb_text(fb, 3, 6, "BL", 1);
    oled_fb_text(fb, OLED_W - 3U - 2U * CELL_W, 6, "BR", 1);
    oled_fb_text(fb, 10, 3, "OLED TEST", 2);
}

void oled_chunk_where(unsigned chunk, unsigned *page, unsigned *col)
{
    *page = chunk / OLED_CHUNKS_PER_PAGE;
    *col = (chunk % OLED_CHUNKS_PER_PAGE) * OLED_CHUNK_COLS;
}

int oled_fb_next_dirty(const oled_fb_t *want, const oled_fb_t *sent, uint16_t valid,
                       unsigned from)
{
    for (unsigned c = from; c < OLED_CHUNKS; ++c) {
        if (((valid >> c) & 1U) == 0U) {
            return (int)c;
        }
        unsigned page;
        unsigned col;
        oled_chunk_where(c, &page, &col);
        if (memcmp(&want->px[page][col], &sent->px[page][col], OLED_CHUNK_COLS) != 0) {
            return (int)c;
        }
    }
    return -1;
}

/* ---- status screen ---- */

/* one decimal from tenths: -123 -> "-12.3", 5 -> "0.5". At most 5
 * characters: everything shown this way (T, RH, CO up to its 500 ppm full
 * scale) fits; anything beyond is shown as a bound, not cut. */
static void fmt_tenths(long v, char *buf, size_t cap)
{
    if (v > 9999L) {
        snprintf(buf, cap, ">999");
        return;
    }
    if (v < -999L) {
        snprintf(buf, cap, "<-99");
        return;
    }
    const unsigned a = (unsigned)(v < 0 ? -v : v);
    snprintf(buf, cap, "%s%u.%u", v < 0 ? "-" : "", a / 10U, a % 10U);
}

/* Lines are built in a scratch buffer and then cut to the 21 columns, so a
 * field wider than planned shortens the line instead of overflowing. */
#define SCRATCH_CAP 48U

static void set_line(char *out, const char *scratch)
{
    size_t n = 0;
    while (n < OLED_TEXT_COLS && scratch[n] != '\0') {
        ++n;
    }
    memcpy(out, scratch, n);
    out[n] = '\0';
}

/* D² from hundredths; >= 10000 does not fit the 7-character field */
static void fmt_d2(long x100, char *buf, size_t cap)
{
    if (x100 < 0) {
        snprintf(buf, cap, "--");
    } else if (x100 >= 1000000L) {
        snprintf(buf, cap, ">9999");
    } else {
        snprintf(buf, cap, "%ld.%02ld", x100 / 100L, x100 % 100L);
    }
}

static const char *show_word(oled_show_t s)
{
    switch (s) {
    case OLED_SHOW_NORMAL:   return "NORMAL";
    case OLED_SHOW_WATCH:    return "WATCH";
    case OLED_SHOW_FAULT:    return "FAULT";
    case OLED_SHOW_ML_ALARM: return "ML ALARM";
    case OLED_SHOW_CO_FAULT: return "CO FAULT";
    case OLED_SHOW_CO_ALARM: return "CO ALARM";
    case OLED_SHOW_UNKNOWN:
    default:                 return "--";
    }
}

static void line_co(const oled_ui_in_t *in, char *out)
{
    char now[8];
    char mean[8];
    switch (in->co) {
    case OLED_CO_OFF:
        snprintf(out, OLED_LINE_CAP, "CO  not built");
        return;
    case OLED_CO_WARMUP:
        snprintf(out, OLED_LINE_CAP, "CO  warm-up");
        return;
    case OLED_CO_FAULT:
        snprintf(out, OLED_LINE_CAP, "CO  FAULT");
        return;
    case OLED_CO_NO_VALUE:
        snprintf(out, OLED_LINE_CAP, "CO    -- ppm");
        return;
    case OLED_CO_VALUE:
        fmt_tenths(in->co_x10, now, sizeof(now));
        fmt_tenths(in->co_mean1h_x10, mean, sizeof(mean));
        char tmp[SCRATCH_CAP];
        snprintf(tmp, sizeof(tmp), "CO %5s ppm 1h %5s", now, mean);
        set_line(out, tmp);
        return;
    case OLED_CO_NONE:
    default:
        snprintf(out, OLED_LINE_CAP, "CO  --");
        return;
    }
}

static void line_faults(uint8_t bits, char *out)
{
    static const struct { uint8_t bit; const char *name; } NAMES[] = {
        { OLED_FAULT_AHT21, "AHT" },
        { OLED_FAULT_ENS160, "ENS" },
        { OLED_FAULT_PMS7003, "PMS" },
        { OLED_FAULT_ML_MODEL, "MDL" },
        { OLED_FAULT_ML_STALE, "STL" },
    };
    /* "F:AHT ENS PMS MDL STL" = 21 characters with every bit set */
    size_t used = (size_t)snprintf(out, OLED_LINE_CAP, "F:");
    bool first = true;
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); ++i) {
        if ((bits & NAMES[i].bit) != 0U && used < OLED_LINE_CAP) {
            const int n = snprintf(out + used, OLED_LINE_CAP - used, "%s%s",
                                   first ? "" : " ", NAMES[i].name);
            if (n < 0) {
                break;
            }
            used += (size_t)n;
            first = false;
        }
    }
}

static void line_ml(const oled_ui_in_t *in, char *out)
{
    const char *word = NULL;
    switch (in->ml) {
    case OLED_ML_STALE:       snprintf(out, OLED_LINE_CAP, "ML  stale"); return;
    case OLED_ML_MODEL_FAULT: snprintf(out, OLED_LINE_CAP, "ML  MODEL FAULT"); return;
    case OLED_ML_MISSING:     snprintf(out, OLED_LINE_CAP, "ML  MISSING"); return;
    case OLED_ML_BELOW:       word = "BELOW"; break;
    case OLED_ML_ABOVE:       word = "ABOVE"; break;
    case OLED_ML_PERSISTENT:  word = "PERSIST"; break;
    case OLED_ML_NONE:
    default:                  snprintf(out, OLED_LINE_CAP, "ML  --"); return;
    }
    char d2[8];
    char tmp[SCRATCH_CAP];
    fmt_d2(in->d2_valid ? in->d2_x100 : -1L, d2, sizeof(d2));
    snprintf(tmp, sizeof(tmp), "ML %-7s D2 %7s", word, d2);
    set_line(out, tmp);
}

void oled_ui_format(const oled_ui_in_t *in, oled_ui_text_t *out)
{
    memset(out, 0, sizeof(*out));
    snprintf(out->big, sizeof(out->big), "%s", show_word(in->show));
    snprintf(out->tag[0], sizeof(out->tag[0]), "%s", in->demo ? "DEMO" : "");
    snprintf(out->tag[1], sizeof(out->tag[1]), "%s", in->muted ? "MUTE" : "");

    line_co(in, out->line[0]);

    /* a stale row shows nothing rather than old values */
    const bool fresh = in->row_seen && in->row_fresh;
    char a[8] = "--";
    char b[8] = "--";
    char tmp[SCRATCH_CAP];
    if (fresh && in->has_pm) {
        snprintf(a, sizeof(a), "%u", (unsigned)in->pm2_5);
    }
    snprintf(tmp, sizeof(tmp), "PM2.5 %5s ug/m3", a);
    set_line(out->line[1], tmp);

    snprintf(a, sizeof(a), "--");
    if (fresh && in->has_th) {
        fmt_tenths(in->temp_x10, a, sizeof(a));
        fmt_tenths(in->rh_x10, b, sizeof(b));
    }
    snprintf(tmp, sizeof(tmp), "T %5sC  RH %5s%%", a, b);
    set_line(out->line[2], tmp);

    if (in->show == OLED_SHOW_FAULT && in->fault_bits != 0U) {
        line_faults(in->fault_bits, out->line[3]);
    } else {
        snprintf(a, sizeof(a), "--");
        snprintf(b, sizeof(b), "--");
        if (fresh && in->has_voc) {
            snprintf(a, sizeof(a), "%u", (unsigned)in->tvoc_ppb);
            snprintf(b, sizeof(b), "%u", (unsigned)in->eco2_ppm);
        }
        snprintf(tmp, sizeof(tmp), "VOC %5s  eCO2 %5s", a, b);
        set_line(out->line[3], tmp);
    }

    line_ml(in, out->line[4]);

    if (!in->row_seen) {
        snprintf(out->line[5], OLED_LINE_CAP, "WAIT SAMPLER");
    } else if (!in->row_fresh) {
        snprintf(out->line[5], OLED_LINE_CAP, "SAMPLER STALE");
    } else if (!in->time_valid) {
        snprintf(out->line[5], OLED_LINE_CAP, "NO TIME  not logging");
    } else {
        snprintf(out->line[5], OLED_LINE_CAP, "%02u:%02u:%02u REC%s",
                 (unsigned)(in->hh % 24U), (unsigned)(in->mm % 60U), (unsigned)(in->ss % 61U),
                 in->marked ? "  MARK" : "");
    }
}

void oled_ui_draw(const oled_ui_text_t *t, oled_fb_t *fb)
{
    oled_fb_clear(fb);
    oled_fb_text(fb, 0, 0, t->big, 2);
    oled_fb_text(fb, TAG_X, 0, t->tag[0], 1);
    oled_fb_text(fb, TAG_X, 1, t->tag[1], 1);
    for (unsigned i = 0; i < 6U; ++i) {
        oled_fb_text(fb, 0, 2U + i, t->line[i], 1);
    }
}

void oled_ui_boot_lines(const char *model_version, oled_ui_lines_t *out)
{
    static const char *const TEXT[OLED_TEXT_ROWS - 1U] = {
        "RESEARCH MONITOR ONLY",
        "NOT A MEDICAL DEVICE",
        "NOT A CO ALARM.",
        "Do not use instead of",
        "a UL 2034 / EN 50291",
        "certified CO alarm.",
        "",
    };
    for (unsigned i = 0; i + 1U < OLED_TEXT_ROWS; ++i) {
        snprintf(out->line[i], OLED_LINE_CAP, "%s", TEXT[i]);
    }
    /* model version, cut to the line ("ML " + 18 characters) */
    snprintf(out->line[OLED_TEXT_ROWS - 1U], OLED_LINE_CAP, "ML %.18s",
             model_version != NULL ? model_version : "?");
}

void oled_ui_draw_lines(const oled_ui_lines_t *lines, oled_fb_t *fb)
{
    oled_fb_clear(fb);
    for (unsigned i = 0; i < OLED_TEXT_ROWS; ++i) {
        oled_fb_text(fb, 0, i, lines->line[i], 1);
    }
}
