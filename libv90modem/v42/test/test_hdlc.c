/* HDLC framing tests: FCS vectors from V.42 8.1.1.6, bit-exact transmit
 * patterns, zero insertion and deletion, abort, flag fill, length limits. */
#include <string.h>
#include "test.h"
#include "v42_hdlc.h"

static unsigned rnd_state = 12345;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 16) & 0x7FFF;
}

/* reverse the low n bits of v */
static uint32_t bitrev(uint32_t v, int n)
{
    uint32_t r = 0; int i;
    for (i = 0; i < n; i++) if (v & (1u << i)) r |= 1u << (n - 1 - i);
    return r;
}

/* parse a "0001 1101 ..." bit string, first character is the high-order bit */
static uint32_t from_bits(const char *s)
{
    uint32_t v = 0;
    for (; *s; s++) if (*s == '0' || *s == '1') v = (v << 1) | (uint32_t)(*s - '0');
    return v;
}

static void test_fcs(void)
{
    static const uint8_t msg[] = "123456789";
    uint8_t f[16];
    uint16_t c16; uint32_t c32;

    /* check values of the two polynomials as used by ISO 3309 */
    CHECK_EQ_I((uint16_t)~v42_fcs16(V42_FCS16_INIT, msg, 9), 0x906E);
    CHECK((uint32_t)~v42_fcs32(V42_FCS32_INIT, msg, 9) == 0xCBF43926u);

    /* residues quoted in V.42 8.1.1.6.1 and 8.1.1.6.2 (x^15 / x^31 first);
     * our registers hold the low-order power in the top bit, so reverse */
    CHECK_EQ_I(bitrev(V42_FCS16_GOOD, 16), from_bits("0001 1101 0000 1111"));
    CHECK(bitrev(V42_FCS32_GOOD, 32) ==
          from_bits("1100 0111 0000 0100 1101 1101 0111 1011"));

    /* frame plus transmitted FCS leaves the residue */
    memcpy(f, msg, 9);
    c16 = (uint16_t)~v42_fcs16(V42_FCS16_INIT, f, 9);
    f[9] = (uint8_t)c16; f[10] = (uint8_t)(c16 >> 8);
    CHECK_EQ_I(v42_fcs16(V42_FCS16_INIT, f, 11), V42_FCS16_GOOD);
    c32 = ~v42_fcs32(V42_FCS32_INIT, f, 9);
    f[9] = (uint8_t)c32; f[10] = (uint8_t)(c32 >> 8);
    f[11] = (uint8_t)(c32 >> 16); f[12] = (uint8_t)(c32 >> 24);
    CHECK(v42_fcs32(V42_FCS32_INIT, f, 13) == V42_FCS32_GOOD);
    /* incremental computation equals one shot */
    CHECK_EQ_I(v42_fcs16(v42_fcs16(V42_FCS16_INIT, msg, 4), msg + 4, 5),
               v42_fcs16(V42_FCS16_INIT, msg, 9));
}

/* collect n line bits into a '0'/'1' string */
static void pull(v42hdlc_tx_t *t, char *out, int n)
{
    int i;
    for (i = 0; i < n; i++) out[i] = (char)('0' + v42hdlc_tx_bit(t));
    out[n] = 0;
}

