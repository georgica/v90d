/* RFC 1662 framing unit test: FCS-16 vectors, octet stuffing, ACCM on both
 * directions, streaming decode, invalid frame handling, header parsing. */
#include <string.h>
#include "test.h"
#include "ppp_hdlc.h"

/* Independent bit-serial FCS straight from the polynomial
 * x^16 + x^12 + x^5 + 1 (reflected: 0x8408), LSB first. */
static uint16_t ref_fcs(uint16_t f, const uint8_t *p, size_t n)
{
    while (n--) {
        uint8_t b = *p++;
        for (int i = 0; i < 8; i++) {
            int bit = (b >> i) & 1;
            f = ((f ^ bit) & 1) ? (uint16_t)((f >> 1) ^ 0x8408) : (uint16_t)(f >> 1);
        }
    }
    return f;
}

static uint32_t lcg = 12345;
static uint8_t rnd(void)
{
    lcg = lcg * 1103515245u + 12345u;
    return (uint8_t)(lcg >> 16);
}

/* feed a byte string; return number of good frames, remember the last */
static uint8_t last[PPP_HDLC_BUF];
static size_t last_len;
static int feed(ppp_hdlc_rx_t *rx, const uint8_t *p, size_t n)
{
    int good = 0;
    for (size_t i = 0; i < n; i++) {
        if (ppp_hdlc_rx_byte(rx, p[i])) {
            good++;
            last_len = rx->len;
            memcpy(last, rx->buf, rx->len);
        }
    }
    return good;
}

static void test_fcs(void)
{
    const uint8_t chk[] = "123456789";
    const uint8_t lcp[] = { 0xff, 0x03, 0xc0, 0x21, 0x01, 0x01, 0x00, 0x04 };
    uint8_t buf[300];
    uint16_t f;

    /* the standard check value of this CRC (X.25 FCS) */
    CHECK_EQ_I(ppp_fcs16(PPP_FCS_INIT, chk, 9) ^ 0xffff, 0x906e);
    CHECK_EQ_I(ppp_fcs16(PPP_FCS_INIT, lcp, 8) ^ 0xffff, 0xb5d1);

    /* agreement with the bit-serial reference on random data, and the
     * RFC 1662 "good final FCS" 0xf0b8 after running over data + FCS */
    for (int t = 0; t < 50; t++) {
        size_t n = 1 + rnd();
        for (size_t i = 0; i < n; i++) buf[i] = rnd();
        f = ppp_fcs16(PPP_FCS_INIT, buf, n);
        CHECK_EQ_I(f, ref_fcs(0xffff, buf, n));
        f ^= 0xffff;
        buf[n] = (uint8_t)(f & 0xff);      /* least significant octet first */
        buf[n + 1] = (uint8_t)(f >> 8);
        CHECK_EQ_I(ppp_fcs16(PPP_FCS_INIT, buf, n + 2), 0xf0b8);
    }
    CHECK_EQ_I(PPP_FCS_GOOD, 0xf0b8);
    /* incremental use gives the same value */
    f = ppp_fcs16(PPP_FCS_INIT, chk, 4);
    f = ppp_fcs16(f, chk + 4, 5);
    CHECK_EQ_I(f ^ 0xffff, 0x906e);
}

