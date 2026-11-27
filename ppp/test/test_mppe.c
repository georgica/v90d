/* ppp_mppe.c (RFC 3078): the first packet against RFC 3079's RC4 sample, a
 * long two-way exchange that must round-trip, and recovery across a lost
 * packet; stateless, and stateful (what Windows does on a modem link). */
#include <string.h>
#include "test.h"
#include "ppp_mppe.h"

static int eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

/* RFC 3079 section 3.5.3's 128-bit send start key. The packets: "test
 * message" as the first three stateless packets, each under its own key
 * change (RFC 3078 7.3, with the RC4 step) from the initial session key.
 * The ciphertexts come from a separate reference (Python hashlib SHA-1 and
 * an RC4 written apart from ppp_crypto) that also reproduces RFC 3079's own
 * RC4 sample; ppp/README.md says how. */
static const uint8_t START128[16] = { 0x8B,0x7C,0xDC,0x14,0x9B,0x99,0x3A,0x1B,0xA1,0x18,0xCB,0x15,0x3F,0x56,0xDC,0xCB };
static const uint8_t PKT[3][12] = {
    { 0x04,0x1C,0x25,0x5F,0xC0,0x02,0x3D,0xC9,0xA0,0x07,0xEE,0x40 },
    { 0x41,0x78,0x92,0x5D,0x86,0x5C,0x8B,0x2F,0x31,0xBC,0x7C,0xF2 },
    { 0xB8,0x9D,0x43,0xAF,0x11,0xD4,0xAE,0x2C,0xA7,0x28,0xC2,0x76 },
};

static void test_vector(void)
{
    ppp_mppe_t m, rx;
    uint8_t out[64], pt[64], zero[16] = { 0 };
    size_t n;
    int i;

    ppp_mppe_begin(&m, START128, zero, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_begin(&rx, zero, START128, 16, PPP_MPPE_FORCE_NONE);   /* the far end */
    for (i = 0; i < 3; i++) {
        n = ppp_mppe_encrypt(&m, (const uint8_t *)"test message", 12, out, sizeof out);
        CHECK_EQ_I((int)n, 14);
        CHECK_EQ_I(out[0], 0x90);    /* FLUSHED | ENCRYPT */
        CHECK_EQ_I(out[1], i);       /* the first packet is number 0 */
        CHECK(eq(out + 2, PKT[i], 12));
        CHECK_EQ_I(ppp_mppe_decrypt(&rx, out, n, pt, sizeof pt), 12);
        CHECK(eq(pt, (const uint8_t *)"test message", 12));
    }
}

/* a and b are two ends: a's send key is b's receive key and vice versa */
static void pair(ppp_mppe_t *a, ppp_mppe_t *b, size_t keylen, int force)
{
    uint8_t ks[16], kr[16];
    unsigned i;
    for (i = 0; i < 16; i++) { ks[i] = (uint8_t)(i * 7 + 1); kr[i] = (uint8_t)(i * 3 + 9); }
    ppp_mppe_begin(a, ks, kr, keylen, force);
    ppp_mppe_begin(b, kr, ks, keylen, force);
}

static void roundtrip(size_t keylen, int force)
{
    ppp_mppe_t a, b;
    uint8_t msg[200], ct[300], pt[300];
    unsigned i;

    pair(&a, &b, keylen, force);
    for (i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 31 + 5);
    for (i = 0; i < 200; i++) {
        size_t ml = 1 + (i % 160);
        size_t n = ppp_mppe_encrypt(&a, msg, ml, ct, sizeof ct);
        int d;
        CHECK_EQ_I((int)n, (int)ml + 2);
        d = ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt);
        CHECK_EQ_I(d, (int)ml);
        CHECK(eq(pt, msg, ml));
    }
    /* the other direction works too */
    {
        size_t n = ppp_mppe_encrypt(&b, msg, 64, ct, sizeof ct);
        int d = ppp_mppe_decrypt(&a, ct, n, pt, sizeof pt);
        CHECK_EQ_I(d, 64);
        CHECK(eq(pt, msg, 64));
    }
}

/* The short keys (40- and 56-bit, RFC 3079 3.3/3.4: the first 8 octets
 * of the start key, the first octets forced), the first three stateless
 * packets of "test message" from START128. The ciphertexts come from a
 * reference written apart from ppp_crypto (Python hashlib SHA-1 and its
 * own RC4); its 40-bit initial key is RFC 3079 3.5's D1 26 9E C4 9F A6 2E 3E. */
