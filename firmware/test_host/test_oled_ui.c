#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "oled_ui.h"

#define PASS(name) printf("PASS %s\n", name)

static void expect_bytes(const uint8_t *got, size_t got_n, const uint8_t *want, size_t want_n,
                         const char *what)
{
    if (got_n != want_n || memcmp(got, want, want_n) != 0) {
        fprintf(stderr, "%s: got %zu bytes:", what, got_n);
        for (size_t i = 0; i < got_n; ++i) {
            fprintf(stderr, " %02X", got[i]);
        }
        fprintf(stderr, "\n");
        assert(0);
    }
}

static void expect_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", what, got, want);
        assert(0);
    }
}

/* Healthy screen: NORMAL, every value present, real time. */
static oled_ui_in_t healthy(void)
{
    return (oled_ui_in_t){
        .show = OLED_SHOW_NORMAL,
        .co = OLED_CO_VALUE, .co_x10 = 5, .co_mean1h_x10 = 4,
        .row_seen = true, .row_fresh = true,
        .has_pm = true, .pm2_5 = 12,
        .has_th = true, .temp_x10 = 284, .rh_x10 = 621,
        .has_voc = true, .tvoc_ppb = 145, .eco2_ppm = 520,
        .time_valid = true, .hh = 14, .mm = 30, .ss = 5,
        .ml = OLED_ML_BELOW, .d2_valid = true, .d2_x100 = 123,
    };
}

static void test_commands(void)
{
    uint8_t buf[32];
    static const uint8_t SSD_UP[] = { 0x00, 0xAE, 0xA8, 0x3F, 0xDA, 0x12, 0x20, 0x02,
                                      0x8D, 0x14, 0xA1, 0xC8 };
    static const uint8_t SSD_ROT[] = { 0x00, 0xAE, 0xA8, 0x3F, 0xDA, 0x12, 0x20, 0x02,
                                       0x8D, 0x14, 0xA0, 0xC0 };
    static const uint8_t SH_UP[] = { 0x00, 0xAE, 0xA8, 0x3F, 0xA1, 0xC8 };
    expect_bytes(buf, oled_init_cmds(OLED_CHIP_SSD1306, false, buf, sizeof(buf)),
                 SSD_UP, sizeof(SSD_UP), "SSD1306 init");
    expect_bytes(buf, oled_init_cmds(OLED_CHIP_SSD1306, true, buf, sizeof(buf)),
                 SSD_ROT, sizeof(SSD_ROT), "SSD1306 init rotated");
    expect_bytes(buf, oled_init_cmds(OLED_CHIP_SH1106, false, buf, sizeof(buf)),
                 SH_UP, sizeof(SH_UP), "SH1106 init");
    /* no display-on in the list: it is sent after the RAM is cleared */
    for (size_t i = 0; i < sizeof(SSD_UP); ++i) {
        assert(SSD_UP[i] != OLED_CMD_DISPLAY_ON);
    }
    assert(oled_init_cmds(OLED_CHIP_SSD1306, false, buf, sizeof(SSD_UP) - 1U) == 0);
    assert(oled_init_cmds(OLED_CHIP_SSD1306, false, NULL, 32) == 0);

    uint8_t pos[4];
    static const uint8_t P_SSD[] = { 0x00, 0xB3, 0x00, 0x14 };     /* page 3, col 64 */
    static const uint8_t P_SH0[] = { 0x00, 0xB0, 0x02, 0x10 };     /* col 0 -> RAM 2 */
    static const uint8_t P_SH64[] = { 0x00, 0xB7, 0x02, 0x14 };    /* col 64 -> RAM 66 */
    static const uint8_t P_SSD127[] = { 0x00, 0xB0, 0x0F, 0x17 };  /* col 127 */
    expect_bytes(pos, oled_pos_cmds(OLED_CHIP_SSD1306, 3, 64, pos), P_SSD, 4, "pos SSD1306");
    expect_bytes(pos, oled_pos_cmds(OLED_CHIP_SH1106, 0, 0, pos), P_SH0, 4, "pos SH1106 0");
    expect_bytes(pos, oled_pos_cmds(OLED_CHIP_SH1106, 7, 64, pos), P_SH64, 4, "pos SH1106 64");
    expect_bytes(pos, oled_pos_cmds(OLED_CHIP_SSD1306, 0, 127, pos), P_SSD127, 4, "pos 127");
    assert(oled_pos_cmds(OLED_CHIP_SSD1306, 8, 0, pos) == 0);
    assert(oled_pos_cmds(OLED_CHIP_SSD1306, 0, 128, pos) == 0);
    assert(oled_col_offset(OLED_CHIP_SSD1306) == 0 && oled_col_offset(OLED_CHIP_SH1106) == 2);
    PASS("init / position commands for SSD1306 and SH1106");
}

