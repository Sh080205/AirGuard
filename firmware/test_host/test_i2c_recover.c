#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "i2c_recover.h"

#define PASS(name) printf("PASS %s\n", name)

/* Simulated bus: open-drain lines (line = AND of everyone's output) with
 * one slave that is in the middle of sending bits. The slave shifts out its
 * next bit on each falling SCL edge (as a slave transmitter does) and
 * returns to idle on a STOP (SDA rising while SCL is high). */
typedef struct {
    int m_sda, m_scl;             /* master outputs: 0 = low, 1 = released */
    const int *bits;              /* bits the slave still has to send */
    unsigned nbits, idx;
    bool slave_idle;              /* after STOP: releases SDA */
    bool sda_stuck;               /* SDA held low whatever happens */
    bool scl_stuck;               /* SCL held low whatever happens */
    unsigned stretch_halves;      /* SCL held low this many half periods after
                                     the master releases it (once) */
    unsigned stretch_left;
    unsigned falling_edges, stops, starts;
    unsigned halves;              /* delay_half calls */
    unsigned set_calls, fail_at;  /* fail_at: set_* call that returns ESP_FAIL (0 = never) */
} sim_t;

static int slave_sda(const sim_t *s)
{
    if (s->sda_stuck) {
        return 0;
    }
    if (s->slave_idle || s->idx >= s->nbits) {
        return 1;
    }
    return s->bits[s->idx];
}

static int line_sda(const sim_t *s) { return s->m_sda & slave_sda(s); }

static int line_scl(const sim_t *s)
{
    if (s->scl_stuck || s->stretch_left > 0U) {
        return 0;
    }
    return s->m_scl;
}

static int rd_sda(void *ctx) { return line_sda(ctx); }
static int rd_scl(void *ctx) { return line_scl(ctx); }

static esp_err_t wr_sda(void *ctx, int level)
{
    sim_t *s = ctx;
    if (++s->set_calls == s->fail_at) {
        return ESP_FAIL;
    }
    const int before = line_sda(s);
    s->m_sda = level ? 1 : 0;
    const int after = line_sda(s);
    if (line_scl(s) == 1 && before != after) {
        if (after == 1) {
            ++s->stops;
            s->slave_idle = true;
        } else {
            ++s->starts;
        }
    }
    return ESP_OK;
}

static esp_err_t wr_scl(void *ctx, int level)
{
    sim_t *s = ctx;
    if (++s->set_calls == s->fail_at) {
        return ESP_FAIL;
    }
    const int before = line_scl(s);
    const int new_m = level ? 1 : 0;
    if (s->m_scl == 0 && new_m == 1 && s->stretch_halves > 0U) {
        s->stretch_left = s->stretch_halves;
        s->stretch_halves = 0U;
    }
    s->m_scl = new_m;
    if (before == 1 && line_scl(s) == 0) {
        ++s->falling_edges;
        if (!s->slave_idle && s->idx < s->nbits) {
            ++s->idx;
        }
    }
    return ESP_OK;
}

static void dly(void *ctx)
{
    sim_t *s = ctx;
    ++s->halves;
    if (s->stretch_left > 0U) {
        --s->stretch_left;
    }
}

static i2c_recover_io_t io_for(sim_t *s)
{
    return (i2c_recover_io_t){
        .ctx = s, .read_sda = rd_sda, .read_scl = rd_scl,
        .set_sda = wr_sda, .set_scl = wr_scl, .delay_half = dly,
    };
}

static sim_t sim_new(const int *bits, unsigned n)
{
    sim_t s;
    memset(&s, 0, sizeof s);
    s.m_sda = 1;
    s.m_scl = 1;
    s.bits = bits;
    s.nbits = n;
    s.slave_idle = (n == 0U);
    return s;
}

static void test_idle_bus(void)
{
    sim_t s = sim_new(NULL, 0);
    i2c_recover_io_t io = io_for(&s);
    i2c_lines_t l;
    i2c_recover_sample(&io, I2C_RECOVER_SAMPLES, &l);
    assert(l.samples == 16U && l.sda_low == 0U && l.scl_low == 0U);
    assert(i2c_lines_state(&l) == I2C_LINES_OK);

    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_IDLE);
    assert(r.clocks == 0U);
    assert(r.sda_high_after && r.scl_high_after);
    /* only a STOP (harmless on an idle bus): no START, no clock beyond it */
    assert(s.stops == 1U && s.starts == 0U);
    assert(s.falling_edges == 1U);
    assert(s.m_sda == 1 && s.m_scl == 1);
    PASS("idle bus: no clocks, one STOP, lines released");
}

