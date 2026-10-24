/* FW-5b step 2: ZE07-CO frame parser (manual V1.7, see ze07.h). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "csv_row.h"
#include "ze07.h"

/* Example frames typed by hand from manual V1.7, not computed here:
 * p.4 Table 4 (initiative upload) and p.5 Tables 5-8 (commands/answer).
 * Only Table 4 is a frame the parser must accept; Tables 5-8 check that
 * ze07_checksum() reproduces every checksum the manual prints. */
static const uint8_t TABLE4_UPLOAD[9] = {0xFF, 0x04, 0x03, 0x01, 0x00, 0x25, 0x13, 0x88, 0x38};
static const uint8_t TABLE5_TO_QA[9]  = {0xFF, 0x01, 0x78, 0x41, 0x00, 0x00, 0x00, 0x00, 0x46};
static const uint8_t TABLE6_TO_UP[9]  = {0xFF, 0x01, 0x78, 0x40, 0x00, 0x00, 0x00, 0x00, 0x47};
static const uint8_t TABLE7_QUERY[9]  = {0xFF, 0x01, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79};
static const uint8_t TABLE8_ANSWER[9] = {0xFF, 0x86, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x3A};

/* Independent reference: sum bytes 1..7 in a wide int, then 256 - sum
 * mod 256 ("Not(sum) + 1" in two's complement). */
static uint8_t ref_checksum(const uint8_t *f)
{
    unsigned sum = 0;
    for (int i = 1; i <= 7; ++i) {
        sum += f[i];
    }
    return (uint8_t)((256U - (sum % 256U)) % 256U);
}

static void make_frame(uint8_t f[9], uint16_t conc_x10)
{
    memcpy(f, TABLE4_UPLOAD, 9);
    f[4] = (uint8_t)(conc_x10 >> 8);
    f[5] = (uint8_t)conc_x10;
    f[8] = ref_checksum(f);
}

static void expect_error(const uint8_t *buf, size_t len, esp_err_t expected,
                         size_t expected_consumed)
{
    ze07_reading_t out = {.conc_x10 = 0xA5A5};
    size_t consumed = SIZE_MAX;
    assert(ze07_parse(buf, len, &out, &consumed) == expected);
    assert(consumed == expected_consumed);
    assert(out.conc_x10 == 0xA5A5);   /* out unchanged on failure */
}

static void expect_ok(const uint8_t *buf, size_t len, uint16_t conc_x10,
                      size_t expected_consumed)
{
    ze07_reading_t out = {.conc_x10 = 0xA5A5};
    size_t consumed = SIZE_MAX;
    assert(ze07_parse(buf, len, &out, &consumed) == ESP_OK);
    assert(consumed == expected_consumed);
    assert(out.conc_x10 == conc_x10);
}

/* Stream helper: append bytes to a buffer in chunks of `chunk`, call the
 * parser after each chunk until it stops returning frames, drop consumed
 * bytes like the UART task will. Returns the number of frames found and
 * stores their values. */
static size_t feed_stream(const uint8_t *stream, size_t n, size_t chunk,
                          uint16_t *values, size_t max_values)
{
    uint8_t buf[64];
    size_t used = 0, found = 0, pos = 0;
    while (pos < n) {
        size_t take = n - pos < chunk ? n - pos : chunk;
        assert(used + take <= sizeof(buf));
        memcpy(buf + used, stream + pos, take);
        used += take;
        pos += take;
        for (;;) {
            ze07_reading_t r;
            size_t consumed = 0;
            const esp_err_t err = ze07_parse(buf, used, &r, &consumed);
            assert(consumed <= used);
            memmove(buf, buf + consumed, used - consumed);
            used -= consumed;
            if (err == ESP_OK) {
                assert(found < max_values);
                values[found++] = r.conc_x10;
                continue;
            }
            if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_FOUND) {
                break;   /* need more bytes */
            }
            assert(err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_RESPONSE);
            assert(consumed >= 1);   /* always progress after a bad frame */
        }
    }
    return found;
}

static uint32_t lcg_state = 12345U;   /* fixed seed: reproducible */
static uint8_t lcg_byte(void)
{
    lcg_state = lcg_state * 1103515245U + 12345U;
    return (uint8_t)(lcg_state >> 16);
}

