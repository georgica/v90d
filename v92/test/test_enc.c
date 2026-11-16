/* Upstream encoder model, V.92 6.4.
 *
 * Expected values are worked out by hand in the comments from: 6.4.1 and
 * 6.4.2 of V.92, the scrambler recurrence of clause 7/V.34 (GPA), Figure 9
 * and Table 13 of V.34 as printed, and Figures 10 and 11 of V.34. The
 * 64-state encoder (Figure 12) is only checked for consistency: its reading
 * is the one of ../v34, which has its own tests. */
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_enc.h"
#include "v34_priv.h"

static int get_ones(void *u) { (void)u; return 1; }
static int get_zeros(void *u) { (void)u; return 0; }
static int get_rand(void *u) { return (int)(tu_lcg((uint32_t *)u) & 1); }

/* ---- 6.4.3 --------------------------------------------------------------- */

static void test_inverse_map(void)
{
    /* Figure 9/V.34, rows im = 3, 1, -1, -3, columns re = -3, -1, 1, 3:
     *    001 110 101 010
     *    100 011 000 111
     *    101 010 001 110
     *    000 111 100 011
     * index y -> coordinate 2y + 1. Table 13 gives Y4 Y3 Y2 Y1. */
    static const struct { int16_t y[4]; unsigned want; } t[] = {
        { {  0,  0,  0,  0 }, 0x0 },   /* (1,1) 000, (1,1) 000 -> 0000 */
        { { -1, -1,  1,  1 }, 0x4 },   /* (-1,-1) 010, (3,3) 010 -> 0100 */
        { {  0, -2, -1, -1 }, 0x9 },   /* (1,-3) 100, (-1,-1) 010 -> 1001 */
        { {  1,  0,  0, -2 }, 0x6 },   /* (3,1) 111, (1,-3) 100 -> 0110 */
        { {  1,  0,  0, -1 }, 0xF },   /* (3,1) 111, (1,-1) 001 -> 1111 */
        { { -2,  0,  0,  0 }, 0x8 },   /* (-3,1) 100, (1,1) 000 -> 1000 */
        { {  0,  1, -2,  1 }, 0xA },   /* (1,3) 101, (-3,3) 001 -> 1010 */
    };
    unsigned n;
    int16_t y[4];
    for (n = 0; n < sizeof t / sizeof t[0]; n++)
        CHECK_EQ_I(v92_inverse_map(t[n].y), t[n].want);

    /* the subsets are cosets of {(4i, 4j), i + j even} in coordinates, that
     * is of {(2i, 2j), i + j even} in indices */
    y[0] = 4; y[1] = 0; y[2] = 0; y[3] = -4;
    CHECK_EQ_I(v92_inverse_map(y), 0x0);
    y[0] = 2; y[1] = 2; y[2] = -2; y[3] = 2;
    CHECK_EQ_I(v92_inverse_map(y), 0x0);
    y[0] = 2; y[1] = 0; y[2] = 0; y[3] = 0;       /* (5,1) = (-3,1) + (8,0): 100 */
    CHECK_EQ_I(v92_inverse_map(y), 0x8);

    CHECK_EQ_I(v92_subset(0, 0), 0);
    CHECK_EQ_I(v92_subset(1, 1), 2);              /* (3,3) */
    CHECK_EQ_I(v92_subset(-2, -2), 0);            /* (-3,-3) */
    CHECK_EQ_I(v92_subset(-1, 1), 6);             /* (-1,3) */
    CHECK_EQ_I(v92_subset(-125, 126), v92_subset(3, 2));
}

/* The fifth bit of the code: within Table 13 the pairs that share Y4..Y1
 * split evenly by the parity of the index sum, and that parity is what
 * 6.4.2 ties to Y0. Checked here from the definitions only. */