/* Slave stopped while sending 0-bits; it lets go after k falling edges. */
static void test_release_after(unsigned zeros)
{
    int bits[9];
    for (unsigned i = 0; i < 9U; ++i) {
        bits[i] = (i < zeros) ? 0 : 1;
    }
    sim_t s = sim_new(bits, 9U);
    i2c_recover_io_t io = io_for(&s);
    i2c_lines_t l;
    i2c_recover_sample(&io, I2C_RECOVER_SAMPLES, &l);
    assert(l.sda_low == 16U && l.scl_low == 0U);
    assert(i2c_lines_state(&l) == I2C_LINES_SDA_LOW);

    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_RECOVERED);
    assert(r.clocks == zeros);   /* stops clocking as soon as SDA is high */
    assert(r.sda_high_after && r.scl_high_after);
    assert(s.stops >= 1U && s.starts == 0U);
    assert(s.slave_idle);
    assert(s.m_sda == 1 && s.m_scl == 1);
}

static void test_release_cases(void)
{
    for (unsigned k = 1U; k <= I2C_RECOVER_MAX_CLOCKS; ++k) {
        test_release_after(k);
    }
    PASS("SDA held for 1..9 bits: released after exactly that many clocks, STOP sent");
}

static void test_sda_never_released(void)
{
    sim_t s = sim_new(NULL, 0);
    s.slave_idle = false;
    s.sda_stuck = true;
    i2c_recover_io_t io = io_for(&s);
    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_SDA_STILL_LOW);
    assert(r.clocks == I2C_RECOVER_MAX_CLOCKS);
    assert(!r.sda_high_after && r.scl_high_after);
    assert(s.m_sda == 1 && s.m_scl == 1);
    PASS("SDA never released: 9 clocks, reported SDA_STILL_LOW");
}

static void test_scl_stuck(void)
{
    sim_t s = sim_new(NULL, 0);
    s.scl_stuck = true;
    i2c_recover_io_t io = io_for(&s);
    i2c_lines_t l;
    i2c_recover_sample(&io, I2C_RECOVER_SAMPLES, &l);
    assert(i2c_lines_state(&l) == I2C_LINES_SCL_LOW);

    s.halves = 0U;
    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_SCL_STUCK);
    assert(r.clocks == 0U);
    /* nothing pulled low: both master outputs released */
    assert(s.m_sda == 1 && s.m_scl == 1);
    /* gave up after the stretch wait (bounded) */
    assert(s.halves == I2C_RECOVER_SCL_WAIT_HALVES + 1U);
    PASS("SCL held low: no clocks, nothing driven, bounded wait");
}

static void test_clock_stretch(void)
{
    int bits[] = { 0, 0, 1 };
    sim_t s = sim_new(bits, 3U);
    s.stretch_halves = 5U;   /* 25 µs, shorter than the 100 µs wait */
    i2c_recover_io_t io = io_for(&s);
    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_RECOVERED);
    assert(r.clocks == 2U);
    PASS("short clock stretch is waited out");
}

static void test_stretch_too_long(void)
{
    int bits[] = { 0, 0, 1 };
    sim_t s = sim_new(bits, 3U);
    s.stretch_halves = I2C_RECOVER_SCL_WAIT_HALVES + 5U;
    i2c_recover_io_t io = io_for(&s);
    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    assert(r.outcome == I2C_RECOVER_SCL_STUCK);
    assert(r.clocks == 1U);
    assert(s.m_sda == 1 && s.m_scl == 1);
    PASS("stretch longer than the wait: SCL_STUCK, lines released");
}

static void test_driver_errors(void)
{
    /* every set_* call position in a 3-clock recovery fails once */
    int bits[] = { 0, 0, 0, 1 };
    sim_t ref = sim_new(bits, 4U);
    i2c_recover_io_t io = io_for(&ref);
    i2c_recover_result_t r;
    assert(i2c_recover_run(&io, &r) == ESP_OK);
    const unsigned calls = ref.set_calls;
    assert(calls > 4U);
    for (unsigned at = 1U; at <= calls; ++at) {
        sim_t s = sim_new(bits, 4U);
        s.fail_at = at;
        io = io_for(&s);
        assert(i2c_recover_run(&io, &r) == ESP_FAIL);
        assert(s.m_sda == 1 && s.m_scl == 1);
    }
    assert(i2c_recover_run(NULL, &r) == ESP_ERR_INVALID_ARG);
    assert(i2c_recover_run(&io, NULL) == ESP_ERR_INVALID_ARG);
    PASS("a GPIO error at any step is returned and both lines are released");
}

