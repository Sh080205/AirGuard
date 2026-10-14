#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ens160_parse.h"

/* Reference MISR built from the polynomial exponents in DS §16.2.14
 * (x^8+x^4+x^3+x^2+x^0), not from the shift/XOR code: in GF(2)[x],
 * misr' = (x * misr + data) mod G. x*misr has degree <= 8 and data < x^8,
 * so one conditional subtraction (XOR) of G is the full reduction. */
static uint8_t ref_misr(uint8_t misr, uint8_t data)
{
    static const unsigned exps[] = {8, 4, 3, 2, 0};
    unsigned g = 0;
    for (size_t i = 0; i < sizeof(exps) / sizeof(exps[0]); ++i) {
        g |= 1u << exps[i];
    }
    unsigned v = ((unsigned)misr << 1) ^ data;
    if (v & 0x100u) {
        v ^= g;
    }
    return (uint8_t)v;
}

static void make_raw(uint8_t raw[ENS160_DATA_LEN], uint8_t status, uint8_t aqi,
                     uint16_t tvoc, uint16_t eco2)
{
    raw[0] = status;
    raw[1] = aqi;
    raw[2] = (uint8_t)tvoc;
    raw[3] = (uint8_t)(tvoc >> 8);
    raw[4] = (uint8_t)eco2;
    raw[5] = (uint8_t)(eco2 >> 8);
}

/* STATAS + validity + NEWDAT */
static uint8_t status_of(bool running, bool error, unsigned validity, bool newdat)
{
    return (uint8_t)((running ? ENS160_STATUS_STATAS : 0) | (error ? ENS160_STATUS_STATER : 0) |
                     ((validity & 3u) << ENS160_STATUS_VALIDITY_SHIFT) |
                     (newdat ? ENS160_STATUS_NEWDAT : 0));
}

static ens160_data_t data_of(uint8_t status, uint16_t tvoc, uint16_t eco2)
{
    uint8_t raw[ENS160_DATA_LEN];
    make_raw(raw, status, 1, tvoc, eco2);
    ens160_data_t d;
    assert(ens160_parse_data(raw, &d) == ESP_OK);
    return d;
}

static void test_part_id(void)
{
    /* DS Table 17: address 0x00 = 0x60 (LSB), 0x01 = 0x01 (MSB) */
    const uint8_t ok[2] = {0x60, 0x01};
    const uint8_t swapped[2] = {0x01, 0x60};
    assert(ens160_u16le(ok) == ENS160_PART_ID);
    assert(ens160_u16le(swapped) != ENS160_PART_ID);
    printf("PASS PART_ID 0x60,0x01 -> 0x0160 (little endian)\n");
}

static void test_parse(void)
{
    uint8_t raw[ENS160_DATA_LEN];
    /* every status bit set, AQI byte with reserved bits set, LE values */
    make_raw(raw, 0xFF, 0xFD, 0x1234, 0xABCD);
    ens160_data_t d;
    memset(&d, 0, sizeof(d));
    assert(ens160_parse_data(raw, &d) == ESP_OK);
    assert(d.status == 0xFF && d.running && d.error && d.new_data && d.validity == 3);
    assert(d.aqi == 5);   /* 0xFD & 0x07: bits 3:7 reserved, dropped */
    assert(d.tvoc_ppb == 0x1234 && d.eco2_ppm == 0xABCD);

    make_raw(raw, 0x00, 0x01, 0, 400);
    assert(ens160_parse_data(raw, &d) == ESP_OK);
    assert(!d.running && !d.error && !d.new_data && d.validity == 0 && d.aqi == 1);

    for (unsigned v = 0; v < 4; ++v) {
        make_raw(raw, status_of(true, false, v, false), 1, 0, 400);
        assert(ens160_parse_data(raw, &d) == ESP_OK && d.validity == v);
    }
    /* NEWGPR (bit 0) and reserved bits 4:5 must not leak into other fields */
    make_raw(raw, 0x31, 1, 0, 400);
    assert(ens160_parse_data(raw, &d) == ESP_OK);
    assert(!d.running && !d.error && !d.new_data && d.validity == 0);

    ens160_data_t keep;
    memset(&keep, 0x5A, sizeof(keep));
    ens160_data_t copy = keep;
    assert(ens160_parse_data(NULL, &keep) == ESP_ERR_INVALID_ARG);
    assert(memcmp(&keep, &copy, sizeof(keep)) == 0);
    assert(ens160_parse_data(raw, NULL) == ESP_ERR_INVALID_ARG);
    printf("PASS parse: status bits, validity 0..3, AQI bits 0:2, LE TVOC/eCO2, NULL\n");
}