static void test_short_vectors(void)
{
    static const uint8_t c40[3][12] = {
        { 0xea,0x98,0xa2,0x12,0xb4,0x83,0x25,0xfc,0x44,0x3f,0xba,0x20 },
        { 0x86,0x47,0x2d,0x18,0x6d,0x94,0xf4,0x89,0xc8,0x3f,0x58,0x96 },
        { 0x43,0x4e,0xf3,0x4a,0xc3,0xa6,0x1d,0xfc,0xfc,0xb0,0x09,0x59 },
    };
    static const uint8_t c56[3][12] = {
        { 0x1c,0x9e,0x80,0x2f,0x85,0x16,0xe2,0x2a,0x7c,0xe3,0x82,0x70 },
        { 0xc6,0x78,0x81,0x72,0x90,0xda,0xcf,0xc6,0x1a,0xcd,0x39,0xa6 },
        { 0xf9,0x78,0xfc,0xc3,0xe9,0xab,0x49,0xb5,0x4f,0x5f,0xfa,0x83 },
    };
    ppp_mppe_t m;
    uint8_t out[64], zero[16] = { 0 };
    int i, k;

    for (k = 0; k < 2; k++) {
        ppp_mppe_begin(&m, START128, zero, 8, k ? PPP_MPPE_FORCE_56 : PPP_MPPE_FORCE_40);
        for (i = 0; i < 3; i++) {
            size_t n = ppp_mppe_encrypt(&m, (const uint8_t *)"test message", 12, out, sizeof out);
            CHECK_EQ_I((int)n, 14);
            CHECK(eq(out + 2, k ? c56[i] : c40[i], 12));
        }
    }
}

static void test_roundtrip(void)
{
    roundtrip(16, PPP_MPPE_FORCE_NONE);     /* 128-bit */
    roundtrip(8, PPP_MPPE_FORCE_40);        /* 40-bit */
    roundtrip(8, PPP_MPPE_FORCE_56);        /* 56-bit */
}

static void test_loss(void)
{
    ppp_mppe_t a, b;
    uint8_t msg[32], ct[64], pt[64];
    unsigned i;
    size_t n;
    int d;

    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    for (i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i + 1);

    n = ppp_mppe_encrypt(&a, msg, 32, ct, sizeof ct);          /* count 0 */
    d = ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt);
    CHECK_EQ_I(d, 32); CHECK(eq(pt, msg, 32));
    (void)ppp_mppe_encrypt(&a, msg, 32, ct, sizeof ct);        /* count 1, lost */
    n = ppp_mppe_encrypt(&a, msg, 32, ct, sizeof ct);          /* count 2 */
    d = ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt);            /* receiver catches up */
    CHECK_EQ_I(d, 32); CHECK(eq(pt, msg, 32));

    /* a replay of the last packet (no key change to make) is dropped */
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);
    /* a plaintext (unencrypted) frame is refused */
    ct[0] &= (uint8_t)~0x10;
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);
}

/* the receive window: up to PPP_MPPE_WINDOW lost packets in a row are
 * stepped over; a count further ahead (or behind) is dropped and leaves the
 * receiver where it was */
