#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "aht21_parse.h"

/* Reference CRC built independently of aht21_parse.c: plain polynomial long
 * division over GF(2), with the generator assembled from the exponents the
 * datasheet prints in §7.4 step 4 (1+x^4+x^5+x^8). The initial value 0xFF
 * (§7.4) is the same as inverting the first 8 message bits. Remainder of
 * M(x)*x^8 mod G(x) is the CRC. */
static uint8_t ref_crc8(const uint8_t *data, size_t len)
{
    static const unsigned exps[] = {8, 5, 4, 0};
    unsigned gen = 0;
    for (size_t i = 0; i < sizeof(exps) / sizeof(exps[0]); ++i) {
        gen |= 1u << exps[i];
    }
    unsigned rem = 0; /* up to 9 bits */
    const size_t nbits = len * 8 + 8;
    for (size_t b = 0; b < nbits; ++b) {
        unsigned bit = 0;
        if (b < len * 8) {
            bit = (data[b / 8] >> (7 - b % 8)) & 1u;
            if (b < 8) {
                bit ^= 1u;
            }
        }
        rem = (rem << 1) | bit;
        if (rem & 0x100u) {
            rem ^= gen;
        }
    }
    return (uint8_t)rem;
}

/* Datasheet p. 13 layout: state, RH[19:12], RH[11:4], RH[3:0]|T[19:16],
 * T[15:8], T[7:0], CRC. */
static void make_raw(uint8_t raw[7], uint8_t state, uint32_t s_rh, uint32_t s_t)
{
    assert(s_rh < (1u << 20) && s_t < (1u << 20));
    raw[0] = state;
    raw[1] = (uint8_t)(s_rh >> 12);
    raw[2] = (uint8_t)(s_rh >> 4);
    raw[3] = (uint8_t)(((s_rh & 0xfu) << 4) | (s_t >> 16));
    raw[4] = (uint8_t)(s_t >> 8);
    raw[5] = (uint8_t)s_t;
    raw[6] = ref_crc8(raw, 6);
}

static void expect_ok(uint32_t s_rh, uint32_t s_t, float rh, float t)
{
    uint8_t raw[7];
    make_raw(raw, 0x18, s_rh, s_t);
    float temp = -999.0f;
    float hum = -999.0f;
    assert(aht21_parse(raw, &temp, &hum) == ESP_OK);
    if (temp != t || hum != rh) {
        fprintf(stderr, "S_RH=0x%05lx S_T=0x%05lx got T=%.6f RH=%.6f "
                "want T=%.6f RH=%.6f\n", (unsigned long)s_rh,
                (unsigned long)s_t, temp, hum, t, rh);
        assert(0);
    }
}

static void expect_error(const uint8_t raw[7], esp_err_t expected)
{
    float temp = 123.25f;
    float hum = 45.5f;
    assert(aht21_parse(raw, &temp, &hum) == expected);
    assert(temp == 123.25f && hum == 45.5f);
}

int main(void)
{
    /* The reference and the driver CRC must agree on many inputs, and data
     * followed by its CRC must leave a zero remainder (no final XOR). */
    uint8_t msg[7];
    for (unsigned seed = 0; seed < 4096; ++seed) {
        for (size_t i = 0; i < 6; ++i) {
            msg[i] = (uint8_t)(seed * 37u + i * 101u + (seed >> 3) * i);
        }
        assert(aht21_crc8(msg, 6) == ref_crc8(msg, 6));
        msg[6] = aht21_crc8(msg, 6);
        assert(ref_crc8(msg, 7) == 0);
    }
    assert(aht21_crc8(msg, 0) == 0xff);
    puts("PASS CRC8 matches long-division reference (4096 inputs)");

    /* §8.2: T = S_T / 2^20 * 200 - 50.  §8.1: RH = S_RH / 2^20 * 100. */
    expect_ok(0, 0, 0.0f, -50.0f);
    expect_ok(1u << 19, 1u << 19, 50.0f, 50.0f);
    expect_ok(1u << 18, 3u << 18, 25.0f, 100.0f);
    puts("PASS S_T=0 -> -50.0 C, S_T=2^19 -> 50.0 C, S_RH=2^19 -> 50.0 %");

    /* Distinct nibbles catch swapped or shifted fields at the shared byte. */
    const uint32_t s_rh = 0xabcde, s_t = 0x12345;
    uint8_t raw[7];
    make_raw(raw, 0x1c, s_rh, s_t);
    assert(raw[3] == 0xe1);
    float temp, hum;
    assert(aht21_parse(raw, &temp, &hum) == ESP_OK);
    assert(fabs(hum - (double)s_rh / 1048576.0 * 100.0) < 1e-4);
    assert(fabs(temp - ((double)s_t / 1048576.0 * 200.0 - 50.0)) < 1e-4);
    make_raw(raw, 0x18, 0xfffff, 0xfffff);
    assert(aht21_parse(raw, &temp, &hum) == ESP_OK);
    assert(hum < 100.0f && hum > 99.999f);
    assert(temp < 150.0f && temp > 149.999f);
    puts("PASS 20-bit fields split at byte 3 nibbles, full-scale values");

    make_raw(raw, 0x18, 0x6b851, 0x5c28f);
    for (size_t i = 0; i < 7; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            raw[i] ^= (uint8_t)(1u << bit);
            expect_error(raw, ESP_ERR_INVALID_CRC);
            raw[i] ^= (uint8_t)(1u << bit);
        }
    }
    puts("PASS every single-bit error (56) -> ESP_ERR_INVALID_CRC, outputs kept");

    make_raw(raw, 0x98, 0x6b851, 0x5c28f); /* bit7 busy, valid CRC */
    expect_error(raw, ESP_ERR_INVALID_STATE);
    make_raw(raw, 0x80, 0, 0);
    expect_error(raw, ESP_ERR_INVALID_STATE);
    puts("PASS busy state (bit7) -> ESP_ERR_INVALID_STATE, outputs kept");

    make_raw(raw, 0x18, 0, 0);
    assert(aht21_parse(NULL, &temp, &hum) == ESP_ERR_INVALID_ARG);
    assert(aht21_parse(raw, NULL, &hum) == ESP_ERR_INVALID_ARG);
    assert(aht21_parse(raw, &temp, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS NULL arguments -> ESP_ERR_INVALID_ARG");

    assert(!aht21_needs_init(0x18));
    assert(aht21_needs_init(0x08));
    assert(!aht21_needs_init(0x1c));
    assert(aht21_needs_init(0x10));
    assert(aht21_needs_init(0x00));
    assert(!aht21_needs_init(0xff));
    puts("PASS aht21_needs_init 0x18/0x08/0x1C/0x10/0x00/0xFF");

    puts("ALL aht21_parse tests passed");
    return 0;
}
