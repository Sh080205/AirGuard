#ifndef TEST_FRAMES_H
#define TEST_FRAMES_H

#include <assert.h>
#include <string.h>
#include "pms7003.h"

/* Independent fixture encoder: manual V2.5 Appendix I (pp. 13-14),
 * 42 4D + length (28) + 13 big-endian words + additive checksum.
 * All data values below are synthetic byte-order/boundary test inputs,
 * not sensor measurements or limits. No production checksum helper is used.
 */
static const uint16_t sample[13] = {
    0x0123, 0x4567, 0x89ab, 0xcdef, 0x0000, 0xffff,
    0x1032, 0x5476, 0x98ba, 0xdcfe, 0x424d, 0x8142, 0x1357
};

static void fix_checksum(uint8_t frame[32])
{
    unsigned sum = 0;
    for (size_t i = 0; i < 30; ++i) {
        sum += frame[i];
    }
    frame[30] = (uint8_t)(sum / 256U);
    frame[31] = (uint8_t)(sum % 256U);
}

static void make_frame(uint8_t frame[32], const uint16_t values[13])
{
    frame[0] = 0x42;
    frame[1] = 0x4d;
    frame[2] = 0;
    frame[3] = 28;
    for (size_t i = 0; i < 13; ++i) {
        frame[4 + 2 * i] = (uint8_t)(values[i] / 256U);
        frame[5 + 2 * i] = (uint8_t)(values[i] % 256U);
    }
    fix_checksum(frame);
}

static void check_data(const pms7003_data_t *out, const uint16_t values[13])
{
    assert(out->pm1_0_cf1 == values[0]);
    assert(out->pm2_5_cf1 == values[1]);
    assert(out->pm10_cf1 == values[2]);
    assert(out->pm1_0_atmospheric == values[3]);
    assert(out->pm2_5_atmospheric == values[4]);
    assert(out->pm10_atmospheric == values[5]);
}
#endif
