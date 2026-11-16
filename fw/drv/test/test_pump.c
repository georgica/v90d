#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../pump.h"
#include "v90_pcm.h"

static pump_t P;

/* run `n` samples of line time in interrupts of `chunk`; the line receives
 * a counter (mod 251), what we send is appended to sent[] */
static uint8_t sent[100000]; static unsigned n_sent, line_t0;
static void line(unsigned n, unsigned chunk)
{
    int16_t rx[64]; uint8_t tx[64]; unsigned i;
    while (n) {
        unsigned c = n < chunk ? n : chunk;
        for (i = 0; i < c; i++) rx[i] = (int16_t)((line_t0 + i) % 251);
        pump_tick(&P, rx, tx, c);
        memcpy(sent + n_sent, tx, c); n_sent += c; line_t0 += c; n -= c;
    }
}
static void reset(unsigned latency) { n_sent = 0; line_t0 = 0; pump_start(&P, 0xD5, latency); }

static void test_blocks_in_order(void)
{
    int16_t b[PUMP_BLOCK]; unsigned k, i, bad = 0;
    reset(160);
    CHECK(!pump_ready(&P)); CHECK(pump_read(&P, b) == 0);
    line(79, 16); CHECK(!pump_ready(&P));
    line(1, 16);  CHECK(pump_ready(&P));
    line(400, 16);
    for (k = 0; k < 6; k++) {
        CHECK(pump_read(&P, b) == 1);
        for (i = 0; i < PUMP_BLOCK; i++) bad += b[i] != (uint8_t)((k * 80 + i) % 251);
    }
    CHECK_EQ_I((int)bad, 0);
    CHECK(pump_read(&P, b) == 0);
    CHECK_EQ_I((int)P.overruns, 0);
}

/* the daemon's loop: read block k, answer it with block k; the answer's
 * first codeword goes out exactly `latency` after the block's last one came
 * in, whatever the interrupt size */
static void constant_delay(unsigned latency, unsigned chunk)
{
    int16_t b[PUMP_BLOCK]; uint8_t a[PUMP_BLOCK]; unsigned k, i, bad = 0;
    reset(latency);
    for (k = 0; k < 50; k++) {
        line(80, chunk);
        CHECK(pump_read(&P, b) == 1);
        for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(1 + (k * 80 + i) % 199);
        CHECK(pump_write(&P, a) == 0);
    }
    for (i = 0; i < latency; i++) bad += sent[i] != 0xD5;
    for (i = latency; i < n_sent; i++) bad += sent[i] != (uint8_t)(1 + (i - latency) % 199);
    CHECK_EQ_I((int)bad, 0);
    CHECK_EQ_I((int)P.underruns, 0);
}

/* an answer written up to latency - 80 late (in samples) still goes out at
 * the same place */
static void test_late_answer_same_place(void)
{
    int16_t b[PUMP_BLOCK]; uint8_t a[PUMP_BLOCK]; unsigned k, i, bad = 0, late;
    for (late = 0; late <= 72; late += 24) {
        reset(160);
        line(80, 8);
        for (k = 0; k < 40; k++) {
            CHECK(pump_read(&P, b) == 1);
            line(late, 8);
            for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(1 + (k * 80 + i) % 199);
            CHECK(pump_write(&P, a) == 0);
            line(80 - late, 8);
        }
        bad = 0;
        for (i = 0; i < 160; i++) bad += sent[i] != 0xD5;
        for (i = 160; i < n_sent; i++) bad += sent[i] != (uint8_t)(1 + (i - 160) % 199);
        CHECK_EQ_I((int)bad, 0); CHECK_EQ_I((int)P.underruns, 0);
    }
}

/* an answer that misses its time: idle goes out and is counted, and the
 * part of the late answer whose time has passed is dropped, so that what
 * follows is on the wire exactly when it would have been (the delay does not
 * grow: the modem's echo canceller and the V.90 frame both depend on it;
 * seen on the box 2026-11-15, where one late block ended a call) */