static void test_text(void)
{
    static oled_fb_t fb;
    oled_fb_clear(&fb);
    oled_fb_text(&fb, 0, 0, "A", 1);
    static const uint8_t A[] = { 0x7E, 0x11, 0x11, 0x11, 0x7E, 0x00 };
    expect_bytes(fb.px[0], 6, A, 6, "glyph A");

    /* scale 2: '1' = 00 42 7F 40 00; 0x42 -> rows 1,6 -> 0x300C over two pages */
    oled_fb_clear(&fb);
    oled_fb_text(&fb, 0, 4, "1", 2);
    assert(fb.px[4][2] == 0x0C && fb.px[4][3] == 0x0C);
    assert(fb.px[5][2] == 0x30 && fb.px[5][3] == 0x30);
    assert(fb.px[4][4] == 0xFF && fb.px[5][4] == 0x3F);   /* 0x7F -> 14 rows */
    assert(fb.px[4][0] == 0 && fb.px[4][10] == 0 && fb.px[4][11] == 0);

    /* unknown character draws '?' */
    static oled_fb_t q;
    oled_fb_clear(&fb);
    oled_fb_clear(&q);
    oled_fb_text(&fb, 0, 0, "\x01", 1);
    oled_fb_text(&q, 0, 0, "?", 1);
    assert(memcmp(&fb, &q, sizeof(fb)) == 0);

    /* clipping: nothing past the right edge or the last page */
    oled_fb_clear(&fb);
    oled_fb_text(&fb, 124, 7, "WW", 2);
    oled_fb_text(&fb, 200, 0, "A", 1);
    oled_fb_text(&fb, 0, 8, "A", 1);
    for (unsigned x = 0; x < 124; ++x) {
        assert(fb.px[7][x] == 0);
    }
    PASS("font rendering, scale 2, '?' and clipping");
}

static void test_dirty(void)
{
    static oled_fb_t want;
    static oled_fb_t sent;
    oled_fb_clear(&want);
    oled_fb_clear(&sent);
    assert(oled_fb_next_dirty(&want, &sent, OLED_ALL_CHUNKS, 0) == -1);
    assert(oled_fb_next_dirty(&want, &sent, 0, 0) == 0);
    assert(oled_fb_next_dirty(&want, &sent, (uint16_t)~(1U << 9), 0) == 9);
    want.px[5][70] = 1;    /* page 5, right half = chunk 11 */
    assert(oled_fb_next_dirty(&want, &sent, OLED_ALL_CHUNKS, 0) == 11);
    assert(oled_fb_next_dirty(&want, &sent, OLED_ALL_CHUNKS, 11) == 11);
    assert(oled_fb_next_dirty(&want, &sent, OLED_ALL_CHUNKS, 12) == -1);
    unsigned page;
    unsigned col;
    oled_chunk_where(11, &page, &col);
    assert(page == 5 && col == 64);
    oled_chunk_where(0, &page, &col);
    assert(page == 0 && col == 0);
    oled_chunk_where(OLED_CHUNKS - 1U, &page, &col);
    assert(page == 7 && col == 64);
    PASS("dirty chunks: unsent, changed, start index");
}

static void check_lengths(const oled_ui_text_t *t, const char *what)
{
    if (strlen(t->big) > 8U) {
        fprintf(stderr, "%s: big \"%s\" longer than 8\n", what, t->big);
        assert(0);
    }
    for (unsigned i = 0; i < 6U; ++i) {
        if (strlen(t->line[i]) > OLED_TEXT_COLS) {
            fprintf(stderr, "%s: line %u \"%s\" longer than 21\n", what, i, t->line[i]);
            assert(0);
        }
    }
}

