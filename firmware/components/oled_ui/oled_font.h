#pragma once

#include <stdint.h>

#define OLED_FONT_FIRST  0x20U
#define OLED_FONT_LAST   0x7EU
#define OLED_FONT_GLYPHS (OLED_FONT_LAST - OLED_FONT_FIRST + 1U)
#define OLED_FONT_W      5U

extern const uint8_t oled_font5x7[OLED_FONT_GLYPHS][OLED_FONT_W];