static void test_underrun(void)
{
    int16_t b[PUMP_BLOCK]; uint8_t a[PUMP_BLOCK]; unsigned i, bad = 0;
    /* places on the wire: 0..159 the start-up idle, 160..239 the answer to
     * the first block, 240..319 to the second ... */
    reset(160);
    line(80, 16); CHECK(pump_read(&P, b) == 1);
    line(120, 8);                               /* time 200: 40 samples too late */
    CHECK_EQ_I((int)P.underruns, 40);
    for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(1 + i);
    CHECK(pump_write(&P, a) == 0);
    /* the next answer, in time, follows without a gap */
    CHECK(pump_read(&P, b) == 1);
    for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(101 + i);
    CHECK(pump_write(&P, a) == 0);
    line(120, 8);
    CHECK_EQ_I((int)P.underruns, 40);
    for (i = 0; i < 200; i++) bad += sent[i] != 0xD5;
    for (i = 40; i < 80; i++) bad += sent[160 + i] != (uint8_t)(1 + i);
    for (i = 0; i < 80; i++) bad += sent[240 + i] != (uint8_t)(101 + i);
    CHECK_EQ_I((int)bad, 0);
    /* an answer more than a block late is dropped whole */
    reset(160);
    line(80, 16); CHECK(pump_read(&P, b) == 1);
    line(240, 8);                               /* time 320 */
    for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(1 + i);
    CHECK(pump_write(&P, a) == 0);              /* place 160..239: gone */
    CHECK(pump_read(&P, b) == 1); CHECK(pump_write(&P, a) == 0);   /* 240..319: gone */
    CHECK(pump_read(&P, b) == 1); CHECK(pump_write(&P, a) == 0);   /* 320..399 */
    CHECK(pump_read(&P, b) == 1);
    for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(101 + i);
    CHECK(pump_write(&P, a) == 0);              /* 400..479 */
    line(160, 8);
    CHECK_EQ_I((int)P.underruns, 160);
    for (i = 0; i < 320; i++) bad += sent[i] != 0xD5;
    for (i = 0; i < 80; i++) bad += sent[320 + i] != (uint8_t)(1 + i);
    for (i = 0; i < 80; i++) bad += sent[400 + i] != (uint8_t)(101 + i);
    CHECK_EQ_I((int)bad, 0);
    /* a new start is a clean one */
    reset(160);
    for (i = 0; i < PUMP_BLOCK; i++) a[i] = (uint8_t)(1 + i);
    line(80, 16); CHECK(pump_read(&P, b) == 1); CHECK(pump_write(&P, a) == 0);
    line(160, 16);
    CHECK_EQ_I((int)P.underruns, 0);
    CHECK(sent[159] == 0xD5 && sent[160] == 1 && sent[239] == 80);
}

/* nobody reads: the oldest received blocks go, whole, and are counted */
static void test_overrun(void)
{
    int16_t b[PUMP_BLOCK]; unsigned i, bad = 0, first;
    reset(160);
    line(PUMP_RING + 400, 32);
    CHECK(P.overruns >= 400); CHECK(P.overruns % PUMP_BLOCK == 0);
    CHECK(pump_read(&P, b) == 1);
    first = (unsigned)b[0];
    for (i = 0; i < PUMP_BLOCK; i++) bad += b[i] != (uint8_t)((first + i) % 251);
    CHECK_EQ_I((int)bad, 0);
    /* still on a block boundary of the line's count */
    CHECK_EQ_I((int)((P.overruns) % PUMP_BLOCK), 0);
}

static void test_write_full(void)
{
    uint8_t a[PUMP_BLOCK]; int i, r = 0;
    memset(a, 1, sizeof a);
    reset(160);
    for (i = 0; i < 20 && r == 0; i++) r = pump_write(&P, a);
    CHECK_EQ_I(r, -1);
    CHECK(i >= 10);
}

/* the transmit table of linear mode: the exact G.711 value of every
 * codeword of both laws, as the modem's own tables have it (v90/v90_pcm.c) */