static void test_sample_majority(void)
{
    i2c_lines_t l = { .samples = 16U, .sda_low = 8U, .scl_low = 0U };
    assert(i2c_lines_state(&l) == I2C_LINES_OK);        /* exactly half: not held */
    l.sda_low = 9U;
    assert(i2c_lines_state(&l) == I2C_LINES_SDA_LOW);
    l.scl_low = 9U;
    assert(i2c_lines_state(&l) == I2C_LINES_SCL_LOW);   /* SCL wins */
    l = (i2c_lines_t){ 0 };
    assert(i2c_lines_state(&l) == I2C_LINES_OK);
    assert(i2c_lines_state(NULL) == I2C_LINES_OK);

    sim_t s = sim_new(NULL, 0);
    i2c_recover_io_t io = io_for(&s);
    i2c_recover_sample(&io, 0U, &l);
    assert(l.samples == 1U);
    assert(s.halves == 0U);
    i2c_recover_sample(&io, 16U, &l);
    assert(s.halves == 15U);   /* 16 samples, 15 gaps ≈ 75 µs */
    PASS("line sampling and majority rule");
}

static void test_classify(void)
{
    assert(i2c_probe_from_err(ESP_OK) == I2C_PROBE_OK);
    assert(i2c_probe_from_err(ESP_ERR_NOT_FOUND) == I2C_PROBE_NACK);
    assert(i2c_probe_from_err(ESP_ERR_TIMEOUT) == I2C_PROBE_TIMEOUT);
    assert(i2c_probe_from_err(ESP_ERR_INVALID_STATE) == I2C_PROBE_ERROR);

    assert(i2c_case_classify(I2C_LINES_OK, I2C_PROBE_OK, I2C_PROBE_OK) == I2C_CASE_NORMAL);
    /* the case names the cause: SDA low before R1 is A even if R1 fixed it */
    assert(i2c_case_classify(I2C_LINES_SDA_LOW, I2C_PROBE_OK, I2C_PROBE_OK) == I2C_CASE_A);
    assert(i2c_case_classify(I2C_LINES_SDA_LOW, I2C_PROBE_TIMEOUT, I2C_PROBE_TIMEOUT) == I2C_CASE_A);
    assert(i2c_case_classify(I2C_LINES_SCL_LOW, I2C_PROBE_TIMEOUT, I2C_PROBE_OK) == I2C_CASE_B);
    assert(i2c_case_classify(I2C_LINES_OK, I2C_PROBE_NACK, I2C_PROBE_NACK) == I2C_CASE_C);
    assert(i2c_case_classify(I2C_LINES_OK, I2C_PROBE_OK, I2C_PROBE_NACK) == I2C_CASE_OTHER);
    assert(i2c_case_classify(I2C_LINES_OK, I2C_PROBE_TIMEOUT, I2C_PROBE_TIMEOUT) == I2C_CASE_OTHER);

    assert(!i2c_probe_wants_reset(I2C_PROBE_OK, I2C_PROBE_OK));
    assert(i2c_probe_wants_reset(I2C_PROBE_NACK, I2C_PROBE_OK));
    assert(i2c_probe_wants_reset(I2C_PROBE_OK, I2C_PROBE_TIMEOUT));
    assert(i2c_probe_wants_reset(I2C_PROBE_SKIPPED, I2C_PROBE_SKIPPED));

    assert(strcmp(i2c_case_name(I2C_CASE_OTHER), "X") == 0);
    assert(strcmp(i2c_probe_name(I2C_PROBE_TIMEOUT), "TIMEOUT") == 0);
    assert(strcmp(i2c_recover_outcome_name(I2C_RECOVER_RECOVERED), "recovered") == 0);
    assert(strcmp(i2c_lines_state_name(I2C_LINES_SDA_LOW), "SDA_LOW") == 0);
    PASS("probe mapping, case A/B/C/N/X, boot reset decision");
}

int main(void)
{
    test_idle_bus();
    test_release_cases();
    test_sda_never_released();
    test_scl_stuck();
    test_clock_stretch();
    test_stretch_too_long();
    test_driver_errors();
    test_sample_majority();
    test_classify();
    printf("test_i2c_recover: all passed\n");
    return 0;
}