static void test_parity_is_free(void)
{
    int cnt[16][2];
    int a, b, c, d;
    memset(cnt, 0, sizeof cnt);
    for (a = 0; a < 4; a++) for (b = 0; b < 4; b++)
    for (c = 0; c < 4; c++) for (d = 0; d < 4; d++) {
        int16_t y[4];
        y[0] = (int16_t)a; y[1] = (int16_t)b; y[2] = (int16_t)c; y[3] = (int16_t)d;
        cnt[v92_inverse_map(y)][(a + b + c + d) & 1]++;
    }
    for (a = 0; a < 16; a++) {
        CHECK_EQ_I(cnt[a][0], 8);
        CHECK_EQ_I(cnt[a][1], 8);
    }
}

/* ---- scrambler, modulus encoder, classes, trellis: one worked data frame -- */

/* N = 8 points +-100, 300, 500, 700 everywhere; M = 4 4 4 2 three times,
 * product 2^21, K = 21. No filters, so x(n) = u(n) and the minimum |x| rule
 * picks the member nearest zero:
 *   k = 0,1,2: Ki 0 -> {-4, 0} -> 0 (100);  1 -> {-3, 1} -> 1 (300);
 *              2 -> {-2, 2} -> -2 (-300);   3 -> {-1, 3} -> -1 (-100)
 *   k = 3: the same with 2 Ki + parity in place of Ki (2 M = 4). */
static const uint8_t M21[12] = { 4, 4, 4, 2, 4, 4, 4, 2, 4, 4, 4, 2 };

