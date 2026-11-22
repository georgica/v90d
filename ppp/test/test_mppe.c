/* ppp_mppe.c (RFC 3078 stateless): the first packet against RFC 3079's RC4
 * sample, a long two-way exchange that must round-trip, and recovery across a
 * lost packet. */
#include <string.h>
#include "test.h"
#include "ppp_mppe.h"

static int eq(const uint8_t *a, const uint8_t *b, size_t n) { return memcmp(a, b, n) == 0; }

/* RFC 3079 section 3.5.3: 128-bit send start key, and the RC4 of "test
 * message" under the first session key derived from it. */
static const uint8_t START128[16] = { 0x8B,0x7C,0xDC,0x14,0x9B,0x99,0x3A,0x1B,0xA1,0x18,0xCB,0x15,0x3F,0x56,0xDC,0xCB };
static const uint8_t CT128[12]    = { 0x81,0x84,0x83,0x17,0xDF,0x68,0x84,0x62,0x72,0xFB,0x5A,0xBE };

static void test_vector(void)
{
    ppp_mppe_t m;
    uint8_t out[64], recv[16] = { 0 };
    size_t n;

    ppp_mppe_begin(&m, START128, recv, 16, PPP_MPPE_FORCE_NONE);
    n = ppp_mppe_encrypt(&m, (const uint8_t *)"test message", 12, out, sizeof out);
    CHECK_EQ_I((int)n, 14);
    CHECK_EQ_I(out[0], 0x90);    /* FLUSHED | ENCRYPT, count 0 */
    CHECK_EQ_I(out[1], 0x00);
    CHECK(eq(out + 2, CT128, 12));
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

    /* a plaintext (unencrypted) frame is refused */
    ct[0] &= (uint8_t)~0x10;
    CHECK_EQ_I(ppp_mppe_decrypt(&b, ct, n, pt, sizeof pt), -1);
}

int main(void)
{
    test_vector();
    test_roundtrip();
    test_loss();
    T_REPORT("mppe");
}
