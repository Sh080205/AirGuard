/* OLED status screen for the slides: the real oled_ui.c draws sample values
 * (the same as the dashboard sample) and the frame buffer is dumped as PGM.
 * Build from firmware/: gcc -std=c11 -I test_host/stubs -I components/oled_ui/include
 *   ../docs/slides/tools/gen_oled.c components/oled_ui/oled_ui.c components/oled_ui/oled_font.c */
#include <stdio.h>
#include <string.h>
#include "oled_ui.h"

int main(void)
{
    oled_ui_in_t in;
    memset(&in, 0, sizeof in);
    in.show = OLED_SHOW_NORMAL;
    in.co = OLED_CO_VALUE; in.co_x10 = 5; in.co_mean1h_x10 = 5;
    in.row_seen = true; in.row_fresh = true;
    in.has_pm = true; in.pm2_5 = 19;
    in.has_th = true; in.temp_x10 = 328; in.rh_x10 = 564;
    in.has_voc = true; in.tvoc_ppb = 213; in.eco2_ppm = 708;
    in.time_valid = true; in.hh = 21; in.mm = 9; in.ss = 59;
    in.ml = OLED_ML_BELOW; in.d2_valid = true; in.d2_x100 = 71;
    oled_ui_text_t t;
    oled_fb_t fb;
    oled_ui_format(&in, &t);
    oled_fb_clear(&fb);
    oled_ui_draw(&t, &fb);
    printf("P2\n%u %u\n1\n", OLED_W, OLED_H);
    for (unsigned y = 0; y < OLED_H; ++y) {
        for (unsigned x = 0; x < OLED_W; ++x) printf("%d ", (fb.px[y / 8][x] >> (y % 8)) & 1);
        printf("\n");
    }
    for (unsigned i = 0; i < 2; ++i) fprintf(stderr, "big=%s tag=%s\n", t.big, t.tag[i]);
    for (unsigned i = 0; i < 6; ++i) fprintf(stderr, "%s\n", t.line[i]);
    return 0;
}
