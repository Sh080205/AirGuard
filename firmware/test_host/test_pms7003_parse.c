#define PMS7003_PARSE_ONLY
#include <stdio.h>
#include "test_frames.h"

static void expect_error(const uint8_t *buf, size_t len, esp_err_t expected,
                         size_t expected_consumed)
{
    pms7003_data_t out;
    unsigned char saved[sizeof(out)];
    memset(&out, 0xa5, sizeof(out));
    memcpy(saved, &out, sizeof(out));
    size_t consumed = SIZE_MAX;
    assert(pms7003_parse(buf, len, &out, &consumed) == expected);
    assert(consumed == expected_consumed);
    assert(memcmp(saved, &out, sizeof(out)) == 0);
}

int main(void)
{
    uint8_t frame[32]; /* Appendix I frame size; deliberately independent. */
    make_frame(frame, sample);
    pms7003_data_t out;
    size_t consumed = 0;
    assert(pms7003_parse(frame, sizeof(frame), &out, &consumed) == ESP_OK);
    assert(consumed == sizeof(frame));
    check_data(&out, sample);
    puts("PASS valid frame: all six fields, endian and uint16 boundaries");

    /* Flip every bit in the checksum-covered data and checksum itself. */
    for (size_t i = 4; i < sizeof(frame); ++i) {
        for (unsigned bit = 0; bit < 8; ++bit) {
            frame[i] ^= (uint8_t)(1U << bit);
            expect_error(frame, sizeof(frame), ESP_ERR_INVALID_CRC, 1);
            frame[i] ^= (uint8_t)(1U << bit);
        }
    }
    puts("PASS single-bit corruption (224 cases), output unchanged");

    const uint8_t garbage[] = {0x99, 0x42, 0x00, 0x4d, 0x42};
    uint8_t prefixed[sizeof(garbage) + sizeof(frame)];
    memcpy(prefixed, garbage, sizeof(garbage));
    memcpy(prefixed + sizeof(garbage), frame, sizeof(frame));
    for (size_t n = 2; n < sizeof(frame); ++n) {
        expect_error(frame, n, ESP_ERR_INVALID_SIZE, 0);
        expect_error(prefixed, sizeof(garbage) + n, ESP_ERR_INVALID_SIZE,
                     sizeof(garbage));
    }
    puts("PASS every incomplete length, with and without garbage prefix");
    assert(pms7003_parse(prefixed, sizeof(prefixed), &out, &consumed) == ESP_OK);
    assert(consumed == sizeof(prefixed));
    check_data(&out, sample);
    puts("PASS leading garbage, false headers and overlapping first byte");

    expect_error(frame, 0, ESP_ERR_NOT_FOUND, 0);
    expect_error(frame, 1, ESP_ERR_NOT_FOUND, 0);
    expect_error(garbage, sizeof(garbage), ESP_ERR_NOT_FOUND, sizeof(garbage) - 1);
    expect_error(garbage, sizeof(garbage) - 1, ESP_ERR_NOT_FOUND, sizeof(garbage) - 1);
    puts("PASS empty buffer, no header and trailing split header");

    uint16_t other[13];
    memcpy(other, sample, sizeof(other));
    other[0] = 0xfedc;
    uint8_t pair[2 * sizeof(frame)];
    memcpy(pair, frame, sizeof(frame));
    make_frame(pair + sizeof(frame), other);
    assert(pms7003_parse(pair, sizeof(pair), &out, &consumed) == ESP_OK);
    assert(consumed == sizeof(frame));
    check_data(&out, sample);
    const size_t first_consumed = consumed;
    assert(pms7003_parse(pair + first_consumed, sizeof(pair) - first_consumed,
                         &out, &consumed) == ESP_OK);
    assert(consumed == sizeof(frame));
    check_data(&out, other);
    puts("PASS consecutive distinct frames consumed independently");

    const uint16_t bad_lengths[] = {0, 1, 27, 29, 0x011c, 0xffff};
    for (size_t i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); ++i) {
        prefixed[sizeof(garbage) + 2] = (uint8_t)(bad_lengths[i] >> 8);
        prefixed[sizeof(garbage) + 3] = (uint8_t)bad_lengths[i];
        expect_error(prefixed, sizeof(prefixed), ESP_ERR_INVALID_RESPONSE,
                     sizeof(garbage) + 1);
        expect_error(prefixed, sizeof(garbage) + 4, ESP_ERR_INVALID_RESPONSE,
                     sizeof(garbage) + 1);
    }
    puts("PASS incorrect frame length rejected before waiting for payload");

    /* Recovery past corrupt candidates, including an embedded false header. */
    pair[4] ^= 1;
    size_t offset = 0;
    while (offset < sizeof(pair)) {
        const esp_err_t err = pms7003_parse(pair + offset, sizeof(pair) - offset,
                                           &out, &consumed);
        assert(consumed > 0);
        offset += consumed;
        if (err == ESP_OK) {
            break;
        }
        assert(err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_RESPONSE);
    }
    assert(offset == sizeof(pair));
    check_data(&out, other);
    puts("PASS resynchronize after bad checksum and nested false header");

    memcpy(prefixed + sizeof(garbage), frame, sizeof(frame));
    /* Exercise every split, including header, length, payload and checksum. */
    for (size_t split = 0; split <= sizeof(prefixed); ++split) {
        uint8_t pending[sizeof(prefixed)];
        memcpy(pending, prefixed, split);
        const esp_err_t first = pms7003_parse(pending, split, &out, &consumed);
        size_t used = split - consumed;
        memmove(pending, pending + consumed, used);
        if (first == ESP_OK) {
            assert(split == sizeof(prefixed));
        } else {
            memcpy(pending + used, prefixed + split, sizeof(prefixed) - split);
            used += sizeof(prefixed) - split;
            assert(pms7003_parse(pending, used, &out, &consumed) == ESP_OK);
            assert(consumed == used);
        }
        check_data(&out, sample);
    }
    puts("PASS all UART two-chunk split positions");

    uint8_t pending[sizeof(prefixed)];
    size_t used = 0, frames = 0;
    for (size_t i = 0; i < sizeof(prefixed); ++i) {
        pending[used++] = prefixed[i];
        const esp_err_t err = pms7003_parse(pending, used, &out, &consumed);
        assert(consumed <= used);
        used -= consumed;
        memmove(pending, pending + consumed, used);
        if (err == ESP_OK) {
            ++frames;
            check_data(&out, sample);
        } else {
            assert(err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_NOT_FOUND);
        }
    }
    assert(frames == 1 && used == 0);
    puts("PASS UART byte-by-byte stream returns exactly one frame");

    expect_error(NULL, sizeof(frame), ESP_ERR_INVALID_ARG, 0);
    expect_error(NULL, 0, ESP_ERR_INVALID_ARG, 0);
    consumed = SIZE_MAX;
    assert(pms7003_parse(frame, sizeof(frame), NULL, &consumed) == ESP_ERR_INVALID_ARG);
    assert(consumed == 0);
    memset(&out, 0xa5, sizeof(out));
    unsigned char saved[sizeof(out)];
    memcpy(saved, &out, sizeof(out));
    assert(pms7003_parse(frame, sizeof(frame), &out, NULL) == ESP_ERR_INVALID_ARG);
    assert(memcmp(saved, &out, sizeof(out)) == 0);
    assert(pms7003_parse(NULL, 0, NULL, NULL) == ESP_ERR_INVALID_ARG);
    puts("PASS NULL arguments, consumed reset and output unchanged");
    puts("All parser tests passed (11 groups).");
    return 0;
}