int main(void)
{
    /* 1. Checksum reproduces every example printed in the manual. */
    const uint8_t *examples[] = {TABLE4_UPLOAD, TABLE5_TO_QA, TABLE6_TO_UP,
                                 TABLE7_QUERY, TABLE8_ANSWER};
    for (size_t i = 0; i < 5; ++i) {
        assert(ze07_checksum(examples[i]) == examples[i][8]);
        assert(ref_checksum(examples[i]) == examples[i][8]);
    }
    puts("PASS checksum matches manual Tables 4-8 (5/5)");

    /* 2. Checksum against the reference for many byte patterns. */
    uint8_t f[9];
    for (unsigned a = 0; a < 256; ++a) {
        for (unsigned b = 0; b < 256; b += 3) {
            memcpy(f, TABLE4_UPLOAD, 9);
            f[1] = (uint8_t)a; f[5] = (uint8_t)b; f[7] = (uint8_t)(a ^ b);
            assert(ze07_checksum(f) == ref_checksum(f));
        }
    }
    puts("PASS checksum equals independent reference (22,016 patterns)");

    /* 3. Table 4 example -> 3.7 ppm. */
    expect_ok(TABLE4_UPLOAD, 9, 37, 9);
    puts("PASS Table 4 example frame = 37 x 0.1 ppm = 3.7 ppm");

    /* 4. Edge concentrations, including 0xFF data bytes and values at and
     * above full scale (accepted, not clipped). */
    const uint16_t edges[] = {0, 1, 9, 10, 255, 256, 899, 900, 4999, 5000,
                              5001, 0x00FF, 0xFF00, 0xFFFF};
    for (size_t i = 0; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        make_frame(f, edges[i]);
        expect_ok(f, 9, edges[i], 9);
    }
    puts("PASS edge values 0..0xFFFF incl. 0xFF data bytes and >= full scale");

    /* 5. Every single-bit flip in bytes 1..8 breaks the checksum. */
    memcpy(f, TABLE4_UPLOAD, 9);
    for (size_t i = 1; i < 9; ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            f[i] ^= (uint8_t)(1U << bit);
            expect_error(f, 9, ESP_ERR_INVALID_CRC, 1);
            f[i] ^= (uint8_t)(1U << bit);
        }
    }
    puts("PASS single-bit corruption (64 cases) -> INVALID_CRC, out unchanged");

    /* 6. Correct checksum but fields not as in Table 4 -> rejected. */
    struct { size_t off; uint8_t val; } bad_fields[] = {
        {1, 0x05}, {1, 0x01}, {2, 0x02}, {2, 0x04}, {3, 0x00}, {3, 0x02},
        {6, 0x13 ^ 0x01}, {7, 0x89}, {7, 0x87},
    };
    for (size_t i = 0; i < sizeof(bad_fields) / sizeof(bad_fields[0]); ++i) {
        memcpy(f, TABLE4_UPLOAD, 9);
        f[bad_fields[i].off] = bad_fields[i].val;
        f[8] = ref_checksum(f);
        expect_error(f, 9, ESP_ERR_INVALID_RESPONSE, 1);
    }
    /* Q&A answer (Table 8) has a valid checksum but is not an upload frame */
    expect_error(TABLE8_ANSWER, 9, ESP_ERR_INVALID_RESPONSE, 1);
    puts("PASS valid checksum with wrong gas/unit/decimal/full range -> INVALID_RESPONSE");

    /* 7. Incomplete frames, with and without a garbage prefix. */
    const uint8_t garbage[] = {0x00, 0x13, 0x88, 0x04, 0x7F};   /* no 0xFF */
    uint8_t pre[sizeof(garbage) + 9];
    memcpy(pre, garbage, sizeof(garbage));
    memcpy(pre + sizeof(garbage), TABLE4_UPLOAD, 9);
    for (size_t n = 1; n < 9; ++n) {
        expect_error(TABLE4_UPLOAD, n, ESP_ERR_INVALID_SIZE, 0);
        expect_error(pre, sizeof(garbage) + n, ESP_ERR_INVALID_SIZE, sizeof(garbage));
    }
    expect_ok(pre, sizeof(pre), 37, sizeof(pre));
    puts("PASS every incomplete length 1..8 keeps the partial frame");

    /* 8. Empty / no start byte. */
    expect_error(TABLE4_UPLOAD, 0, ESP_ERR_NOT_FOUND, 0);
    expect_error(garbage, sizeof(garbage), ESP_ERR_NOT_FOUND, sizeof(garbage));
    puts("PASS empty buffer and garbage without start byte");

    /* 9. False start bytes just before a real frame: each rejected 0xFF is
     * consumed alone, so the real frame is found on a later call. */
    const uint8_t false_starts[] = {0xFF, 0xFF, 0x04, 0x03, 0xFF, 0x01};
    uint8_t mix[sizeof(false_starts) + 9];
    memcpy(mix, false_starts, sizeof(false_starts));
    memcpy(mix + sizeof(false_starts), TABLE4_UPLOAD, 9);
    uint16_t vals[256];
    assert(feed_stream(mix, sizeof(mix), sizeof(mix), vals, 256) == 1);
    assert(vals[0] == 37);
    puts("PASS false 0xFF starts before a frame are skipped one byte at a time");

    /* 10. Two consecutive distinct frames. */
    uint8_t pair[18];
    make_frame(pair, 37);
    make_frame(pair + 9, 1234);
    expect_ok(pair, 18, 37, 9);
    expect_ok(pair + 9, 9, 1234, 9);
    puts("PASS consecutive frames consumed independently");

    /* 11. Every chunk size 1..18 (incl. byte-by-byte) gives both frames. */
    for (size_t chunk = 1; chunk <= 18; ++chunk) {
        assert(feed_stream(pair, 18, chunk, vals, 256) == 2);
        assert(vals[0] == 37 && vals[1] == 1234);
    }
    puts("PASS every split position / chunk size 1..18");

    /* 12. A corrupted frame followed directly by a good one: the good one is
     * recovered (resync), whatever byte was corrupted. */
    for (size_t i = 1; i < 9; ++i) {
        uint8_t two[18];
        make_frame(two, 0x00FF);   /* 0xFF inside data on purpose */
        two[i] ^= 0x10;
        make_frame(two + 9, 500);
        size_t n = feed_stream(two, 18, 1, vals, 256);
        assert(n >= 1 && vals[n - 1] == 500);
        assert(n == 1);
    }
    puts("PASS corrupted frame then good frame -> only the good one, resynced");

    /* 13. Seeded random garbage with 200 real frames inserted: every real
     * frame is recovered in order, nothing else is accepted. */
    static uint8_t stream[200 * (9 + 40)];
    uint16_t expected[200];
    size_t n = 0;
    for (size_t k = 0; k < 200; ++k) {
        const size_t gap = lcg_byte() % 41U;
        for (size_t g = 0; g < gap; ++g) {
            uint8_t b = lcg_byte();
            if ((b & 7U) == 0) {
                b = 0xFF;   /* plenty of false start bytes */
            }
            stream[n++] = b;
        }
        expected[k] = (uint16_t)((lcg_byte() << 8) | lcg_byte()) % 6000U;
        make_frame(stream + n, expected[k]);
        n += 9;
    }
    static uint16_t got[400];
    for (size_t chunk = 1; chunk <= 32; chunk = chunk * 2 + 1) {
        const size_t found = feed_stream(stream, n, chunk, got, 400);
        assert(found == 200);
        assert(memcmp(got, expected, sizeof(expected)) == 0);
    }
    puts("PASS 200 frames in seeded random garbage, chunk 1/3/7/15/31");

    /* 14. NULL arguments. */
    ze07_reading_t r;
    size_t consumed = 99;
    assert(ze07_parse(NULL, 9, &r, &consumed) == ESP_ERR_INVALID_ARG && consumed == 0);
    assert(ze07_parse(TABLE4_UPLOAD, 9, NULL, &consumed) == ESP_ERR_INVALID_ARG);
    assert(ze07_parse(TABLE4_UPLOAD, 9, &r, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS NULL arguments -> INVALID_ARG");

    /* 15. conc_x10 -> co_ppm column: every 16-bit value through
     * csv_format_row (the real CSV code) gives exactly "<x10/10>.<x10%10>",
     * i.e. converting 0.1 ppm units to float and back never changes the
     * one-decimal value. */
    csv_sample_t s;
    memset(&s, 0, sizeof(s));
    s.local_time.tm_year = 2026 - 1900;
    s.local_time.tm_mon = 9;
    s.local_time.tm_mday = 18;
    s.has_co = true;
    for (uint32_t x = 0; x <= 0xFFFF; ++x) {
        s.co_ppm = (float)x / 10.0f;
        char row[128], want[32];
        assert(csv_format_row(&s, row, sizeof(row)) > 0);
        snprintf(want, sizeof(want), ",%u.%u,normal\n", (unsigned)(x / 10), (unsigned)(x % 10));
        const size_t rl = strlen(row), wl = strlen(want);
        assert(rl > wl && strcmp(row + rl - wl, want) == 0);
    }
    puts("PASS all 65,536 conc_x10 values -> co_ppm text exact via csv_format_row");

    puts("ALL ZE07 PARSE TESTS PASSED");
    return 0;
}