static void test_worked_frame(void)
{
    /* Data: all ones (B1u). GPA from zero: s(n) = 1 + s(n-5) + s(n-23):
     *   n = 0..22: 11111 00000 11111 00000 111
     *   n = 23..41: 0 0 1 1 1 1 1 0 0 0 1 1 0 0 0 0 0 1 1
     * Frame 0: b0..b20 = 11111 00000 11111 00000 1. M = 2^21, so s(f) = b20
     *   = 1; d(-1) = 0: R0 = R. The mixed radix digits are bit groups:
     *   K = 3 3 1 0  0 2 3 1  1 0 0 1.
     * Trellis frame 0 (Y0 = 0): eta = -1, -1, 1; parity (-1-1+1+0) odd,
     *   K3 = 0 -> 2*0 + 1 = 1 -> eta = 1. Points (-1,-1) 010 and (3,3) 010:
     *   Y4..Y1 = 0100. 16 states (Figure 10) and 32 states (Figure 11) do
     *   not use Y3: state stays 0, Y0 = 0.
     * Trellis frame 1 (Y0 = 0): K = 0 2 3 1: eta = 0, -2, -1; parity
     *   (0-2-1+0) odd, K7 = 1 -> 3 -> eta = -1. Points (1,-3) 100 and
     *   (-1,-1) 010: 1001, Y4 = 1, Y1 = 1.
     *   Figure 10 from 0000: d4' = d3 + Y1 = 1, so Y0 = 1.
     *   Figure 11 from 00000: d3' = d2 + Y1 = 1, d4' = d3 + Y4 = 1, Y0 = d5 = 0.
     * Trellis frame 2: K = 1 0 0 1: eta = 1, 0, 0.
     *   16 states (Y0 = 1): parity (1+0+0+1) even, K11 = 1 -> 2 -> eta = -2.
     *   32 states (Y0 = 0): parity odd, 3 -> eta = -1.
     * Frame 1: bits s(21..41) = 1 1 0 0 1 1 1 1 1 0 0 0 1 1 0 0 0 0 0 1 1,
     *   d(0) = 1 so R0 is the complement 0 0 1 1 0 0 0 0 0 1 1 1 0 0 1 1 1 1
     *   1 0 0: K = 0 3 0 0  0 3 1 0  3 3 1 0. */
    static const uint8_t k0[12] = { 3, 3, 1, 0, 0, 2, 3, 1, 1, 0, 0, 1 };
    static const uint8_t k1[12] = { 0, 3, 0, 0, 0, 3, 1, 0, 3, 3, 1, 0 };
    static const int16_t eta16[12] = { -1, -1, 1, 1, 0, -2, -1, -1, 1, 0, 0, -2 };
    static const int16_t eta32[12] = { -1, -1, 1, 1, 0, -2, -1, -1, 1, 0, 0, -1 };
    static const uint8_t y0_16[3] = { 0, 0, 1 };
    v92_map_t m;
    v92_enc_t e;
    v92_sym_t s;
    int tr, i;

    for (tr = 0; tr < 2; tr++) {
        const int16_t *want = tr ? eta32 : eta16;
        tu_map_uniform(&m, 21, tr, M21, 4, 100);
        CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_ones, NULL), 0);
        for (i = 0; i < 12; i++) {
            int32_t out = v92_enc_symbol(&e, &s);
            CHECK_EQ_I(s.i, i);
            CHECK_EQ_I(s.ki, k0[i]);
            CHECK_EQ_I(s.eta, want[i]);
            CHECK_EQ_I(s.u, (2 * want[i] + 1) * 100);
            CHECK_EQ_I(s.x, s.u * 256);
            CHECK_EQ_I(out, s.u);                 /* no filters: u(n) itself */
            CHECK_EQ_I(s.y0, tr ? 0 : y0_16[i / 4]);
        }
        /* state after the frame, by hand:
         * 16: 0000 -> (Y2 = 0, Y1 = 1) 0001 -> frame 2 points (3,1) 111 and
         *     (1,-3) 100: 0110, Y2 = 1, Y1 = 0: d1' = d4 = 1, d2' = d1 + Y2 +
         *     d4 = 0, d3' = d2 + Y2 = 1, d4' = d3 + Y1 = 0: 1010, bits 0 and 2.
         * 32: 00110 after frame 1 -> frame 2 points (3,1) 111 and (1,-1) 001:
         *     1111: d1' = d5 = 0, d2' = d1 + Y2 = 1, d3' = d2 + Y1 = 1,
         *     d4' = d3 + Y4 = 0, d5' = d4 + Y2 = 0: 01100, bits 1 and 2. */
        CHECK_EQ_I(e.conv, tr ? 0x06 : 0x05);
        for (i = 0; i < 12; i++) {
            (void)v92_enc_symbol(&e, &s);
            CHECK_EQ_I(s.i, i);
            CHECK_EQ_I(s.ki, k1[i]);
        }
    }

    /* all-zero data from the zero state: scrambler output 0, R = 0, every
     * Ki = 0, every index 0 (parity even, Y0 stays 0) */
    tu_map_uniform(&m, 21, 0, M21, 4, 100);
    CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_zeros, NULL), 0);
    for (i = 0; i < 36; i++) {
        CHECK_EQ_I(v92_enc_symbol(&e, &s), 100);
        CHECK_EQ_I(s.eta, 0);
        CHECK_EQ_I(s.ki, 0);
    }
}

/* ---- precoder filter and prefilter recursions ---------------------------- */