static void test_format(void)
{
    oled_ui_text_t t;
    oled_ui_in_t in = healthy();
    oled_ui_format(&in, &t);
    expect_str(t.big, "NORMAL", "big");
    expect_str(t.tag[0], "", "tag0");
    expect_str(t.line[0], "CO   0.5 ppm 1h   0.4", "co");
    expect_str(t.line[1], "PM2.5    12 ug/m3", "pm");
    expect_str(t.line[2], "T  28.4C  RH  62.1%", "th");
    expect_str(t.line[3], "VOC   145  eCO2   520", "voc");
    expect_str(t.line[4], "ML BELOW   D2    1.23", "ml");
    expect_str(t.line[5], "14:30:05 REC", "time");
    check_lengths(&t, "healthy");

    /* the word follows the io task, demo and mute are tags */
    static const struct { oled_show_t s; const char *w; } WORDS[] = {
        { OLED_SHOW_UNKNOWN, "--" }, { OLED_SHOW_WATCH, "WATCH" },
        { OLED_SHOW_FAULT, "FAULT" }, { OLED_SHOW_ML_ALARM, "ALARM" },
        { OLED_SHOW_CO_FAULT, "CO FAULT" }, { OLED_SHOW_CO_ALARM, "CO ALARM" },
    };
    for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); ++i) {
        in = healthy();
        in.show = WORDS[i].s;
        in.demo = true;
        in.muted = true;
        oled_ui_format(&in, &t);
        expect_str(t.big, WORDS[i].w, "show word");
        expect_str(t.tag[0], "DEMO", "demo tag");
        expect_str(t.tag[1], "MUTE", "mute tag");
    }

    /* missing values are "--", never 0 */
    in = healthy();
    in.has_pm = false;
    in.has_th = false;
    in.has_voc = false;
    in.co = OLED_CO_NO_VALUE;
    in.ml = OLED_ML_MISSING;
    in.marked = true;
    oled_ui_format(&in, &t);
    expect_str(t.line[0], "CO    -- ppm", "co no value");
    expect_str(t.line[1], "PM2.5    -- ug/m3", "pm missing");
    expect_str(t.line[2], "T    --C  RH    --%", "th missing");
    expect_str(t.line[3], "VOC    --  eCO2    --", "voc missing");
    expect_str(t.line[4], "ML  MISSING", "ml missing");
    expect_str(t.line[5], "14:30:05 REC  MARK", "mark");

    /* stale row: no old values, and the bottom line says why */
    in = healthy();
    in.row_fresh = false;
    oled_ui_format(&in, &t);
    expect_str(t.line[1], "PM2.5    -- ug/m3", "pm stale");
    expect_str(t.line[2], "T    --C  RH    --%", "th stale");
    expect_str(t.line[5], "SAMPLER STALE", "stale");
    in.row_seen = false;
    oled_ui_format(&in, &t);
    expect_str(t.line[5], "WAIT SAMPLER", "never");
    in = healthy();
    in.time_valid = false;
    oled_ui_format(&in, &t);
    expect_str(t.line[5], "NO TIME  not logging", "no time");

    /* CO states */
    static const struct { oled_co_t c; const char *w; } COS[] = {
        { OLED_CO_OFF, "CO  not built" }, { OLED_CO_NONE, "CO  --" },
        { OLED_CO_WARMUP, "CO  warm-up" }, { OLED_CO_FAULT, "CO  FAULT" },
    };
    for (size_t i = 0; i < sizeof(COS) / sizeof(COS[0]); ++i) {
        in = healthy();
        in.co = COS[i].c;
        oled_ui_format(&in, &t);
        expect_str(t.line[0], COS[i].w, "co state");
    }

    /* negative temperature, full-scale CO, huge D² */
    in = healthy();
    in.temp_x10 = -45;
    in.rh_x10 = 1000;
    in.co_x10 = 5000;
    in.co_mean1h_x10 = 4999;
    in.ml = OLED_ML_PERSISTENT;
    in.d2_x100 = 123456789L;
    in.pm2_5 = 65535;
    in.tvoc_ppb = 65000;
    in.eco2_ppm = 65000;
    oled_ui_format(&in, &t);
    expect_str(t.line[0], "CO 500.0 ppm 1h 499.9", "co full scale");
    expect_str(t.line[2], "T  -4.5C  RH 100.0%", "negative T");
    expect_str(t.line[4], "ML PERSIST D2   >9999", "d2 huge");
    check_lengths(&t, "extremes");
    in.d2_x100 = 999999L;
    oled_ui_format(&in, &t);
    expect_str(t.line[4], "ML PERSIST D2 9999.99", "d2 max");
    in.ml = OLED_ML_ABOVE;
    in.d2_valid = false;
    oled_ui_format(&in, &t);
    expect_str(t.line[4], "ML ABOVE   D2      --", "d2 invalid");
    in.ml = OLED_ML_MODEL_FAULT;
    oled_ui_format(&in, &t);
    expect_str(t.line[4], "ML  MODEL FAULT", "model fault");
    in.ml = OLED_ML_STALE;
    oled_ui_format(&in, &t);
    expect_str(t.line[4], "ML  stale", "ml stale");
    in.ml = OLED_ML_NONE;
    oled_ui_format(&in, &t);
    expect_str(t.line[4], "ML  --", "ml none");

    /* SYS FAULT replaces the VOC line with the reasons */
    in = healthy();
    in.show = OLED_SHOW_FAULT;
    in.fault_bits = OLED_FAULT_AHT21 | OLED_FAULT_PMS7003;
    oled_ui_format(&in, &t);
    expect_str(t.line[3], "F:AHT PMS", "fault reasons");
    in.fault_bits = 0x1F;
    oled_ui_format(&in, &t);
    expect_str(t.line[3], "F:AHT ENS PMS MDL STL", "all reasons");
    in.fault_bits = 0;    /* demo FAULT step: no real reason, keep VOC */
    oled_ui_format(&in, &t);
    expect_str(t.line[3], "VOC   145  eCO2   520", "fault without reasons");
    PASS("status screen text: values, missing, stale, CO/ML states, faults");
}