static void test_encode(void)
{
    uint8_t out[64];
    size_t n;
    /* RFC 1662 4.2 examples: 7e -> 7d 5e, 7d -> 7d 5d, 03 -> 7d 23,
     * 11 -> 7d 31, 13 -> 7d 33 */
    const uint8_t a[] = { 0x7e, 0x7d };
    const uint8_t b[] = { 0x03, 0x11, 0x13, 0x41 };
    uint8_t all[6];
    uint16_t f;

    memcpy(all, a, 2);
    memcpy(all + 2, b, 4);
    f = (uint16_t)(ref_fcs(0xffff, all, 6) ^ 0xffff);

    n = ppp_hdlc_encode(out, sizeof out, a, 2, b, 4, 0xffffffffu, 1);
    CHECK(n >= 14);
    {
        const uint8_t want[] = { 0x7e, 0x7d, 0x5e, 0x7d, 0x5d, 0x7d, 0x23,
                                 0x7d, 0x31, 0x7d, 0x33, 0x41 };
        CHECK(memcmp(out, want, sizeof want) == 0);
        CHECK_EQ_I(out[n - 1], 0x7e);
        CHECK_EQ_I(ppp_hdlc_encoded_size(a, 2, b, 4, 0xffffffffu, 1), n);
    }
    /* ACCM 0: control characters go out raw, flag and escape still stuffed */
    n = ppp_hdlc_encode(out, sizeof out, a, 2, b, 4, 0, 1);
    {
        const uint8_t want[] = { 0x7e, 0x7d, 0x5e, 0x7d, 0x5d, 0x03, 0x11, 0x13, 0x41 };
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
    /* ACCM with only XON/XOFF mapped */
    n = ppp_hdlc_encode(out, sizeof out, a, 2, b, 4, (1u << 0x11) | (1u << 0x13), 1);
    {
        const uint8_t want[] = { 0x7e, 0x7d, 0x5e, 0x7d, 0x5d, 0x03,
                                 0x7d, 0x31, 0x7d, 0x33, 0x41 };
        CHECK(memcmp(out, want, sizeof want) == 0);
    }
    /* no opening flag when asked: first octet is data */
    n = ppp_hdlc_encode(out, sizeof out, NULL, 0, b, 4, 0, 0);
    CHECK_EQ_I(out[0], 0x03);
    CHECK_EQ_I(out[n - 1], 0x7e);
    /* FCS is appended LSB first (and is itself subject to stuffing):
     * a frame whose FCS octets need no escape */
    {
        const uint8_t m[] = { 0x41, 0x42, 0x43 };
        f = (uint16_t)(ref_fcs(0xffff, m, 3) ^ 0xffff);
        n = ppp_hdlc_encode(out, sizeof out, m, 3, NULL, 0, 0, 1);
        if ((f & 0xff) != 0x7e && (f & 0xff) != 0x7d &&
            (f >> 8) != 0x7e && (f >> 8) != 0x7d) {
            CHECK_EQ_I(n, 7);
            CHECK_EQ_I(out[4], f & 0xff);
            CHECK_EQ_I(out[5], f >> 8);
        }
    }
    /* does not fit: returns 0 and reports the needed size separately */
    CHECK_EQ_I(ppp_hdlc_encode(out, 5, a, 2, b, 4, 0, 1), 0);
}

static void test_roundtrip(void)
{
    static uint8_t msg[1600], wire[3400];
    ppp_hdlc_rx_t rx;
    size_t n, w;
    static const uint32_t maps[] = { 0xffffffffu, 0, 0x000a0000u };

    /* every octet value, each ACCM */
    for (int m = 0; m < 3; m++) {
        ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
        rx.rx_accm = 0;
        for (n = 0; n < 256; n++) msg[n] = (uint8_t)n;
        w = ppp_hdlc_encode(wire, sizeof wire, msg, 100, msg + 100, 156, maps[m], 1);
        CHECK(w > 0);
        CHECK_EQ_I(feed(&rx, wire, w), 1);
        CHECK_EQ_I(last_len, 256);
        CHECK(memcmp(last, msg, 256) == 0);
        CHECK_EQ_I(rx.frames_ok, 1);
        CHECK_EQ_I(rx.bad_fcs, 0);
    }
    /* random frames of random length, shared flags between frames */
    ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
    for (int t = 0; t < 200; t++) {
        n = 4 + ((size_t)rnd() * 5) % 1400;
        for (size_t i = 0; i < n; i++) msg[i] = rnd();
        w = ppp_hdlc_encode(wire, sizeof wire, msg, n, NULL, 0, 0xffffffffu, t == 0);
        CHECK_EQ_I(feed(&rx, wire, w), 1);
        CHECK(last_len == n && memcmp(last, msg, n) == 0);
    }
    CHECK_EQ_I(rx.frames_ok, 200);
    CHECK_EQ_I(rx.bad_fcs + rx.runts + rx.overlong + rx.aborts, 0);
}

static void test_invalid(void)
{
    ppp_hdlc_rx_t rx;
    uint8_t msg[64], wire[4000];
    static uint8_t big[1600];
    size_t w;

    for (int i = 0; i < 64; i++) msg[i] = (uint8_t)(0x40 + i % 32);

    /* garbage before the first flag is ignored, not counted */
    ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
    CHECK_EQ_I(feed(&rx, msg, 20), 0);
    CHECK_EQ_I(rx.bad_fcs + rx.runts, 0);

    /* corrupted octet: dropped and counted as FCS error */
    w = ppp_hdlc_encode(wire, sizeof wire, msg, 30, NULL, 0, 0, 1);
    wire[10] ^= 0x01;
    CHECK_EQ_I(feed(&rx, wire, w), 0);
    CHECK_EQ_I(rx.bad_fcs, 1);
    /* the decoder recovers on the very next frame */
    wire[10] ^= 0x01;
    CHECK_EQ_I(feed(&rx, wire, w), 1);
    CHECK_EQ_I(rx.frames_ok, 1);

    /* runts: 1..3 octets between flags; counted as runt, not as FCS error */
    {
        const uint8_t r[] = { 0x7e, 0x41, 0x7e, 0x41, 0x42, 0x7e, 0x41, 0x42, 0x43, 0x7e };
        CHECK_EQ_I(feed(&rx, r, sizeof r), 0);
        CHECK_EQ_I(rx.runts, 3);
        CHECK_EQ_I(rx.bad_fcs, 1);
    }
    /* back-to-back flags are time fill, not frames */
    {
        const uint8_t r[] = { 0x7e, 0x7e, 0x7e, 0x7e };
        CHECK_EQ_I(feed(&rx, r, sizeof r), 0);
        CHECK_EQ_I(rx.runts, 3);
    }
    /* abort: control escape immediately followed by a flag */
    w = ppp_hdlc_encode(wire, sizeof wire, msg, 30, NULL, 0, 0, 1);
    wire[w - 1] = 0x7d;
    wire[w] = 0x7e;
    CHECK_EQ_I(feed(&rx, wire, w + 1), 0);
    CHECK_EQ_I(rx.aborts, 1);
    CHECK_EQ_I(rx.bad_fcs, 1);

    /* overlong: more than max_frame octets; dropped, counted once, and the
     * next frame is received normally */
    ppp_hdlc_rx_init(&rx, 100);
    for (int i = 0; i < 1600; i++) big[i] = (uint8_t)i;
    w = ppp_hdlc_encode(wire, sizeof wire, big, 99, NULL, 0, 0xffffffffu, 1);  /* 101 with FCS */
    CHECK_EQ_I(feed(&rx, wire, w), 0);
    CHECK_EQ_I(rx.overlong, 1);
    CHECK_EQ_I(rx.bad_fcs, 0);
    w = ppp_hdlc_encode(wire, sizeof wire, big, 98, NULL, 0, 0xffffffffu, 1);  /* exactly 100 */
    CHECK_EQ_I(feed(&rx, wire, w), 1);
    CHECK_EQ_I(last_len, 98);
    w = ppp_hdlc_encode(wire, sizeof wire, big, 1600, NULL, 0, 0xffffffffu, 1);
    CHECK_EQ_I(feed(&rx, wire, w), 0);
    CHECK_EQ_I(rx.overlong, 2);
    /* a max_frame larger than the buffer is clamped, never overruns */
    ppp_hdlc_rx_init(&rx, 60000);
    CHECK_EQ_I(rx.max_frame, PPP_HDLC_BUF);
    CHECK_EQ_I(feed(&rx, wire, w), 0);
    CHECK_EQ_I(rx.overlong, 1);
}

static void test_rx_accm(void)
{
    ppp_hdlc_rx_t rx;
    uint8_t msg[40], wire[200], noisy[300];
    size_t w, n = 0;

    for (int i = 0; i < 40; i++) msg[i] = (uint8_t)(i * 7);
    w = ppp_hdlc_encode(wire, sizeof wire, msg, 40, NULL, 0, 0xffffffffu, 1);
    /* a DCE sprinkles XON/XOFF into the stream */
    for (size_t i = 0; i < w; i++) {
        noisy[n++] = wire[i];
        if (i % 5 == 2) noisy[n++] = (i & 1) ? 0x11 : 0x13;
    }
    /* receiving ACCM maps them: they are removed and the frame is good */
    ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
    rx.rx_accm = 0xffffffffu;
    CHECK_EQ_I(feed(&rx, noisy, n), 1);
    CHECK(last_len == 40 && memcmp(last, msg, 40) == 0);
    /* receiving ACCM 0: they are data, so the frame fails its FCS */
    ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
    rx.rx_accm = 0;
    CHECK_EQ_I(feed(&rx, noisy, n), 0);
    CHECK_EQ_I(rx.bad_fcs, 1);
    /* escaped control characters always survive, whatever the map */
    ppp_hdlc_rx_init(&rx, PPP_HDLC_BUF);
    rx.rx_accm = 0xffffffffu;
    CHECK_EQ_I(feed(&rx, wire, w), 1);
    CHECK(memcmp(last, msg, 40) == 0);
}

static void test_header(void)
{
    uint16_t proto = 0;
    size_t off = 0;
    const uint8_t full[] = { 0xff, 0x03, 0x00, 0x21, 0x45 };
    const uint8_t acfc[] = { 0x00, 0x21, 0x45 };
    const uint8_t pfc[]  = { 0xff, 0x03, 0x21, 0x45 };
    const uint8_t both[] = { 0x21, 0x45 };
    const uint8_t lcp[]  = { 0xff, 0x03, 0xc0, 0x21, 0x01 };
    const uint8_t badp[] = { 0xff, 0x03, 0x00, 0x20, 0x45 };  /* even low octet */
    const uint8_t shrt[] = { 0xff, 0x03, 0xc0 };

    CHECK_EQ_I(ppp_hdlc_parse(full, 5, 0, 0, &proto, &off), 0);
    CHECK_EQ_I(proto, 0x0021); CHECK_EQ_I(off, 4);
    CHECK_EQ_I(ppp_hdlc_parse(lcp, 5, 0, 0, &proto, &off), 0);
    CHECK_EQ_I(proto, 0xc021); CHECK_EQ_I(off, 4);

    /* compressed forms are refused until negotiated */
    CHECK_EQ_I(ppp_hdlc_parse(acfc, 3, 0, 0, &proto, &off), -1);
    CHECK_EQ_I(ppp_hdlc_parse(pfc, 4, 0, 0, &proto, &off), -1);
    CHECK_EQ_I(ppp_hdlc_parse(both, 2, 1, 0, &proto, &off), -1);
    CHECK_EQ_I(ppp_hdlc_parse(both, 2, 0, 1, &proto, &off), -1);

    CHECK_EQ_I(ppp_hdlc_parse(acfc, 3, 1, 0, &proto, &off), 0);
    CHECK_EQ_I(proto, 0x0021); CHECK_EQ_I(off, 2);
    CHECK_EQ_I(ppp_hdlc_parse(pfc, 4, 0, 1, &proto, &off), 0);
    CHECK_EQ_I(proto, 0x0021); CHECK_EQ_I(off, 3);
    CHECK_EQ_I(ppp_hdlc_parse(both, 2, 1, 1, &proto, &off), 0);
    CHECK_EQ_I(proto, 0x0021); CHECK_EQ_I(off, 1);
    /* uncompressed forms stay acceptable after negotiation */
    CHECK_EQ_I(ppp_hdlc_parse(full, 5, 1, 1, &proto, &off), 0);
    CHECK_EQ_I(proto, 0x0021); CHECK_EQ_I(off, 4);

    CHECK_EQ_I(ppp_hdlc_parse(badp, 5, 1, 1, &proto, &off), -1);
    CHECK_EQ_I(ppp_hdlc_parse(shrt, 3, 1, 1, &proto, &off), -1);
    CHECK_EQ_I(ppp_hdlc_parse(shrt, 0, 1, 1, &proto, &off), -1);
}

int main(void)
{
    test_fcs();
    test_encode();
    test_roundtrip();
    test_invalid();
    test_rx_accm();
    test_header();
    T_REPORT("test_hdlc");
}
