/* Modulus encoder / decoder, V.92 6.4.1.
 *
 * The expected K0..K11 of the worked examples were computed by hand from the
 * six steps of 6.4.1 (the arithmetic is in the comments). The 72-bit case
 * uses Mi = 64, where the mixed-radix digits are simply 6-bit groups of R0,
 * so it can be checked without multi-precision arithmetic in the test. */
#include <string.h>
#include "test.h"
#include "v92_mod.h"

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void int_bits(unsigned long v, int K, uint8_t *b)
{
    int i;
    for (i = 0; i < K; i++) b[i] = (uint8_t)((v >> i) & 1);
}

static unsigned long bits_int(const uint8_t *b, int K)
{
    unsigned long v = 0;
    int i;
    for (i = 0; i < K; i++) v |= (unsigned long)b[i] << i;
    return v;
}

static int same(const uint8_t *a, const uint8_t *b, int n) { return memcmp(a, b, (size_t)n) == 0; }

/* M = 5*3*4*2*3*5*2^6 = 115200 >= 2^16, so K = 16. (M - 1) / 2 = 57599.5. */
static const uint8_t M1[12] = { 5, 3, 4, 2, 3, 5, 2, 2, 2, 2, 2, 2 };

static void test_worked(void)
{
    /* Frame 1: R = 50000 <= 57599.5, s = 0, d(-1) = 0 so R0 = R, d(0) = 0.
     *   50000 = 5*10000 + 0; 10000 = 3*3333 + 1; 3333 = 4*833 + 1;
     *   833 = 2*416 + 1; 416 = 3*138 + 2; 138 = 5*27 + 3; 27 = 2*13 + 1;
     *   13 = 2*6 + 1; 6 = 2*3 + 0; 3 = 2*1 + 1; 1 = 2*0 + 1; 0.
     * Frame 2: R = 60000 > 57599.5, s = 1; d(0) = 0 so R0 = R; d(1) = 1.
     *   60000 = 5*12000 + 0; 12000 = 3*4000 + 0; 4000 = 4*1000 + 0;
     *   1000 = 2*500 + 0; 500 = 3*166 + 2; 166 = 5*33 + 1; 33 = 2*16 + 1;
     *   16, 8, 4, 2 even; 1 = 2*0 + 1.
     * Frame 3: R = 50000, d(1) = 1 so R0 = 115199 - 50000 = 65199; s = 0,
     *   d(2) = 1. 65199 = 5*13039 + 4; 13039 = 3*4346 + 1; 4346 = 4*1086 + 2;
     *   1086 = 2*543 + 0; 543 = 3*181 + 0; 181 = 5*36 + 1; 36 = 2*18 + 0;
     *   18 = 2*9 + 0; 9 = 2*4 + 1; 4 = 2*2 + 0; 2 = 2*1 + 0; 1.
     *   (each digit is Mi - 1 - the digit of frame 1, as it must be)
     * Frame 4: R = 60000, d(2) = 1 so R0 = 55199; s = 1, d(3) = 0.
     *   55199 = 5*11039 + 4; 11039 = 3*3679 + 2; 3679 = 4*919 + 3;
     *   919 = 2*459 + 1; 459 = 3*153 + 0; 153 = 5*30 + 3; 30 = 2*15 + 0;
     *   15 = 2*7 + 1; 7 = 2*3 + 1; 3 = 2*1 + 1; 1 = 2*0 + 1; 0.
     * Frame 5: R = 1, d(3) = 0 so R0 = 1: K0 = 1, the rest 0. */
    static const unsigned long R[5] = { 50000, 60000, 50000, 60000, 1 };
    static const uint8_t want[5][12] = {
        { 0, 1, 1, 1, 2, 3, 1, 1, 0, 1, 1, 0 },
        { 0, 0, 0, 0, 2, 1, 1, 0, 0, 0, 0, 1 },
        { 4, 1, 2, 0, 0, 1, 0, 0, 1, 0, 0, 1 },
        { 4, 2, 3, 1, 0, 3, 0, 1, 1, 1, 1, 0 },
        { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    };
    v92_mod_t e, d;
    uint8_t bits[16], out[16], k[12];
    int f, i;

    CHECK_EQ_I(v92_mod_init(&e, M1, 16), 0);
    CHECK_EQ_I(v92_mod_init(&d, M1, 16), 0);
    for (f = 0; f < 5; f++) {
        int_bits(R[f], 16, bits);
        memset(k, 0xEE, sizeof k);
        v92_mod_encode(&e, bits, k);
        for (i = 0; i < 12; i++) CHECK_EQ_I(k[i], want[f][i]);
        memset(out, 0xEE, sizeof out);
        CHECK_EQ_I(v92_mod_decode(&d, want[f], out), 0);
        CHECK_EQ_I(bits_int(out, 16), R[f]);
    }
}

/* Mi = 64: M = 2^72, K = 72, Ki = bits 6i .. 6i+5 of R0. s = b71. */
static void test_pow2(void)
{
    uint8_t M[12], bits[72], out[72], k[12];
    v92_mod_t e, d;
    uint32_t seed = 7;
    int f, i, j, dprev = 0;

    memset(M, 64, sizeof M);
    CHECK_EQ_I(v92_mod_init(&e, M, 72), 0);
    CHECK_EQ_I(v92_mod_init(&d, M, 72), 0);
    for (f = 0; f < 200; f++) {
        for (i = 0; i < 72; i++) bits[i] = (uint8_t)(lcg(&seed) & 1);
        v92_mod_encode(&e, bits, k);
        for (i = 0; i < 12; i++) {
            unsigned v = 0;
            /* R0 = R, or its complement 2^72 - 1 - R when d(f - 1) = 1 */
            for (j = 0; j < 6; j++) v |= (unsigned)(bits[6 * i + j] ^ dprev) << j;
            CHECK_EQ_I(k[i], v);
        }
        dprev ^= bits[71];
        CHECK_EQ_I(v92_mod_decode(&d, k, out), 0);
        CHECK(same(bits, out, 72));
    }
}

/* M = 3 * 3 = 9 (odd), K = 3. (M - 1) / 2 = 4 is the midpoint: R = 4 has
 * s = 0 and R0 = 4 whatever d(f - 1) is, so the sign of R0 says nothing
 * about d(f) there and the decoder has to keep its previous value. */
static void test_midpoint(void)
{
    static const uint8_t M[12] = { 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    /* R = 6: s = 1, d(-1) = 0: R0 = 6 = 3*2 + 0;         d(0) = 1
     * R = 4: s = 0, d(0) = 1: R0 = 8 - 4 = 4 = 3*1 + 1;  d(1) = 1
     * R = 2: s = 0, d(1) = 1: R0 = 8 - 2 = 6 = 3*2 + 0;  d(2) = 1
     * R = 7: s = 1, d(2) = 1: R0 = 8 - 7 = 1 = 3*0 + 1;  d(3) = 0
     * R = 3: s = 0, d(3) = 0: R0 = 3 = 3*1 + 0;          d(4) = 0
     * R = 4: s = 0, d(4) = 0: R0 = 4 = 3*1 + 1;          d(5) = 0
     * R = 5: s = 1, d(5) = 0: R0 = 5 = 3*1 + 2;          d(6) = 1
     * R = 0: s = 0, d(6) = 1: R0 = 8 = 3*2 + 2 */
    static const unsigned long R[8] = { 6, 4, 2, 7, 3, 4, 5, 0 };
    static const uint8_t want[8][2] = {
        { 0, 2 }, { 1, 1 }, { 0, 2 }, { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 }, { 2, 2 } };
    v92_mod_t e, d, dinv;
    uint8_t bits[3], out[3], k[12], kinv[12];
    int f, i;

    CHECK_EQ_I(v92_mod_init(&e, M, 3), 0);
    CHECK_EQ_I(v92_mod_init(&d, M, 3), 0);
    CHECK_EQ_I(v92_mod_init(&dinv, M, 3), 0);
    for (f = 0; f < 8; f++) {
        int_bits(R[f], 3, bits);
        v92_mod_encode(&e, bits, k);
        CHECK_EQ_I(k[0], want[f][0]);
        CHECK_EQ_I(k[1], want[f][1]);
        for (i = 2; i < 12; i++) CHECK_EQ_I(k[i], 0);
        CHECK_EQ_I(v92_mod_decode(&d, k, out), 0);
        CHECK_EQ_I(bits_int(out, 3), R[f]);
        /* inverted line: every Ki arrives as Mi - 1 - Ki. The first frame
         * comes out complemented (8 - 6 = 2), every later one is right. */
        for (i = 0; i < 12; i++) kinv[i] = (uint8_t)(M[i] - 1 - k[i]);
        (void)v92_mod_decode(&dinv, kinv, out);
        CHECK_EQ_I(bits_int(out, 3), f == 0 ? 2 : R[f]);
    }
    /* R0 = 8 with d(f - 1) = 0 would be R = 8 >= 2^3: not an encoder output */
    v92_mod_reset(&d);
    k[0] = 2; k[1] = 2;
    CHECK_EQ_I(v92_mod_decode(&d, k, out), 1);
    /* a digit out of range is reported too */
    v92_mod_reset(&d);
    k[0] = 3; k[1] = 0;
    CHECK_EQ_I(v92_mod_decode(&d, k, out), 1);
}

static void test_init(void)
{
    static const uint8_t Mz[12] = { 5, 3, 4, 2, 0, 5, 2, 2, 2, 2, 2, 2 };
    uint8_t M[12];
    v92_mod_t s;
    CHECK_EQ_I(v92_mod_init(&s, M1, 16), 0);
    CHECK_EQ_I(v92_mod_init(&s, M1, 17), V92_ERR_RANGE);   /* 2^17 = 131072 > 115200 */
    CHECK_EQ_I(v92_mod_init(&s, M1, 0), V92_ERR_RANGE);
    CHECK_EQ_I(v92_mod_init(&s, Mz, 8), V92_ERR_RANGE);
    memset(M, 64, sizeof M);
    CHECK_EQ_I(v92_mod_init(&s, M, 72), 0);                /* 2^72 <= 2^72 */
    M[11] = 63;
    CHECK_EQ_I(v92_mod_init(&s, M, 72), V92_ERR_RANGE);    /* 63 * 2^66 < 2^72 */
    CHECK_EQ_I(v92_mod_init(&s, M, 71), 0);
    memset(M, 255, sizeof M);
    CHECK_EQ_I(v92_mod_init(&s, M, 73), V92_ERR_RANGE);    /* K > 72 */
    CHECK_EQ_I(v92_mod_init(&s, M, 72), 0);
}

/* Random frames through uneven moduli at every K the product allows, straight
 * and with the line inverted; then error recovery. */
static void test_random(void)
{
    static const uint8_t Ma[12] = { 255, 254, 253, 127, 255, 3, 97, 128, 200, 1, 77, 31 };
    /* log2 of the product is about 72.6 */
    uint8_t Mb[12];
    v92_mod_t e, d, dinv;
    uint8_t bits[72], out[72], k[12], kinv[12];
    uint32_t seed = 99;
    int K, f, i, bad;

    for (K = 1; K <= 72; K++) {
        CHECK_EQ_I(v92_mod_init(&e, Ma, K), 0);
        CHECK_EQ_I(v92_mod_init(&d, Ma, K), 0);
        CHECK_EQ_I(v92_mod_init(&dinv, Ma, K), 0);
        bad = 0;
        for (f = 0; f < 50; f++) {
            for (i = 0; i < K; i++) bits[i] = (uint8_t)(lcg(&seed) & 1);
            v92_mod_encode(&e, bits, k);
            for (i = 0; i < 12; i++) if (k[i] >= Ma[i]) bad++;
            if (v92_mod_decode(&d, k, out) != 0 || !same(bits, out, K)) bad++;
            for (i = 0; i < 12; i++) kinv[i] = (uint8_t)(Ma[i] - 1 - k[i]);
            (void)v92_mod_decode(&dinv, kinv, out);
            if (f > 0 && !same(bits, out, K)) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }

    /* all moduli 255, the largest M there is */
    memset(Mb, 255, sizeof Mb);
    CHECK_EQ_I(v92_mod_init(&e, Mb, 72), 0);
    CHECK_EQ_I(v92_mod_init(&d, Mb, 72), 0);
    bad = 0;
    for (f = 0; f < 300; f++) {
        for (i = 0; i < 72; i++) bits[i] = (uint8_t)(lcg(&seed) & 1);
        v92_mod_encode(&e, bits, k);
        if (f == 100) {            /* one corrupted frame */
            k[11] = (uint8_t)(254 - k[11]);
            (void)v92_mod_decode(&d, k, out);
            continue;
        }
        if (f == 101) {            /* the next may be wrong (wrong d), no more */
            (void)v92_mod_decode(&d, k, out);
            continue;
        }
        if (v92_mod_decode(&d, k, out) != 0 || !same(bits, out, 72)) bad++;
    }
    CHECK_EQ_I(bad, 0);
}

int main(void)
{
    test_worked();
    test_pow2();
    test_midpoint();
    test_init();
    test_random();
    T_REPORT("test_mod");
}