static void test_filters(void)
{
    /* N = 8, M = 8 8 8 4 ...: every class has exactly one member, so u(n)
     * follows from Ki alone: eta = Ki for Ki < 4, Ki - 8 otherwise; for
     * k = 3, eta = 2 Ki + parity, minus 8 if that is 4 or more.
     *   K = 4 1 7 3 0: eta = -4, 1, -1, then parity (-4+1-1+0) even and
     *   2*3 = 6 -> -2, then 0: u = -700, 300, -100, -300, 100.
     * z1(1) = 0.5; p1(1) = -0.25, p1(2) = 0.125:
     *   x0 = -700
     *   x1 = 300 - 350 + 175 = 125
     *   x2 = -100 + 150 - 31.25 - 87.5 = -68.75
     *   x3 = -300 - 50 + 17.1875 + 15.625 = -317.1875
     *   x4 = 100 - 150 + 79.296875 - 8.59375 = 20.703125
     * z2(0) = 0.5, z2(1) = 0.25; p2(1) = 0.5:
     *   v0 = -350
     *   v1 = 62.5 - 175 - 175 = -287.5
     *   v2 = -34.375 + 31.25 - 143.75 = -146.875
     *   v3 = -158.59375 - 17.1875 - 73.4375 = -249.21875
     *   v4 = 10.3515625 - 79.296875 - 124.609375 = -193.5546875
     * 4 G = 0.5: G = 0.125; output G v in Q14 = v * 2048. */
    static const uint8_t M[12] = { 8, 8, 8, 4, 8, 8, 8, 4, 8, 8, 8, 4 };
    static const uint8_t k[12] = { 4, 1, 7, 3, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const int32_t u[5] = { -700, 300, -100, -300, 100 };
    static const double x[5] = { -700, 125, -68.75, -317.1875, 20.703125 };
    static const double v[5] = { -350, -287.5, -146.875, -249.21875, -193.5546875 };
    v92_map_t m;
    v92_filt_t f;
    v92_enc_t e;
    v92_sym_t s;
    int n;

    tu_map_uniform(&m, 33, 0, M, 4, 100);
    memset(&f, 0, sizeof f);
    f.lz1 = 1; f.lp1 = 2; f.lz2 = 2; f.lp2 = 1;
    f.coef[0] = 16384;              /* z1(1) = 0.5, Q0.15 */
    f.coef[1] = -4096;              /* p1(1) = -0.25, Q1.14 */
    f.coef[2] = 2048;               /* p1(2) = 0.125 */
    f.coef[3] = 16384;              /* z2(0) = 0.5, Q0.15 */
    f.coef[4] = 8192;               /* z2(1) = 0.25 */
    f.coef[5] = 8192;               /* p2(1) = 0.5, Q1.14 */
    f.gain4 = 0x8000;               /* 4 G = 0.5, Q0.16 */
    CHECK_EQ_I(v92_enc_init(&e, &m, &f, get_zeros, NULL), 0);
    v92_enc_force_k(&e, k);
    for (n = 0; n < 5; n++) {
        int32_t out = v92_enc_symbol(&e, &s);
        CHECK_EQ_I(s.u, u[n]);
        CHECK_EQ_I(s.x, (long)(x[n] * 256));
        CHECK_EQ_I(s.v, (long)(v[n] * 256));
        CHECK_EQ_I(out, (long)(v[n] * 2048));
    }
}

/* The minimum |x(n)| rule with feedback: the precoder history moves the
 * choice inside the class. */
static void test_min_power(void)
{
    /* Same constellation, M = 4 4 4 2, p1(1) = 1.5, nothing else (z2(0) is
     * needed for an output but does not matter here).
     *   n = 0: K = 1: {-3, 1} = {-500, 300}; no history: 300.     x = 300
     *   n = 1: K = 0: {-4, 0} = {-700, 100}; feedback 450:
     *          -250 or 550: -700 (alone it would be 100).         x = -250
     *   n = 2: K = 2: {-2, 2} = {-300, 500}; feedback -375:
     *          -675 or 125: 500 (alone it would be -300).         x = 125
     *   n = 3: Y0 = 0, indices 1 - 4 + 2 odd; K = 0: 2*0 + 1 = 1:
     *          {-3, 1} = {-500, 300}; feedback 187.5:
     *          -312.5 or 487.5: -500.                             x = -312.5 */
    static const uint8_t k[12] = { 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    static const int16_t eta[4] = { 1, -4, 2, -3 };
    static const int32_t xq8[4] = { 76800, -64000, 32000, -80000 };
    v92_map_t m;
    v92_filt_t f;
    v92_enc_t e;
    v92_sym_t s;
    int n;

    tu_map_uniform(&m, 21, 0, M21, 4, 100);
    memset(&f, 0, sizeof f);
    f.lp1 = 1; f.lz2 = 1;
    f.coef[0] = 24576;              /* p1(1) = 1.5, Q1.14 */
    f.coef[1] = 16384;              /* z2(0) = 0.5 */
    f.gain4 = 0xFFFF;
    CHECK_EQ_I(v92_enc_init(&e, &m, &f, get_zeros, NULL), 0);
    v92_enc_force_k(&e, k);
    for (n = 0; n < 4; n++) {
        (void)v92_enc_symbol(&e, &s);
        CHECK_EQ_I(s.eta, eta[n]);
        CHECK_EQ_I(s.x, xq8[n]);
    }

    /* A tie goes to the higher index, as a real analogue modem was seen to
     * do it (reading 1): M = 3 (odd), N = 6, K = 1 at k = 0:
     * {-2, 1} = {-300, 300}. */
    {
        static const uint8_t M3[12] = { 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3 };
        static const uint8_t k3[12] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
        tu_map_uniform(&m, 19, 0, M3, 3, 100);    /* 3^12 = 531441 >= 2^19 */
        CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_zeros, NULL), 0);
        v92_enc_force_k(&e, k3);
        (void)v92_enc_symbol(&e, &s);
        CHECK_EQ_I(s.eta, 1);
    }
}