static void test_window(void)
{
    static ppp_mppe_t a, b, a2;
    uint8_t msg[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, ct[16], pt[16];
    size_t n;
    unsigned i;

    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    for (i = 0; i < PPP_MPPE_CREDIT_MAX; i++)                   /* lost: as many as the credit covers */
        (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    CHECK(eq(pt, msg, 8));
    /* the credit is spent: a peer that jumps ahead in every packet is
     * followed a few key changes a packet, and not further */
    {
        int dropped = 0, taken = 0;
        for (i = 0; i < 40; i++) {
            unsigned j;
            for (j = 0; j < 20; j++) (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);   /* 20 lost each time */
            n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
            if (ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt) == 8) taken++; else dropped++;
        }
        CHECK(dropped > 30 && taken < 10);
    }
    /* ordinary traffic earns it back: a burst of loss is followed again */
    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    for (i = 0; i < 100; i++) {
        n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
        CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    }
    for (i = 0; i < 300; i++) (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);

    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    a2 = a;
    for (i = 0; i < PPP_MPPE_WINDOW; i++)                       /* one more lost */
        (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);  /* too far: dropped */
    n = ppp_mppe_encrypt(&a2, msg, 8, ct, sizeof ct);           /* the next in line still works */
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    CHECK(eq(pt, msg, 8));
}

/* A long run of lost packets (a noise burst, a retrain) is caught up on
 * over the next packets, paid out of the credit a few key changes at a
 * time, however long the run was up to the window: the receiver never
 * stays out of step for good. Past the window the packets are dropped and
 * counted (ppp_mppe_rx_late) for the caller to end the link. */
static void test_long_loss(void)
{
    static ppp_mppe_t a, b;
    static const unsigned runs[] = { 513, 514, 600, 1000, 1500, 2000, PPP_MPPE_WINDOW - 1 };
    uint8_t msg[8] = { 9, 8, 7, 6, 5, 4, 3, 2 }, ct[16], pt[16];
    unsigned r, i, first, ok;
    size_t n;

    for (r = 0; r < sizeof runs / sizeof runs[0]; r++) {
        pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
        for (i = 0; i < 100; i++) {                                     /* ordinary traffic first */
            n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
            CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
        }
        for (i = 0; i < runs[r]; i++)
            (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);            /* lost */
        first = 0; ok = 0;
        for (i = 1; i <= 600; i++) {
            int d;
            msg[0] = (uint8_t)i;
            n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
            d = ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt);
            if (d == 8) { CHECK(eq(pt, msg, 8)); ok++; if (!first) first = i; }
            else CHECK_EQ_I(d, -1);
        }
        CHECK(first > 0 && first <= 300);                               /* back in step */
        CHECK_EQ_I(ok, 600 - first + 1);                                /* and stays there */
        CHECK_EQ_I(ppp_mppe_rx_late(&b), 0);
    }

    /* past the window: dropped and counted, one by one; a packet in line
     * clears the count */
    {
        static ppp_mppe_t a2;
        pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
        n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
        CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
        a2 = a;
        for (i = 0; i < PPP_MPPE_WINDOW; i++)
            (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
        for (i = 1; i <= 3; i++) {
            n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
            CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);
            CHECK_EQ_I(ppp_mppe_rx_late(&b), i);
        }
        n = ppp_mppe_encrypt(&a2, msg, 8, ct, sizeof ct);
        CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
        CHECK_EQ_I(ppp_mppe_rx_late(&b), 0);
    }

    /* stateful the same: out of step past the window, the packet meant to
     * bring it back is too late, and each one is counted */
    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_stateful(&a, 1, 1);
    ppp_mppe_stateful(&b, 1, 1);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), 8);
    for (i = 0; i < PPP_MPPE_WINDOW + 10; i++)
        (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    ppp_mppe_tx_resync(&a);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);
    CHECK_EQ_I(ppp_mppe_rx_late(&b), 1);
    /* a reset clears it */
    ppp_mppe_reset(&b);
    CHECK_EQ_I(ppp_mppe_rx_late(&b), 0);
}

/* ---- stateful (RFC 3078 7.2, 8.2) ---- */

/* The first stateful packet is RC4 under the initial key itself, from the
 * start of its stream: exactly RFC 3079 3.5.3's "test message" sample. The
 * second continues the same stream (no key change, no FLUSHED). */