static void test_verdict(void)
{
    const uint8_t ok = status_of(true, false, 0, true);
    assert(ens160_verdict(&(ens160_data_t){0}) == ENS160_VERDICT_NOT_RUNNING);
    ens160_data_t d = data_of(ok, 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_OK);
    /* NEWDAT does not change the verdict */
    d = data_of(status_of(true, false, 0, false), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_OK);

    d = data_of(status_of(true, false, 1, true), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_WARMUP);
    d = data_of(status_of(true, false, 2, true), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_STARTUP);
    d = data_of(status_of(true, false, 3, true), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_INVALID);
    d = data_of(status_of(false, false, 0, true), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_NOT_RUNNING);
    /* STATER wins over everything else */
    d = data_of(status_of(true, true, 0, true), 100, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_ERROR);
    d = data_of(status_of(false, true, 1, true), 70000 & 0xFFFF, 0);
    assert(ens160_verdict(&d) == ENS160_VERDICT_ERROR);
    assert(ens160_verdict(NULL) == ENS160_VERDICT_ERROR);
    printf("PASS verdict: STATER > STATAS > validity 1/2/3 > range\n");

    /* DS Table 4: TVOC 0..65000 ppb, eCO2 400..65000 ppm (edges inclusive) */
    d = data_of(ok, 0, 400);
    assert(ens160_verdict(&d) == ENS160_VERDICT_OK);
    d = data_of(ok, 65000, 65000);
    assert(ens160_verdict(&d) == ENS160_VERDICT_OK);
    d = data_of(ok, 65001, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_RANGE);
    d = data_of(ok, 0xFFFF, 500);
    assert(ens160_verdict(&d) == ENS160_VERDICT_RANGE);
    d = data_of(ok, 100, 399);
    assert(ens160_verdict(&d) == ENS160_VERDICT_RANGE);
    d = data_of(ok, 100, 0);    /* register default 0x0000 before any data */
    assert(ens160_verdict(&d) == ENS160_VERDICT_RANGE);
    d = data_of(ok, 100, 65001);
    assert(ens160_verdict(&d) == ENS160_VERDICT_RANGE);
    printf("PASS range edges: TVOC 0/65000 ok 65001/65535 bad, eCO2 400/65000 ok 0/399/65001 bad\n");
}

