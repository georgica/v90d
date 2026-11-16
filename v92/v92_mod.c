/* V.92 6.4.1 modulus encoder and decoder. See v92_mod.h.
 *
 * Numbers are 96 bits in three 32-bit limbs, least significant first. The
 * encoder divides by the moduli (16 bits at a time, so that every division
 * is a plain 32-bit one); the decoder, which is the side that runs on the
 * target, only multiplies (Horner) and compares. */
#include <string.h>
#include "v92_mod.h"

static int cmp3(const uint32_t *a, const uint32_t *b)
{
    int i;
    for (i = 2; i >= 0; i--) {
        if (a[i] > b[i]) return 1;
        if (a[i] < b[i]) return -1;
    }
    return 0;
}

/* r = a - b, a >= b */
static void sub3(uint32_t *r, const uint32_t *a, const uint32_t *b)
{
    uint32_t borrow = 0;
    int i;
    for (i = 0; i < 3; i++) {
        uint64_t t = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)t;
        borrow = (uint32_t)(t >> 63);
    }
}

/* a = a * m + add; returns the carry out of 96 bits */
static uint32_t muladd3(uint32_t *a, uint32_t m, uint32_t add)
{
    uint64_t c = add;
    int i;
    for (i = 0; i < 3; i++) {
        c += (uint64_t)a[i] * m;
        a[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

/* a = a / m, returns a mod m; 1 <= m <= 255 */
static unsigned divmod3(uint32_t *a, unsigned m)
{
    uint32_t rem = 0;
    int i;
    for (i = 2; i >= 0; i--) {
        uint32_t hi = (rem << 16) | (a[i] >> 16);
        uint32_t lo;
        rem = hi % m;
        lo = (rem << 16) | (a[i] & 0xFFFF);
        rem = lo % m;
        a[i] = ((hi / m) << 16) | (lo / m);
    }
    return rem;
}

int v92_mod_init(v92_mod_t *s, const uint8_t M[V92_DFRAME], int K)
{
    static const uint32_t one[3] = { 1, 0, 0 };
    uint32_t p[3] = { 1, 0, 0 }, t[3] = { 0, 0, 0 };
    int i;

    memset(s, 0, sizeof *s);
    if (K < 1 || K > V92_MAX_K) return V92_ERR_RANGE;
    for (i = 0; i < V92_DFRAME; i++) {
        if (M[i] == 0) return V92_ERR_RANGE;
        (void)muladd3(p, M[i], 0);          /* 255^12 < 2^96: no carry */
    }
    t[K >> 5] = (uint32_t)1 << (K & 31);    /* 2^K */
    if (cmp3(p, t) < 0) return V92_ERR_RANGE;

    s->K = (uint8_t)K;
    memcpy(s->M, M, V92_DFRAME);
    s->odd = (uint8_t)(p[0] & 1);
    sub3(s->m1, p, one);
    s->half[0] = (s->m1[0] >> 1) | (s->m1[1] << 31);
    s->half[1] = (s->m1[1] >> 1) | (s->m1[2] << 31);
    s->half[2] = s->m1[2] >> 1;
    return 0;
}

void v92_mod_reset(v92_mod_t *s)
{
    s->d = 0;
}

void v92_mod_encode(v92_mod_t *s, const uint8_t *bits, uint8_t k[V92_DFRAME])
{
    uint32_t r[3] = { 0, 0, 0 }, r0[3];
    unsigned sf;
    int i;

    for (i = 0; i < s->K; i++)                       /* 1) */
        r[i >> 5] |= (uint32_t)(bits[i] & 1) << (i & 31);
    /* 2) R > (M - 1) / 2 is R > floor((M - 1) / 2) for an integer R */
    sf = cmp3(r, s->half) > 0;
    if (s->d) sub3(r0, s->m1, r);                    /* 4), with d(f - 1) */
    else memcpy(r0, r, sizeof r0);
    s->d ^= (uint8_t)sf;                             /* 3) */
    for (i = 0; i < V92_DFRAME; i++)                 /* 5) */
        k[i] = (uint8_t)divmod3(r0, s->M[i]);
}

int v92_mod_decode(v92_mod_t *s, const uint8_t k[V92_DFRAME], uint8_t *bits)
{
    uint32_t r0[3] = { 0, 0, 0 }, r[3];
    int i, c, bad = 0;

    /* R0 = K0 + M0 (K1 + M1 (K2 + ...)) */
    for (i = V92_DFRAME - 1; i >= 0; i--) {
        unsigned ki = k[i];
        if (ki >= s->M[i]) { bad = 1; ki = s->M[i] - 1u; }
        (void)muladd3(r0, s->M[i], ki);              /* stays below M */
    }
    c = cmp3(r0, s->half);
    if (s->d) sub3(r, s->m1, r0);
    else memcpy(r, r0, sizeof r);
    /* d(f) is the sign of R0, except at the midpoint of an odd M, where
     * s(f) = 0 and d(f) = d(f - 1) */
    if (!(s->odd && c == 0)) s->d = (uint8_t)(c > 0);

    for (i = 0; i < s->K; i++)
        bits[i] = (uint8_t)((r[i >> 5] >> (i & 31)) & 1);
    for (i = s->K; i < 96; i++)
        if ((r[i >> 5] >> (i & 31)) & 1) bad = 1;    /* R >= 2^K */
    return bad;
}