static void test_stateful_vector(void)
{
    static const uint8_t ct128[12] = { 0x81,0x84,0x83,0x17,0xDF,0x68,0x84,0x62,0x72,0xFB,0x5A,0xBE };
    ppp_mppe_t m, rx;
    ppp_rc4_t r;
    uint8_t out[64], pt[64], zero[16] = { 0 }, ks[16], two[24];
    size_t n;

    ppp_mppe_begin(&m, START128, zero, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_begin(&rx, zero, START128, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_stateful(&m, 1, 1);
    ppp_mppe_stateful(&rx, 1, 1);
    n = ppp_mppe_encrypt(&m, (const uint8_t *)"test message", 12, out, sizeof out);
    CHECK_EQ_I((int)n, 14);
    CHECK_EQ_I(out[0], 0x10);        /* ENCRYPT only, count 0 */
    CHECK_EQ_I(out[1], 0);
    CHECK(eq(out + 2, ct128, 12));
    CHECK_EQ_I(ppp_mppe_decrypt(&rx, out, n, pt, sizeof pt), 12);
    CHECK(eq(pt, (const uint8_t *)"test message", 12));
    /* packet 1: octets 12..23 of the same keystream */
    n = ppp_mppe_encrypt(&m, (const uint8_t *)"test message", 12, out, sizeof out);
    CHECK_EQ_I(out[0], 0x10);
    CHECK_EQ_I(out[1], 1);
    ppp_mppe_initial_key(START128, ks, 16, PPP_MPPE_FORCE_NONE);
    ppp_rc4_init(&r, ks, 16);
    memcpy(two, "test messagetest message", 24);
    ppp_rc4(&r, two, two, 24);
    CHECK(eq(out + 2, two + 12, 12));
    CHECK_EQ_I(ppp_mppe_decrypt(&rx, out, n, pt, sizeof pt), 12);
    CHECK(eq(pt, (const uint8_t *)"test message", 12));
}

/* 9000 packets one way (twice round the 4096 counts, 35 key changes) with
 * losses, replays and resets on the way; every packet that is accepted
 * must decrypt to what was sent, and the stream must come back each time */
static void test_stateful_long(void)
{
    ppp_mppe_t a, b;
    uint8_t msg[100], ct[200], pt[200], old[200];
    unsigned i, sent = 0, ok = 0, bad = 0, resets = 0, flags = 0, flushed = 0;
    size_t n, oldn = 0;
    int d;

    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_stateful(&a, 1, 1);
    ppp_mppe_stateful(&b, 1, 1);
    for (i = 0; i < 9000; i++) {
        msg[0] = (uint8_t)i; msg[1] = (uint8_t)(i >> 8);
        memset(msg + 2, (int)(i * 13), sizeof msg - 2);
        n = ppp_mppe_encrypt(&a, msg, sizeof msg, ct, sizeof ct);
        sent++;
        if ((ct[1] == 0xff)) flags++;
        if (ct[0] & 0x80) flushed++;
        if (i % 997 == 500 || (i >= 3000 && i < 3400)) {
            /* lost (one, or a run of 400 across key changes) */
            continue;
        }
        if (i == 7000) {
            memcpy(old, ct, n);
            oldn = n;
        }
        d = ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt);
        if (d == (int)sizeof msg && eq(pt, msg, sizeof msg)) ok++;
        else if (d >= 0) bad++;
        if (d < 0 && ppp_mppe_rx_out_of_step(&b)) {
            /* the receiver asks (once) for a reset; the sender resyncs */
            ppp_mppe_tx_resync(&a);
            resets++;
        }
        if (i == 7100) {
            /* an old packet again: dropped, and the stream goes on */
            CHECK_EQ_I(ppp_mppe_decrypt(&b, old, oldn, pt, sizeof pt), -1);
            CHECK_EQ_I(ppp_mppe_rx_out_of_step(&b), 0);
        }
    }
    CHECK_EQ_I(bad, 0);
    CHECK(resets >= 9 && resets <= 12);
    CHECK(ok > sent - 400 - 9 * 2 - 9 * 256);
    CHECK(flags > 0 && flushed == flags);  /* FLUSHED only on flag packets */
    /* the last packet came through */
    CHECK_EQ_I(ppp_mppe_rx_out_of_step(&b), 0);

    /* a resync is a key change, never RC4 again on a key already used: the
     * packet after a reset is a flag packet */
    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_stateful(&a, 1, 1);
    for (i = 0; i < 10; i++)
        (void)ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    ppp_mppe_tx_resync(&a);
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ct[0], 0x90);
    CHECK_EQ_I(ct[1], 0xff);
    ppp_mppe_tx_resync(&a);                  /* again: to the next one */
    n = ppp_mppe_encrypt(&a, msg, 8, ct, sizeof ct);
    CHECK_EQ_I(ct[0], 0x91);
    CHECK_EQ_I(ct[1], 0xff);
    (void)n;
}

/* stateful one way, stateless the other: each direction keeps its own mode */
static void test_mixed(void)
{
    ppp_mppe_t a, b;
    uint8_t msg[40], ct[80], pt[80];
    unsigned i;
    size_t n;

    memset(msg, 0x5a, sizeof msg);
    pair(&a, &b, 16, PPP_MPPE_FORCE_NONE);
    ppp_mppe_stateful(&a, 1, 0);
    ppp_mppe_stateful(&b, 0, 1);
    for (i = 0; i < 600; i++) {
        n = ppp_mppe_encrypt(&a, msg, sizeof msg, ct, sizeof ct);
        CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), (int)sizeof msg);
        n = ppp_mppe_encrypt(&b, msg, sizeof msg, ct, sizeof ct);
        CHECK_EQ_I(ct[0] & 0x80, 0x80);      /* stateless: every packet FLUSHED */
        CHECK_EQ_I(ppp_mppe_decrypt(&a, ct, n, pt, sizeof pt), (int)sizeof msg);
    }
}

int main(void)
{
    test_vector();
    test_stateful_vector();
    test_stateful_long();
    test_mixed();
    test_roundtrip();
    test_short_vectors();
    test_loss();
    test_window();
    test_long_loss();
    T_REPORT("mppe");
}