static void test_encode(void)
{
    uint16_t v = 0;
    /* DS §16.2.5 example: 25 °C -> 0x4A8A; DS §16.2.6 example: 50 % -> 0x6400 */
    assert(ens160_encode_temp(25.0f, &v) == ESP_OK && v == 0x4A8A);
    assert(ens160_encode_rh(50.0f, &v) == ESP_OK && v == 0x6400);
    /* DS §16.2.12-13 examples back the other way */
    assert(fabsf(ens160_decode_temp(0x4A8A) - 25.0f) < 0.01f);
    assert(ens160_decode_rh(0x6400) == 50.0f);

    /* edges of the accepted range */
    assert(ens160_encode_temp(-40.0f, &v) == ESP_OK && v == 14922);   /* 233.15*64=14921.6 */
    assert(ens160_encode_temp(85.0f, &v) == ESP_OK && v == 22922);    /* 358.15*64=22921.6 */
    assert(ens160_encode_rh(0.0f, &v) == ESP_OK && v == 0);
    assert(ens160_encode_rh(100.0f, &v) == ESP_OK && v == 51200);
    /* typical AHT21 value: 28.4 °C -> 301.55*64 = 19299.2 -> 19299 */
    assert(ens160_encode_temp(28.4f, &v) == ESP_OK && v == 19299);
    /* 62.1 % -> 31795.2 -> 31795 */
    assert(ens160_encode_rh(62.1f, &v) == ESP_OK && v == 31795);

    /* rejected inputs leave *out untouched */
    const float bad_t[] = {-40.1f, 85.1f, NAN, INFINITY, -INFINITY};
    for (size_t i = 0; i < sizeof(bad_t) / sizeof(bad_t[0]); ++i) {
        v = 0xBEEF;
        assert(ens160_encode_temp(bad_t[i], &v) == ESP_ERR_INVALID_ARG && v == 0xBEEF);
    }
    const float bad_rh[] = {-0.1f, 100.1f, NAN, INFINITY};
    for (size_t i = 0; i < sizeof(bad_rh) / sizeof(bad_rh[0]); ++i) {
        v = 0xBEEF;
        assert(ens160_encode_rh(bad_rh[i], &v) == ESP_ERR_INVALID_ARG && v == 0xBEEF);
    }
    assert(ens160_encode_temp(25.0f, NULL) == ESP_ERR_INVALID_ARG);
    assert(ens160_encode_rh(50.0f, NULL) == ESP_ERR_INVALID_ARG);
    printf("PASS TEMP_IN/RH_IN: DS examples 0x4A8A/0x6400, edges, NaN/inf rejected\n");
}

static void test_misr(void)
{
    /* every (misr, data) pair against the polynomial reference */
    for (unsigned m = 0; m < 256; ++m) {
        for (unsigned b = 0; b < 256; ++b) {
            assert(ens160_misr_update((uint8_t)m, (uint8_t)b) == ref_misr((uint8_t)m, (uint8_t)b));
        }
    }
    /* hand-worked: 0 -> 0x80 (no reduction), then 0x80 << 1 overflows -> POLY */
    assert(ens160_misr_update(0x00, 0x80) == 0x80);
    assert(ens160_misr_update(0x80, 0x00) == 0x1D);
    const uint8_t seq[] = {0x80, 0x00};
    assert(ens160_misr_bytes(0x00, seq, 2) == 0x1D);
    assert(ens160_misr_bytes(0x42, NULL, 3) == 0x42);
    assert(ens160_misr_bytes(0x42, seq, 0) == 0x42);
    /* any single-bit error in a 6-byte burst changes the checksum */
    uint8_t raw[ENS160_DATA_LEN];
    make_raw(raw, status_of(true, false, 0, true), 1, 123, 456);
    const uint8_t good = ens160_misr_bytes(0x5C, raw, sizeof(raw));
    for (size_t bit = 0; bit < sizeof(raw) * 8; ++bit) {
        raw[bit / 8] ^= (uint8_t)(1u << (bit % 8));
        assert(ens160_misr_bytes(0x5C, raw, sizeof(raw)) != good);
        raw[bit / 8] ^= (uint8_t)(1u << (bit % 8));
    }
    printf("PASS MISR: 65536 pairs match x^8+x^4+x^3+x^2+1 reference, 48 single-bit errors detected\n");
}