static void test_boot(void)
{
    oled_ui_lines_t b;
    oled_ui_boot_lines("ml3-v1+pm_th+b07dd6863436", &b);
    expect_str(b.line[0], "RESEARCH MONITOR ONLY", "boot 0");
    expect_str(b.line[2], "NOT A CO ALARM.", "boot 2");
    expect_str(b.line[4], "a UL 2034 / EN 50291", "boot 4");
    expect_str(b.line[7], "ML ml3-v1+pm_th+b07dd", "boot model cut");
    for (unsigned i = 0; i < OLED_TEXT_ROWS; ++i) {
        assert(strlen(b.line[i]) <= OLED_TEXT_COLS);
    }
    oled_ui_boot_lines(NULL, &b);
    expect_str(b.line[7], "ML ?", "boot no model");
    PASS("boot screen: CLAUDE.md 8 text + model version");
}

/* ASCII art of a frame buffer, for checking the font and layout by eye */
static void dump(FILE *f, const char *title, const oled_fb_t *fb)
{
    fprintf(f, "== %s\n+", title);
    for (unsigned x = 0; x < OLED_W; ++x) {
        fputc('-', f);
    }
    fputs("+\n", f);
    for (unsigned y = 0; y < OLED_H; ++y) {
        fputc('|', f);
        for (unsigned x = 0; x < OLED_W; ++x) {
            fputc(((fb->px[y / 8U][x] >> (y % 8U)) & 1U) ? '#' : ' ', f);
        }
        fputs("|\n", f);
    }
    fputc('+', f);
    for (unsigned x = 0; x < OLED_W; ++x) {
        fputc('-', f);
    }
    fputs("+\n\n", f);
}

static void dump_screens(const char *path)
{
    FILE *f = fopen(path, "w");
    assert(f != NULL);
    static oled_fb_t fb;
    oled_fb_test_pattern(&fb);
    dump(f, "test pattern", &fb);

    oled_ui_lines_t boot;
    oled_ui_boot_lines("ml3-v1+pm_th+b07dd6863436", &boot);
    oled_ui_draw_lines(&boot, &fb);
    dump(f, "boot", &fb);

    oled_ui_text_t t;
    oled_ui_in_t in = healthy();
    oled_ui_format(&in, &t);
    oled_ui_draw(&t, &fb);
    dump(f, "normal", &fb);

    in.show = OLED_SHOW_CO_ALARM;
    in.muted = true;
    in.co_x10 = 1234;
    in.co_mean1h_x10 = 456;
    oled_ui_format(&in, &t);
    oled_ui_draw(&t, &fb);
    dump(f, "CO alarm, muted", &fb);

    in = healthy();
    in.show = OLED_SHOW_FAULT;
    in.fault_bits = 0x1F;
    in.demo = true;
    in.time_valid = false;
    oled_ui_format(&in, &t);
    oled_ui_draw(&t, &fb);
    dump(f, "fault, demo, no time", &fb);

    static oled_fb_t all;
    oled_fb_clear(&all);
    char row[OLED_LINE_CAP];
    for (unsigned r = 0; r < 5U; ++r) {
        memset(row, 0, sizeof(row));
        for (unsigned i = 0; i < OLED_TEXT_COLS; ++i) {
            const unsigned c = 0x20U + r * OLED_TEXT_COLS + i;
            row[i] = c <= 0x7EU ? (char)c : ' ';
        }
        oled_fb_text(&all, 0, r, row, 1);
    }
    dump(f, "font 0x20-0x7E", &all);
    fclose(f);
    printf("PASS screens written to %s\n", path);
}

int main(int argc, char **argv)
{
    test_commands();
    test_text();
    test_dirty();
    test_format();
    test_boot();
    dump_screens(argc > 1 ? argv[1] : "oled_screens.txt");
    puts("ALL oled_ui tests passed");
    return 0;
}