static void test_lin16_table(void)
{
    int16_t t[256]; int alaw, i, bad = 0, distinct = 0;
    for (alaw = 0; alaw < 2; alaw++) {
        pump_lin16_table(alaw, t);
        for (i = 0; i < 256; i++) {
            bad += t[i] != (int16_t)v90_octet_linear(alaw, (uint8_t)i);
            bad += t[i] != pump_lin16(alaw, (uint8_t)i);
            if (i && i != 128) distinct += t[i] != t[i - 1];
        }
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(distinct, 2 * 254);
    CHECK_EQ_I(pump_lin16(0, 0xFF), 0); CHECK_EQ_I(pump_lin16(0, 0x7F), 0);
    CHECK_EQ_I(pump_lin16(0, 0x80), 32124); CHECK_EQ_I(pump_lin16(0, 0x00), -32124);
    CHECK_EQ_I(pump_lin16(1, 0xD5), 8); CHECK_EQ_I(pump_lin16(1, 0x55), -8);
    CHECK_EQ_I(pump_lin16(1, 0xAA), 32256); CHECK_EQ_I(pump_lin16(1, 0x2A), -32256);
}

/* FIFO words: a slot is bits 31:16, first bit on the wire first */
static void test_words(void)
{
    int16_t t[256];
    pump_lin16_table(0, t);
    CHECK(pump_tx_word(0, 0x9E) == 0x9EFF0000ul);           /* companded: codeword, then ones */
    CHECK(pump_tx_word(t, 0x80) == 0x7D7C0000ul);           /* 32124 */
    CHECK(pump_tx_word(t, 0x00) == 0x82840000ul);           /* -32124 */
    CHECK(pump_tx_word(t, 0xFF) == 0);
    CHECK_EQ_I(pump_rx_sample(0, 0x9EFF0000ul), 0x9E);
    CHECK_EQ_I(pump_rx_sample(0, 0xFFFF0000ul), 0xFF);
    CHECK_EQ_I(pump_rx_sample(1, 0x7D7C0000ul), 32124);
    CHECK_EQ_I(pump_rx_sample(1, 0xFFFF1234ul), -1);
    CHECK_EQ_I(pump_rx_sample(1, 0x80000000ul), -32768);
}

/* a whole linear path through the pump: 16-bit samples come out of
 * pump_read as they went in, and the block is 1 + 160 octets, little endian;
 * the companded block is the 81 octets it always was */
static void test_block_formats(void)
{
    int16_t rx[80], b[PUMP_BLOCK]; uint8_t tx[80], out[PUMP_READ_LINEAR]; unsigned i, bad = 0;
    pump_start(&P, 0xFF, 160);
    for (i = 0; i < 80; i++) rx[i] = (int16_t)(i * 811 - 32000);
    pump_tick(&P, rx, tx, 80);
    CHECK(pump_read(&P, b) == 1);
    for (i = 0; i < 80; i++) bad += b[i] != rx[i];
    CHECK_EQ_I(pump_block_out(1, 1, b, out), 161);
    CHECK_EQ_I(out[0], 1);
    for (i = 0; i < 80; i++) bad += (int16_t)(out[1 + 2 * i] | (out[2 + 2 * i] << 8)) != rx[i];
    CHECK_EQ_I((int)bad, 0);
    for (i = 0; i < 80; i++) b[i] = (int16_t)(255 - i);
    memset(out, 0xEE, sizeof out);
    CHECK_EQ_I(pump_block_out(0, 0, b, out), 81);
    CHECK_EQ_I(out[0], 0);
    for (i = 0; i < 80; i++) bad += out[1 + i] != 255 - i;
    CHECK_EQ_I((int)bad, 0);
    CHECK_EQ_I(out[81], 0xEE);
}

/* the level meter: a square wave of +/-1000 on a DC of 10 */
static void test_meter(void)
{
    pump_meter m; unsigned i;
    memset(&m, 0, sizeof m);
    for (i = 0; i < PUMP_METER - 1; i++) pump_meter_add(&m, (int16_t)((i & 1) ? 1010 : -990));
    CHECK_EQ_I(m.ms, 0);                                    /* nothing latched yet */
    pump_meter_add(&m, 1010);
    CHECK_EQ_I(m.dc, 10); CHECK_EQ_I(m.last_peak, 1010); CHECK_EQ_I(m.ms, 1000100);
    for (i = 0; i < PUMP_METER; i++) pump_meter_add(&m, -3);
    CHECK_EQ_I(m.dc, -3); CHECK_EQ_I(m.last_peak, 3); CHECK_EQ_I(m.ms, 9);
    for (i = 0; i < PUMP_METER; i++) pump_meter_add(&m, -32768);
    CHECK_EQ_I(m.last_peak, 32768); CHECK_EQ_I(m.ms, 1073741824L); CHECK_EQ_I(m.dc, -32768);
}

int main(void)
{
    test_lin16_table();
    test_words();
    test_block_formats();
    test_meter();
    test_blocks_in_order();
    constant_delay(160, 16);
    constant_delay(160, 1);
    constant_delay(160, 32);
    constant_delay(240, 20);
    test_late_answer_same_place();
    test_underrun();
    test_overrun();
    test_write_full();
    T_REPORT("pump");
}