static void test_hold(void)
{
    const int64_t age = ENS160_MAX_AGE_US;
    const uint8_t fresh = status_of(true, false, 0, true);
    const uint8_t same = status_of(true, false, 0, false);
    ens160_hold_t h = {0};
    uint16_t tv = 0xBEEF;
    uint16_t co = 0xBEEF;
    ens160_data_t d;

    /* warm-up: empty, outputs untouched */
    d = data_of(status_of(true, false, 1, true), 0, 0);
    assert(ens160_hold_step(&h, true, &d, 0, age, &tv, &co) == ENS160_FILL_EMPTY);
    assert(tv == 0xBEEF && co == 0xBEEF && !h.have);
    /* valid but no NEWDAT yet and nothing held: empty */
    d = data_of(same, 0, 0);
    assert(ens160_hold_step(&h, true, &d, 1000000, age, &tv, &co) == ENS160_FILL_EMPTY);

    /* NEWDAT at t=2 s */
    d = data_of(fresh, 150, 520);
    assert(ens160_hold_step(&h, true, &d, 2000000, age, &tv, &co) == ENS160_FILL_NEW);
    assert(tv == 150 && co == 520);
    /* no NEWDAT at 3 s and 4 s: reused (registers may already hold other
     * values; the held reading is what is written) */
    d = data_of(same, 999, 999);
    tv = co = 0;
    assert(ens160_hold_step(&h, true, &d, 3000000, age, &tv, &co) == ENS160_FILL_REUSED);
    assert(tv == 150 && co == 520);
    assert(ens160_hold_step(&h, true, &d, 2000000 + age, age, &tv, &co) == ENS160_FILL_REUSED);
    /* 1 us past the bound: empty */
    tv = co = 0xBEEF;
    assert(ens160_hold_step(&h, true, &d, 2000000 + age + 1, age, &tv, &co) == ENS160_FILL_EMPTY);
    assert(tv == 0xBEEF && co == 0xBEEF);
    printf("PASS hold: NEWDAT new, reused up to %d ms inclusive, empty after\n", ENS160_MAX_AGE_MS);

    /* read failure (I2C / MISR) keeps the held reading, age-limited */
    d = data_of(fresh, 200, 600);
    assert(ens160_hold_step(&h, true, &d, 10000000, age, &tv, &co) == ENS160_FILL_NEW);
    assert(ens160_hold_step(&h, false, NULL, 11000000, age, &tv, &co) == ENS160_FILL_REUSED);
    assert(tv == 200 && co == 600);
    assert(ens160_hold_step(&h, false, &d, 13000000, age, &tv, &co) == ENS160_FILL_EMPTY);
    assert(h.have);   /* still held, just too old */
    /* clock "from the future" is never fresh */
    assert(ens160_hold_step(&h, false, NULL, 9000000, age, &tv, &co) == ENS160_FILL_EMPTY);

    /* any non-OK verdict drops the held reading at once */
    const uint8_t bad[] = {
        status_of(true, false, 1, false), status_of(true, false, 2, false),
        status_of(true, false, 3, false), status_of(false, false, 0, false),
        status_of(true, true, 0, false),
    };
    for (size_t i = 0; i < sizeof(bad); ++i) {
        d = data_of(fresh, 300, 700);
        assert(ens160_hold_step(&h, true, &d, 20000000, age, &tv, &co) == ENS160_FILL_NEW);
        d = data_of(bad[i], 300, 700);
        assert(ens160_hold_step(&h, true, &d, 20500000, age, &tv, &co) == ENS160_FILL_EMPTY);
        assert(!h.have);
        /* back to valid without NEWDAT: still empty (no pre-warm-up value) */
        d = data_of(same, 300, 700);
        assert(ens160_hold_step(&h, true, &d, 21000000, age, &tv, &co) == ENS160_FILL_EMPTY);
    }
    /* out-of-range NEWDAT reading is not stored */
    d = data_of(fresh, 300, 700);
    assert(ens160_hold_step(&h, true, &d, 30000000, age, &tv, &co) == ENS160_FILL_NEW);
    d = data_of(fresh, 300, 399);
    assert(ens160_hold_step(&h, true, &d, 31000000, age, &tv, &co) == ENS160_FILL_EMPTY);
    assert(!h.have);
    /* NULL guards */
    assert(ens160_hold_step(NULL, true, &d, 0, age, &tv, &co) == ENS160_FILL_EMPTY);
    assert(ens160_hold_step(&h, true, &d, 0, age, NULL, &co) == ENS160_FILL_EMPTY);
    assert(ens160_hold_step(&h, true, NULL, 0, age, &tv, &co) == ENS160_FILL_EMPTY);
    printf("PASS hold: read failure keeps value (age-limited); warm-up/start-up/invalid/"
           "not running/STATER/range drop it\n");
}

int main(void)
{
    test_part_id();
    test_parse();
    test_verdict();
    test_encode();
    test_misr();
    test_hold();
    printf("ALL ens160_parse tests passed\n");
    return 0;
}