static void test_tx_bits(void)
{
    v42hdlc_tx_t t;
    char s[400];
    static const uint8_t sabme[2] = { 0x03, 0x7F };
    static const uint8_t ones[3] = { 0xFF, 0xFF, 0xFF };
    uint16_t fcs;
    char want[400]; int n = 0, i, run;
    uint8_t all[5];

    /* idle line is flag fill: 01111110 repeated (8.1.5) */
    v42hdlc_tx_init(&t);
    pull(&t, s, 24);
    CHECK(strcmp(s, "011111100111111001111110") == 0);
    CHECK(v42hdlc_tx_need_frame(&t));

    /* one frame: flag, address 0x03 lsb first, control 0x7F (six ones in the
     * octet, so a zero is inserted after the fifth), FCS, flag */
    CHECK_EQ_I(v42hdlc_tx_frame(&t, sabme, 2, 0), 0);
    CHECK(!v42hdlc_tx_need_frame(&t));
    CHECK_EQ_I(v42hdlc_tx_frame(&t, sabme, 2, 0), -1);   /* still loaded */
    fcs = (uint16_t)~v42_fcs16(V42_FCS16_INIT, sabme, 2);
    all[0] = 0x03; all[1] = 0x7F; all[2] = (uint8_t)fcs; all[3] = (uint8_t)(fcs >> 8);
    /* the previous flag doubles as opening flag: data starts at once */
    run = 0;
    for (i = 0; i < 32; i++) {
        int b = (all[i / 8] >> (i % 8)) & 1;
        want[n++] = (char)('0' + b);
        if (b) { if (++run == 5) { want[n++] = '0'; run = 0; } } else run = 0;
    }
    memcpy(want + n, "01111110", 8); n += 8; want[n] = 0;
    pull(&t, s, n);
    CHECK(strcmp(s, want) == 0);
    /* the data really did start 1,1,0,0,0,0,0,0 then 1,1,1,1,1,(0),1,1,0 */
    CHECK(strncmp(s, "11000000" "111110110", 17) == 0);
    CHECK(v42hdlc_tx_need_frame(&t));
    CHECK_EQ_I(t.frames, 1);

    /* all-ones payload never shows six ones in a row inside the frame */
    v42hdlc_tx_init(&t);
    v42hdlc_tx_frame(&t, ones, 3, 1);
    pull(&t, s, 8);                                   /* opening flag */
    CHECK(strcmp(s, "01111110") == 0);
    pull(&t, s, 24 + 4);                              /* 24 ones + 4 stuffed */
    CHECK(strcmp(s, "1111101111101111101111101111") == 0);

    /* a demanded flag preamble is honoured before the frame goes out */
    v42hdlc_tx_init(&t);
    v42hdlc_tx_flags(&t, 16);
    v42hdlc_tx_frame(&t, sabme, 2, 0);
    pull(&t, s, 16 * 8);
    for (i = 0; i < 16; i++) CHECK(strncmp(s + 8 * i, "01111110", 8) == 0);
    pull(&t, s, 8);
    CHECK(strcmp(s, "11000000") == 0);
}

/* run tx bits into rx, returning the first non-zero rx event */
static int pump(v42hdlc_tx_t *t, v42hdlc_rx_t *r, int maxbits)
{
    int i, ev;
    for (i = 0; i < maxbits; i++)
        if ((ev = v42hdlc_rx_bit(r, v42hdlc_tx_bit(t))) != 0) return ev;
    return 0;
}

static void test_loopback(void)
{
    v42hdlc_tx_t t; v42hdlc_rx_t r;
    uint8_t f[V42HDLC_MAX_PAYLOAD];
    int iter, i, len, ev, fcs32;

    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS_EITHER);
    for (iter = 0; iter < 400; iter++) {
        len = 2 + (int)(rnd() % (V42HDLC_MAX_PAYLOAD - 1));
        for (i = 0; i < len; i++) {
            unsigned k = rnd() % 8;
            f[i] = k < 3 ? 0xFF : k == 3 ? 0x7E : k == 4 ? 0x00 : (uint8_t)rnd();
        }
        fcs32 = (int)(rnd() & 1);
        while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
        CHECK_EQ_I(v42hdlc_tx_frame(&t, f, len, fcs32), 0);
        ev = pump(&t, &r, 20 * V42HDLC_MAX_FRAME);
        if (ev != len || memcmp(r.buf, f, (size_t)len) != 0 || r.last_fcs32 != fcs32) {
            CHECK(ev == len);
            CHECK(r.last_fcs32 == fcs32);
            break;
        }
    }
    CHECK_EQ_I(iter, 400);
    CHECK_EQ_I(r.good, 400);
    CHECK_EQ_I(r.bad_fcs + r.runts + r.aborts + r.too_long + r.misaligned, 0);

    /* strict modes refuse the other FCS */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    for (i = 0; i < 20; i++) f[i] = (uint8_t)(i * 7);
    v42hdlc_tx_frame(&t, f, 20, 1);
    CHECK_EQ_I(pump(&t, &r, 1000), V42HDLC_RX_BAD_FCS);   /* default is FCS-16 */
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS32);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 20, 0);
    CHECK_EQ_I(pump(&t, &r, 1000), V42HDLC_RX_BAD_FCS);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 20, 1);
    CHECK_EQ_I(pump(&t, &r, 1000), 20);
}