/* ---- framing, classes and trellis over long random runs ------------------ */

typedef struct { uint32_t seed; int calls; int bad; } pick_t;

static int pick_rand(void *user, int i, const int16_t *eta, int n)
{
    pick_t *p = (pick_t *)user;
    int j;
    (void)i;
    p->calls++;
    for (j = 1; j < n; j++) if (eta[j] <= eta[j - 1]) p->bad++;
    return (int)(tu_lcg(&p->seed) % (uint32_t)n);
}

static void test_invariants(void)
{
    /* two constellation sets: set 0 (N = 16) in intervals 0, 2, 3, 5 of the
     * constellation frame, set 1 (N = 12) in 1 and 4; moduli differ per
     * data frame interval (the k = 3 intervals 3, 7, 11 need 2 M <= N). */
    static const uint8_t M[12] = { 7, 5, 8, 4, 6, 3, 7, 6, 5, 8, 6, 8 };
    /* log2 of the product is about 30.3 */
    v92_map_t m;
    v92_enc_t e;
    v92_sym_t s;
    pick_t pk;
    uint32_t seed;
    int tr, mode, n, i, e0;
    int16_t y[4];

    for (tr = 0; tr < 3; tr++) for (mode = 0; mode < 2; mode++) {
        unsigned state = 0;
        int bad_i = 0, bad_range = 0, bad_class = 0, bad_par = 0, bad_u = 0;
        int seen_neg = 0, seen_pos = 0;
        tu_map_uniform(&m, 30, tr, M, 8, 50);
        m.cidx[1] = m.cidx[4] = 1;
        m.lc[1] = 6;
        for (e0 = 0; e0 < 6; e0++) m.pt[1][e0] = (uint16_t)(40 + 90 * e0 + e0 * e0);
        CHECK_EQ_I(v92_map_check(&m), 0);
        seed = 1234 + (uint32_t)tr;
        CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_rand, &seed), 0);
        pk.seed = 5; pk.calls = 0; pk.bad = 0;
        if (mode) v92_enc_set_pick(&e, pick_rand, &pk);
        for (n = 0; n < 12000; n++) {
            int lc;
            (void)v92_enc_symbol(&e, &s);
            i = n % 12;
            lc = (i % 6 == 1 || i % 6 == 4) ? 6 : 8;
            if (s.i != i) bad_i++;
            if (s.eta < -lc || s.eta >= lc) bad_range++;
            if (v92_eta_to_k(&m, i, s.eta) != s.ki) bad_class++;
            if (s.u != v92_point(&m, i, s.eta)) bad_u++;
            if (s.y0 != v34_conv_y0(tr, state)) bad_par++;
            if (s.eta < 0) seen_neg++; else seen_pos++;
            y[i % 4] = s.eta;
            if (i % 4 == 3) {
                /* eta0 + eta1 + eta2 + eta3 = Y0 mod 2 */
                if (((y[0] + y[1] + y[2] + y[3] + 1024) & 1) != (int)v34_conv_y0(tr, state))
                    bad_par++;
                state = v34_conv_next(tr, state, v92_inverse_map(y));
            }
        }
        CHECK_EQ_I(bad_i, 0);
        CHECK_EQ_I(bad_range, 0);
        CHECK_EQ_I(bad_class, 0);
        CHECK_EQ_I(bad_u, 0);
        CHECK_EQ_I(bad_par, 0);
        CHECK(seen_neg > 3000 && seen_pos > 3000);
        if (mode) { CHECK_EQ_I(pk.calls, 12000); CHECK_EQ_I(pk.bad, 0); }
    }
}