static void test_errors(void)
{
    v42hdlc_tx_t t; v42hdlc_rx_t r;
    uint8_t f[V42HDLC_MAX_PAYLOAD];
    uint8_t line[4000];
    int nb, i, ev, got, bad;
    static const char runt[] = "01111110" "10000000" "01000000" "11000000" "01111110";
    static const char odd[]  = "01111110" "10000000" "01000000" "11000000" "00100000" "101" "01111110";

    for (i = 0; i < 60; i++) f[i] = (uint8_t)(i * 13 + 1);

    /* a single flipped bit anywhere in the frame is caught by the FCS */
    for (bad = 8; bad < 8 + 62 * 8; bad += 37) {
        v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
        v42hdlc_tx_frame(&t, f, 60, 0);
        nb = 0;
        while (nb < 8 || v42hdlc_tx_busy(&t)) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
        for (i = 0; i < 16; i++) line[nb++] = (uint8_t)v42hdlc_tx_bit(&t);
        line[bad] ^= 1;
        got = 0;
        for (i = 0; i < nb; i++) { ev = v42hdlc_rx_bit(&r, line[i]); if (ev > 0) got++; }
        CHECK_EQ_I(got, 0);
        CHECK_EQ_I(r.good, 0);
    }

    /* transmitter abort: receiver drops the frame, then recovers */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_tx_frame(&t, f, 60, 0);
    for (i = 0; i < 100; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_abort(&t);
    CHECK_EQ_I(pump(&t, &r, 100), V42HDLC_RX_ABORT);
    CHECK_EQ_I(t.aborts, 1);
    CHECK_EQ_I(r.aborts, 1);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 60, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), 60);
    CHECK(memcmp(r.buf, f, 60) == 0);

    /* line stuck at mark is not a frame and not repeated aborts */
    v42hdlc_rx_init(&r);
    for (i = 0; i < 16; i++) v42hdlc_rx_bit(&r, "01111110"[i % 8] - '0');
    for (i = 0; i < 500; i++) CHECK(v42hdlc_rx_bit(&r, 1) == 0);
    CHECK_EQ_I(r.aborts, 0);                 /* nothing was in progress */

    /* fewer than four octets between flags (8.1.3 b) */
    v42hdlc_rx_init(&r);
    ev = 0;
    for (i = 0; runt[i]; i++) { int e = v42hdlc_rx_bit(&r, runt[i] - '0'); if (e) ev = e; }
    CHECK_EQ_I(ev, V42HDLC_RX_SHORT);
    CHECK_EQ_I(r.runts, 1);

    /* not an integral number of octets (8.1.3 c) */
    v42hdlc_rx_init(&r);
    ev = 0;
    for (i = 0; odd[i]; i++) { int e = v42hdlc_rx_bit(&r, odd[i] - '0'); if (e) ev = e; }
    CHECK_EQ_I(ev, V42HDLC_RX_ALIGN);
    CHECK_EQ_I(r.misaligned, 1);

    /* maximum frame length enforcement: over the limit is discarded, at the
     * limit is accepted */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    v42hdlc_rx_set_max(&r, 3 + 20 + 2);
    v42hdlc_tx_frame(&t, f, 3 + 21, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), V42HDLC_RX_LONG);
    CHECK_EQ_I(r.too_long, 1);
    while (!v42hdlc_tx_need_frame(&t)) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    v42hdlc_tx_frame(&t, f, 3 + 20, 0);
    CHECK_EQ_I(pump(&t, &r, 2000), 3 + 20);
    /* the transmitter refuses frames that cannot fit */
    v42hdlc_tx_init(&t);
    CHECK_EQ_I(v42hdlc_tx_frame(&t, f, V42HDLC_MAX_PAYLOAD + 1, 0), -1);
    CHECK_EQ_I(v42hdlc_tx_frame(&t, f, 0, 0), -1);

    /* consecutive flags are counted (used to spot the protocol phase) */
    v42hdlc_tx_init(&t); v42hdlc_rx_init(&r);
    for (i = 0; i < 80; i++) v42hdlc_rx_bit(&r, v42hdlc_tx_bit(&t));
    CHECK_EQ_I(r.flag_run, 10);
    for (i = 0; i < 20; i++) v42hdlc_rx_bit(&r, 1);
    CHECK_EQ_I(r.flag_run, 0);
}

/* random bit noise must never produce a crash or an oversized frame */
static void test_noise(void)
{
    v42hdlc_rx_t r;
    long i; int ev, worst = 0;
    v42hdlc_rx_init(&r);
    v42hdlc_rx_set_fcs(&r, V42HDLC_FCS_EITHER);
    for (i = 0; i < 2000000; i++) {
        ev = v42hdlc_rx_bit(&r, (int)(rnd() >> 7) & 1);
        if (ev > worst) worst = ev;
    }
    CHECK(worst <= V42HDLC_MAX_PAYLOAD);
    CHECK(r.bad_fcs > 100);
    /* FCS-16 lets through about one random frame in 65536 */
    CHECK(r.good * 2000 < r.bad_fcs + 2000);
}

int main(void)
{
    test_fcs();
    test_tx_bits();
    test_loopback();
    test_errors();
    test_noise();
    T_REPORT("test_hdlc");
}