/* ---- RM / RM' (Tables 25, 26) -------------------------------------------- */

static void test_rm(void)
{
    static const uint8_t M[12] = { 7, 5, 8, 4, 6, 3, 7, 6, 5, 8, 6, 8 };
    /* Table 25: K0 = M0-1, K1 = M1-1, K2 = K3 = 0, K4 = M4-1, K5 = M5-1,
     * K6 = K7 = 0, K8 = M8-1, K9 = M9-1, K10 = 0, interval 11: 0.
     * Table 26 is the other half: Ki = Mi-1 where RM has 0 and 0 where RM
     * has Mi-1. */
    static const uint8_t rm[12]  = { 6, 4, 0, 0, 5, 2, 0, 0, 4, 7, 0, 0 };
    static const uint8_t rmp[12] = { 0, 0, 7, 3, 0, 0, 6, 5, 0, 0, 5, 7 };
    uint8_t k[12];
    v92_map_t m;
    v92_enc_t e;
    v92_sym_t s;
    int i;

    v92_rm_k(M, 0, k);
    for (i = 0; i < 12; i++) CHECK_EQ_I(k[i], rm[i]);
    v92_rm_k(M, 1, k);
    for (i = 0; i < 12; i++) CHECK_EQ_I(k[i], rmp[i]);

    /* sent through the encoder ("trellis encoded") the Ki are the forced
     * ones and get() is left alone */
    tu_map_uniform(&m, 30, 0, M, 8, 50);
    CHECK_EQ_I(v92_enc_init(&e, &m, NULL, NULL, NULL), 0);
    v92_enc_force_k(&e, rm);
    for (i = 0; i < 12; i++) {
        (void)v92_enc_symbol(&e, &s);
        CHECK_EQ_I(s.ki, rm[i]);
        CHECK_EQ_I(v92_eta_to_k(&m, i, s.eta), rm[i]);
    }
}

static void test_init(void)
{
    v92_map_t m;
    v92_filt_t f;
    v92_enc_t e;
    tu_map_uniform(&m, 21, 0, M21, 4, 100);
    memset(&f, 0, sizeof f);
    f.lz2 = 1; f.coef[0] = 100; f.gain4 = 1;
    CHECK_EQ_I(v92_enc_init(&e, &m, &f, get_ones, NULL), 0);
    f.gain4 = 0;
    CHECK_EQ_I(v92_enc_init(&e, &m, &f, get_ones, NULL), V92_ERR_RANGE);
    m.K = 22;
    CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_ones, NULL), V92_ERR_RANGE);
}

int main(void)
{
    test_inverse_map();
    test_parity_is_free();
    test_worked_frame();
    test_filters();
    test_min_power();
    test_invariants();
    test_rm();
    test_init();
    T_REPORT("test_enc");
}
