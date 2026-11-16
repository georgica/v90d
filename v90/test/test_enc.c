/* V.90 5.4 encoder building blocks.
 *
 * Hand-computed vectors (worked in the comments from the text of 5.4.2 to
 * 5.4.5.6 and Tables 3, 4, 5, Figure 2), then properties: the shaper never
 * leaves the trellis, it picks a metric-minimal rule sequence (checked against
 * a brute-force search in floating point written here), it does lower the
 * metric it is asked to lower, and every frame decodes with the reference
 * decoder. */
#include <string.h>
#include "test.h"
#include "v90_enc.h"
#include "v90_pcm.h"
#include "v90_dec.h"

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }
static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }

/* a constellation of the n Ucodes given, into interval i */
static void points(v90_map_t *m, int i, const int *uc, int n)
{
    uint8_t mask[16];
    int k;
    memset(mask, 0, sizeof mask);
    for (k = 0; k < n; k++) setbit(mask, uc[k]);
    v90_map_set_points(m, i, mask);
}

/* M distinct points spread over Ucodes 0..top, the largest being `top`
 * (needs top + 1 >= M) */
static void spread(v90_map_t *m, int i, int M, int top)
{
    uint8_t mask[16];
    int k;
    memset(mask, 0, sizeof mask);
    for (k = 0; k < M; k++) setbit(mask, top - (k * (top + 1)) / M);
    v90_map_set_points(m, i, mask);
}

/* ---- mapping parameters --------------------------------------------------- */

static void test_map(void)
{
    static v90_map_t m;
    static const int uc[] = { 10, 30, 20 };
    uint8_t mask[16];
    int i;

    memset(&m, 0, sizeof m);
    /* 5.4.4: label 0 is the largest PCM code, label M-1 the smallest */
    points(&m, 2, uc, 3);
    CHECK_EQ_I(m.M[2], 3);
    CHECK_EQ_I(m.C[2][0], 30); CHECK_EQ_I(m.C[2][1], 20); CHECK_EQ_I(m.C[2][2], 10);
    memset(mask, 0xFF, sizeof mask);
    v90_map_set_points(&m, 5, mask);
    CHECK_EQ_I(m.M[5], 128);
    CHECK_EQ_I(m.C[5][0], 127); CHECK_EQ_I(m.C[5][127], 0);

    /* 2^K <= product of Mi */
    for (i = 0; i < 6; i++) spread(&m, i, 4, 100);       /* product 4096 = 2^12 */
    m.K = 12; m.Sr = 0; m.ld = 0;
    CHECK_EQ_I(v90_map_check(&m), 0);
    m.K = 13; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);
    m.K = 12; m.Sr = 4; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);
    m.Sr = 3; m.ld = 4; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);
    m.ld = 3; CHECK_EQ_I(v90_map_check(&m), 0);
    m.M[3] = 0; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);
    m.M[3] = 4;
    m.C[1][2] = m.C[1][1]; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);  /* not descending */
    spread(&m, 1, 4, 100);
    for (i = 0; i < 6; i++) v90_map_set_points(&m, i, mask);             /* 128^6 = 2^42 */
    m.K = 42; CHECK_EQ_I(v90_map_check(&m), 0);
    m.K = 43; CHECK_EQ_I(v90_map_check(&m), V90_ERR_RANGE);
    CHECK_EQ_I(V90_MAP_D(&m), 43 + 3);
    CHECK_EQ_I(V90_MAP_S(&m), 3);
}

/* ---- modulus encoder ------------------------------------------------------ */

static void bits_of(uint64_t v, int n, uint8_t *b) { int i; for (i = 0; i < n; i++) b[i] = (uint8_t)((v >> i) & 1); }

static void test_modulus(void)
{
    static v90_map_t m;
    uint8_t b[48], k[6];
    int i;

    /* K = 16, R0 = 0xBEEF = 48879, M = {3,5,7,11,13,17} (product 255255):
     *   48879 = 3*16293 + 0      K0 = 0
     *   16293 = 5*3258  + 3      K1 = 3
     *    3258 = 7*465   + 3      K2 = 3
     *     465 = 11*42   + 3      K3 = 3
     *      42 = 13*3    + 3      K4 = 3
     *       3 = 17*0    + 3      K5 = 3 */
    memset(&m, 0, sizeof m);
    m.K = 16;
    { static const int M[6] = { 3, 5, 7, 11, 13, 17 }; for (i = 0; i < 6; i++) spread(&m, i, M[i], 120); }
    CHECK_EQ_I(v90_map_check(&m), 0);
    bits_of(0xBEEF, 16, b);
    v90_modulus_encode(&m, b, k);
    CHECK_EQ_I(k[0], 0); CHECK_EQ_I(k[1], 3); CHECK_EQ_I(k[2], 3);
    CHECK_EQ_I(k[3], 3); CHECK_EQ_I(k[4], 3); CHECK_EQ_I(k[5], 3);

    /* K = 6, R0 = 63, M = {3,3,3,3,2,2}:
     *   63 = 3*21 + 0, 21 = 3*7 + 0, 7 = 3*2 + 1, 2 = 3*0 + 2, 0, 0 */
    memset(&m, 0, sizeof m);
    m.K = 6;
    { static const int M[6] = { 3, 3, 3, 3, 2, 2 }; for (i = 0; i < 6; i++) spread(&m, i, M[i], 120); }
    bits_of(63, 6, b);
    v90_modulus_encode(&m, b, k);
    CHECK_EQ_I(k[0], 0); CHECK_EQ_I(k[1], 0); CHECK_EQ_I(k[2], 1);
    CHECK_EQ_I(k[3], 2); CHECK_EQ_I(k[4], 0); CHECK_EQ_I(k[5], 0);
    /* b0 is the least significant bit: only b0 set -> R0 = 1 -> K0 = 1 */
    memset(b, 0, sizeof b); b[0] = 1;
    v90_modulus_encode(&m, b, k);
    CHECK_EQ_I(k[0], 1); CHECK_EQ_I(k[1] | k[2] | k[3] | k[4] | k[5], 0);
    /* only b5 set -> R0 = 32 = 3*10 + 2, 10 = 3*3 + 1, 3 = 3*1 + 0, 1 = 3*0 + 1 */
    memset(b, 0, sizeof b); b[5] = 1;
    v90_modulus_encode(&m, b, k);
    CHECK_EQ_I(k[0], 2); CHECK_EQ_I(k[1], 1); CHECK_EQ_I(k[2], 0);
    CHECK_EQ_I(k[3], 1); CHECK_EQ_I(k[4], 0); CHECK_EQ_I(k[5], 0);

    /* The largest case of Table 2: K = 39, all Mi = 128, R0 = 2^39 - 1.
     * Dividing by 128 peels 7 bits at a time: K0..K4 = 127 (35 bits), K5 = the
     * remaining 4 bits = 15. */
    memset(&m, 0, sizeof m);
    m.K = 39;
    for (i = 0; i < 6; i++) spread(&m, i, 128, 127);
    CHECK_EQ_I(v90_map_check(&m), 0);
    memset(b, 1, sizeof b);
    v90_modulus_encode(&m, b, k);
    for (i = 0; i < 5; i++) CHECK_EQ_I(k[i], 127);
    CHECK_EQ_I(k[5], 15);
    /* R0 = 2^38: 38 = 5*7 + 3, so K0..K4 = 0 and K5 = 8 */
    memset(b, 0, sizeof b); b[38] = 1;
    v90_modulus_encode(&m, b, k);
    for (i = 0; i < 5; i++) CHECK_EQ_I(k[i], 0);
    CHECK_EQ_I(k[5], 8);

    /* random moduli against 64-bit arithmetic */
    {
        uint32_t seed = 7;
        int trial, bad = 0;
        for (trial = 0; trial < 3000; trial++) {
            uint64_t prod = 1, r;
            int K = 0;
            memset(&m, 0, sizeof m);
            for (i = 0; i < 6; i++) { int M = 1 + (int)(lcg(&seed) % 128); spread(&m, i, M, 127); prod *= (uint64_t)M; }
            while (K < 42 && ((uint64_t)1 << (K + 1)) <= prod) K++;
            m.K = (uint8_t)K;
            r = (((uint64_t)lcg(&seed) << 24) ^ lcg(&seed)) & (((uint64_t)1 << K) - 1);
            bits_of(r, K, b);
            v90_modulus_encode(&m, b, k);
            for (i = 0; i < 6; i++) {
                if (k[i] != (uint8_t)(r % m.M[i])) bad++;
                r /= m.M[i];
            }
            if (r != 0) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }
}

/* ---- sign precoding (Tables 3 and 4) -------------------------------------- */

static void precode_check(v90_diff_t *d, int Sr, const char *s, const char *want)
{
    uint8_t sb[6], t[6];
    int i, S = 6 - Sr;
    for (i = 0; i < S; i++) sb[i] = (uint8_t)(s[i] - '0');
    v90_sign_precode(d, Sr, sb, t);
    for (i = 0; i < 6; i++) CHECK_EQ_I(t[i], want[i] - '0');
}

static void test_precode(void)
{
    v90_diff_t d;

    /* Sr = 0 (5.4.5.1): $0 = s0 ^ ($5 previous), $i = si ^ $(i-1)
     *   s = 101100: $ = 1, 1^0=1, 1^1=0, 0^1=1, 1^0=1, 1^0=1   -> 110111
     *   s = 111111: $ = 1^1=0, 1, 0, 1, 0, 1                   -> 010101 */
    v90_diff_init(&d);
    precode_check(&d, 0, "101100", "110111");
    precode_check(&d, 0, "111111", "010101");

    /* Sr = 1: one shaping frame, p = (0, s0, s1, s2, s3, s4)
     *   p'(1) = p(1)^p'prev(5), p'(3) = p(3)^p'(1), p'(5) = p(5)^p'(3),
     *   even bits unchanged; t(k) = p'(k) ^ tprev(k)
     * frame 1: s = 11011  p = 011011  p' = 0,1^0=1,1,0^1=1,1,1^1=0 = 011110
     *          t = 011110
     * frame 2: s = 10110  p = 010110  p' = 0,1^0=1,0,1^1=0,1,0^0=0 = 010010
     *          t = 010010 ^ 011110 = 001100
     * frame 3: s = 00001  p = 000001  p' = 0,0^0=0,0,0,0,1^0=1     = 000001
     *          t = 000001 ^ 001100 = 001101
     * frame 4: s = 10000  p = 010000  p' = 0,1^1=0,0,0,0,0         = 000000
     *          t = 001101 */
    v90_diff_init(&d);
    precode_check(&d, 1, "11011", "011110");
    precode_check(&d, 1, "10110", "001100");
    precode_check(&d, 1, "00001", "001101");
    precode_check(&d, 1, "10000", "001101");

    /* Sr = 2: two shaping frames of 3: (0, s0, s1) and (0, s2, s3)
     *   p'(1) = p(1) ^ p'(1) of the previous shaping frame, p'(2) = p(2)
     * data frame 1: s = 1011
     *   j:   p = 010, p' = 0,1^0=1,0 = 010, t = 010
     *   j+1: p = 011, p' = 0,1^1=0,1 = 001, t = 001 ^ 010 = 011
     * data frame 2: s = 1101
     *   j+2: p = 011, p' = 0,1^0=1,1 = 011, t = 011 ^ 011 = 000
     *   j+3: p = 001, p' = 0,0^1=1,1 = 011, t = 011 ^ 000 = 011 */
    v90_diff_init(&d);
    precode_check(&d, 2, "1011", "010011");
    precode_check(&d, 2, "1101", "000011");

    /* Sr = 3: three shaping frames of 2: (0, s0), (0, s1), (0, s2)
     * data frame 1: s = 110
     *   p'(1) = 1^0 = 1, t = 01
     *   p'(1) = 1^1 = 0, t = 00 ^ 01 = 01
     *   p'(1) = 0^0 = 0, t = 00 ^ 01 = 01
     * data frame 2: s = 101
     *   p'(1) = 1^0 = 1, t = 01 ^ 01 = 00
     *   p'(1) = 0^1 = 1, t = 01 ^ 00 = 01
     *   p'(1) = 1^1 = 0, t = 00 ^ 01 = 01 */
    v90_diff_init(&d);
    precode_check(&d, 3, "110", "010101");
    precode_check(&d, 3, "101", "000101");
}

/* ---- one whole frame by hand ---------------------------------------------- */

static void test_frame(void)
{
    static v90_map_t m;
    static const int big[] = { 20, 50, 80 }, small[] = { 33, 66 };
    v90_diff_t d;
    uint8_t bits[12], u[6], t[6];
    int i;

    /* K = 6, Sr = 0, D = 12, twelve ones (what the first frame of TRN2d
     * carries: the first 18 scrambled ones are ones).
     *   5.4.2: s0..s5 = d0..d5 = 111111, b0..b5 = d6..d11 = 111111
     *   5.4.3: R0 = 63, M = {3,3,3,3,2,2} -> K = 0,0,1,2,0,0 (see above)
     *   5.4.4: C0..C3 = {80,50,20}, C4..C5 = {66,33}, label 0 the largest
     *          U = 80, 80, 50, 20, 66, 66
     *   5.4.5.1: $ = 1, 0, 1, 0, 1, 0 */
    memset(&m, 0, sizeof m);
    m.K = 6; m.Sr = 0;
    for (i = 0; i < 4; i++) points(&m, i, big, 3);
    for (i = 4; i < 6; i++) points(&m, i, small, 2);
    CHECK_EQ_I(v90_map_check(&m), 0);
    CHECK_EQ_I(V90_MAP_D(&m), 12);
    memset(bits, 1, sizeof bits);
    v90_diff_init(&d);
    v90_frame_encode(&m, &d, bits, u, t);
    CHECK_EQ_I(u[0], 80); CHECK_EQ_I(u[1], 80); CHECK_EQ_I(u[2], 50);
    CHECK_EQ_I(u[3], 20); CHECK_EQ_I(u[4], 66); CHECK_EQ_I(u[5], 66);
    CHECK_EQ_I(t[0], 1); CHECK_EQ_I(t[1], 0); CHECK_EQ_I(t[2], 1);
    CHECK_EQ_I(t[3], 0); CHECK_EQ_I(t[4], 1); CHECK_EQ_I(t[5], 0);

    /* Same constellations, Sr = 3 so S = 3, K = 6, D = 9. Bits 1,0,1 then
     * b = 0,1,0,1,0,0 (R0 = 2 + 8 = 10):
     *   10 = 3*3 + 1, 3 = 3*1 + 0, 1 = 3*0 + 1 -> K = 1,0,1,0,0,0
     *   U = 50, 80, 50, 80, 66, 66
     *   s = 101 from zero state: p'(1) = 1, 0^1 = 1, 1^1 = 0
     *   t = 01, 01^01 = 00, 00^00 = 00 -> 010000 */
    m.Sr = 3;
    {
        static const uint8_t in[9] = { 1, 0, 1,  0, 1, 0, 1, 0, 0 };
        v90_diff_init(&d);
        v90_frame_encode(&m, &d, in, u, t);
        CHECK_EQ_I(u[0], 50); CHECK_EQ_I(u[1], 80); CHECK_EQ_I(u[2], 50);
        CHECK_EQ_I(u[3], 80); CHECK_EQ_I(u[4], 66); CHECK_EQ_I(u[5], 66);
        CHECK_EQ_I(t[0], 0); CHECK_EQ_I(t[1], 1); CHECK_EQ_I(t[2], 0);
        CHECK_EQ_I(t[3], 0); CHECK_EQ_I(t[4], 0); CHECK_EQ_I(t[5], 0);
    }
}

/* ---- metric filter -------------------------------------------------------- */

static void test_filter(void)
{
    static v90_map_t m;
    v90_shaper_t sh;

    /* a1 = 0.5, b1 = 0.25, a2 = -0.5, b2 = 0.75; x = 64, -128, 256, 0
     *   y[n] = x[n] - b1 x[n-1] + a1 y[n-1]
     *   v[n] = y[n] - b2 y[n-1] + a2 v[n-1]
     *   n=0: y = 64                              v = 64
     *   n=1: y = -128 - 16 + 32 = -112           v = -112 - 48 - 32 = -192
     *   n=2: y = 256 + 32 - 56 = 232             v = 232 + 84 + 96 = 412
     *   n=3: y = 0 - 64 + 116 = 52               v = 52 - 174 - 206 = -328
     * returned in Q6 (times 64). */
    memset(&m, 0, sizeof m);
    m.a1 = 32; m.b1 = 16; m.a2 = -32; m.b2 = 48;
    v90_shaper_init(&sh);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, 64), 64 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, -128), -192 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, 256), 412 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, 0), -328 * 64);

    /* a1 = 1, rest 0: F(z) = 1/(1 - z^-1), v is the running sum */
    memset(&m, 0, sizeof m);
    m.a1 = 64;
    v90_shaper_init(&sh);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, 100), 100 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, -30), 70 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, -80), -10 * 64);

    /* all zero: F(z) = 1 */
    memset(&m, 0, sizeof m);
    v90_shaper_init(&sh);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, -32124), -32124 * 64);
    CHECK_EQ_I(v90_shaper_step(&sh, &m, 5), 5 * 64);

    /* a1 = a2 = 1 with a constant full-scale input diverges; the state must
     * saturate rather than wrap */
    m.a1 = 64; m.a2 = 64;
    v90_shaper_init(&sh);
    {
        int i; int32_t v = 0, prev = 0, mono = 1;
        for (i = 0; i < 5000; i++) { v = v90_shaper_step(&sh, &m, 32124); if (v < prev) mono = 0; prev = v; }
        CHECK(mono);
        CHECK(v > 0);
    }
}

/* ---- shaper decisions by hand --------------------------------------------- */

static void shape_check(v90_shaper_t *sh, const v90_map_t *m, const int32_t *lin,
                        const char *t, int nframes, int want_rule, const char *want_sign)
{
    uint8_t tb[24], sign[6];
    int i, F = 6 / m->Sr, rule;
    for (i = 0; t[i]; i++) tb[i] = (uint8_t)(t[i] - '0');
    memset(sign, 9, sizeof sign);
    rule = v90_shape(sh, m, lin, tb, nframes, sign);
    CHECK_EQ_I(rule, want_rule);
    for (i = 0; i < F; i++) CHECK_EQ_I(sign[i], want_sign[i] - '0');
}

static void test_shape_hand(void)
{
    static v90_map_t m;
    v90_shaper_t sh;
    enum { A, B, C, D };

    /* Sr = 1 (one 6-symbol shaping frame), ld = 0, a1 = 1 so the metric is the
     * sum of squared running sums (RDS). Start: state 0, RDS 0.
     *
     * frame 1: |x| = 10,2,2,2,2,2  t = 111111, state 0 (A or B)
     *   A: RDS 10,12,14,16,18,20; B: the negatives. Tie -> A. RDS = 20.
     * frame 2: |x| = 3 x6, t = 101010, state 0
     *   A (+-+-+-): RDS 23,20,23,20,23,20  -> 3*529 + 3*400 = 2787
     *   B (-+-+-+): RDS 17,20,17,20,17,20  -> 3*289 + 3*400 = 2067   <- B
     *   signs 010101, next state 1, RDS = 20
     * frame 3: |x| = 5,1,1,1,1,1, t = 111111, state 1 (C or D)
     *   C inverts even: 010101 = -5,+1,-1,+1,-1,+1: RDS 15,16,15,16,15,16 -> 1443  <- C
     *   D inverts odd:  101010 = +5,-1,+1,-1,+1,-1: RDS 25,24,25,24,25,24 -> 3603
     *   next state 0, RDS = 16
     * frame 4: |x| = 8 x6, t = 111000, state 0
     *   A (+++---): RDS 24,32,40,32,24,16 -> 5056
     *   B (---+++): RDS 8,0,-8,0,8,16     -> 448                    <- B
     *   signs 000111, next state 1, RDS = 16
     * frame 5: |x| = 2,6,2,6,2,6, t = 000000, state 1
     *   C: 101010 = +2,-6,+2,-6,+2,-6: RDS 18,12,14,8,10,4    -> 844   <- C
     *   D: 010101 = -2,+6,-2,+6,-2,+6: RDS 14,20,18,24,22,28  -> 2764
     *   next state 0 */
    memset(&m, 0, sizeof m);
    m.Sr = 1; m.ld = 0; m.a1 = 64;
    v90_shaper_init(&sh);
    { static const int32_t l[6] = { 10, 2, 2, 2, 2, 2 }; shape_check(&sh, &m, l, "111111", 1, A, "111111"); }
    CHECK_EQ_I(sh.q, 0);
    { static const int32_t l[6] = { 3, 3, 3, 3, 3, 3 };  shape_check(&sh, &m, l, "101010", 1, B, "010101"); }
    CHECK_EQ_I(sh.q, 1);
    { static const int32_t l[6] = { 5, 1, 1, 1, 1, 1 };  shape_check(&sh, &m, l, "111111", 1, C, "010101"); }
    CHECK_EQ_I(sh.q, 0);
    { static const int32_t l[6] = { 8, 8, 8, 8, 8, 8 };  shape_check(&sh, &m, l, "111000", 1, B, "000111"); }
    CHECK_EQ_I(sh.q, 1);
    { static const int32_t l[6] = { 2, 6, 2, 6, 2, 6 };  shape_check(&sh, &m, l, "000000", 1, C, "101010"); }
    CHECK_EQ_I(sh.q, 0);
    CHECK_EQ_I(sh.y1, 4 * 64);                           /* RDS after frame 5 */

    /* Lookahead. Sr = 3 (2-symbol shaping frames), a1 = 1, state 0, RDS 0.
     *   f0: |x| = 1,1   t = 10      f1: |x| = 5,3   t = 11      f2: |x| = 2,6   t = 10
     * f0 alone:  A (+1,-1): RDS 1,0 -> 1;  B (-1,+1): RDS -1,0 -> 1.  Tie -> A.
     * f0 with f1 in view (ld = 1):
     *   after A (state 0, RDS 0): f1 A (+5,+3): 25+64 = 89; B: 89       -> 1 + 89 = 90
     *   after B (state 1, RDS 0): f1 C (-5,+3): 25+4 = 29;  D: 29       -> 1 + 29 = 30
     *   so B: signs 01, state 1, RDS 0.
     * f1 with f2 in view, state 1, RDS 0:
     *   C (-5,+3): cost 29, RDS -2, state 0:
     *       f2 A (+2,-6): RDS 0,-6 -> 36;  B (-2,+6): RDS -4,2 -> 20    -> 29 + 20 = 49
     *   D (+5,-3): cost 29, RDS +2, state 1:
     *       f2 C (-2,-6): RDS 0,-6 -> 36;  D (+2,+6): RDS 4,10 -> 116   -> 29 + 36 = 65
     *   so C: signs 01, state 0, RDS -2. */
    memset(&m, 0, sizeof m);
    m.Sr = 3; m.a1 = 64;
    {
        static const int32_t l[6] = { 1, 1, 5, 3, 2, 6 };
        m.ld = 0;
        v90_shaper_init(&sh);
        shape_check(&sh, &m, l, "101110", 3, A, "10");   /* extra frames ignored at ld = 0 */
        CHECK_EQ_I(sh.q, 0);
        m.ld = 1;
        v90_shaper_init(&sh);
        shape_check(&sh, &m, l, "1011", 1, A, "10");     /* ld = 1 but only one frame known */
        v90_shaper_init(&sh);
        shape_check(&sh, &m, l, "101110", 3, B, "01");
        CHECK_EQ_I(sh.q, 1);
        CHECK_EQ_I(sh.y1, 0);
        shape_check(&sh, &m, l + 2, "1110", 2, C, "01");
        CHECK_EQ_I(sh.q, 0);
        CHECK_EQ_I(sh.y1, -2 * 64);
    }

    /* an all-zero filter cannot tell the rules apart: signs stay as given */
    memset(&m, 0, sizeof m);
    m.Sr = 2; m.ld = 3;
    {
        static const int32_t l[12] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 2, 3, 4 };
        v90_shaper_init(&sh);
        shape_check(&sh, &m, l, "110100101101", 4, A, "110");
        shape_check(&sh, &m, l + 3, "100101101", 3, A, "100");
    }
}

/* ---- shaper properties ---------------------------------------------------- */

/* floating point model of 5.4.5.6 kept here */
typedef struct { double x1, y1, v1; } ffilt_t;
static double fstep(ffilt_t *f, const v90_map_t *m, double x)
{
    double y = x - (m->b1 / 64.0) * f->x1 + (m->a1 / 64.0) * f->y1;
    double v = y - (m->b2 / 64.0) * f->y1 + (m->a2 / 64.0) * f->v1;
    f->x1 = x; f->y1 = y; f->v1 = v;
    return v * v;
}

/* Figure 2 as a table: rules allowed in each state and where they lead */
static const int fig2_rule[2][2] = { { 0, 1 }, { 2, 3 } };      /* state 0: A, B; state 1: C, D */
static const int fig2_next[4] = { 0, 1, 0, 1 };                 /* A, B, C, D */
static int rule_inverts(int rule, int k)
{
    switch (rule) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (k & 1) == 0;
    default: return (k & 1) == 1;
    }
}

/* least metric over all allowed rule sequences for frames f.., starting from
 * state q, with frame f forced to `first` when first >= 0 */
static double brute(const v90_map_t *m, int F, const int32_t *lin, const uint8_t *t,
                    int f, int nf, ffilt_t st, int q, int first)
{
    double best = -1;
    int c, k;
    for (c = 0; c < 2; c++) {
        int rule = fig2_rule[q][c];
        ffilt_t s2 = st;
        double w = 0;
        if (first >= 0 && rule != first) continue;
        for (k = 0; k < F; k++) {
            int sg = t[f * F + k] ^ rule_inverts(rule, k);
            w += fstep(&s2, m, sg ? (double)lin[f * F + k] : -(double)lin[f * F + k]);
        }
        if (f + 1 < nf) w += brute(m, F, lin, t, f + 1, nf, s2, fig2_next[rule], -1);
        if (best < 0 || w < best) best = w;
    }
    return best;
}

static void test_shape_props(void)
{
    static v90_map_t m;
    /* coefficient sets: DC null, Nyquist null, both, and a general one */
    static const int8_t coef[5][4] = {      /* a1, a2, b1, b2 */
        { 64, 0, 0, 0 }, { -64, 0, 0, 0 }, { 64, -64, 0, 0 }, { 58, -40, 12, -20 }, { 60, 0, 32, 0 }
    };
    uint32_t seed = 4242;
    int Sr, ld, cs;

    for (cs = 0; cs < 5; cs++)
    for (Sr = 1; Sr <= 3; Sr++)
    for (ld = 0; ld <= 3; ld++) {
        enum { NF = 400 };
        int F = 6 / Sr, j, k, q = 0;
        int trellis_bad = 0, subopt = 0, argmin_same = 0, decided = 0;
        static int32_t lin[(NF + 4) * 6];
        static uint8_t t[(NF + 4) * 6];
        v90_shaper_t sh;
        ffilt_t ff;
        memset(&m, 0, sizeof m);
        m.Sr = (uint8_t)Sr; m.ld = (uint8_t)ld;
        m.a1 = coef[cs][0]; m.a2 = coef[cs][1]; m.b1 = coef[cs][2]; m.b2 = coef[cs][3];
        for (j = 0; j < (NF + 4) * F; j++) {
            lin[j] = v90_ucode_linear(0, lcg(&seed) % 128);
            t[j] = (uint8_t)(lcg(&seed) & 1);
        }
        v90_shaper_init(&sh);
        memset(&ff, 0, sizeof ff);
        for (j = 0; j < NF; j++) {
            uint8_t sign[6];
            int avail = 1 + (int)(lcg(&seed) % 5);       /* sometimes fewer than ld + 1 */
            int nf = avail < ld + 1 ? avail : ld + 1;
            int rule = v90_shape(&sh, &m, lin + j * F, t + j * F, avail, sign);
            int e = sign[0] ^ t[j * F], o = sign[1] ^ t[j * F + 1];
            double best, mine;
            /* Figure 2: the rule is one of the two allowed in state q, the
             * signs are that rule applied to t, and the state follows */
            if (rule != fig2_rule[q][0] && rule != fig2_rule[q][1]) trellis_bad++;
            for (k = 0; k < F; k++)
                if ((sign[k] ^ t[j * F + k]) != rule_inverts(rule, k)) trellis_bad++;
            if ((e ^ o) != q) trellis_bad++;
            if (sh.q != fig2_next[rule]) trellis_bad++;
            /* optimality over the same window */
            best = brute(&m, F, lin + j * F, t + j * F, 0, nf, ff, q, -1);
            mine = brute(&m, F, lin + j * F, t + j * F, 0, nf, ff, q, rule);
            if (mine > best * (1.0 + 1e-3) + 1e-6) subopt++;
            {
                double other = brute(&m, F, lin + j * F, t + j * F, 0, nf, ff, q, fig2_rule[q][rule == fig2_rule[q][0]]);
                if (fabs(other - mine) > 1e-3 * best) { decided++; if (mine < other) argmin_same++; }
            }
            q = fig2_next[rule];
            for (k = 0; k < F; k++)
                (void)fstep(&ff, &m, sign[k] ? (double)lin[j * F + k] : -(double)lin[j * F + k]);
        }
        CHECK_EQ_I(trellis_bad, 0);
        CHECK_EQ_I(subopt, 0);
        CHECK_EQ_I(argmin_same, decided);
        CHECK(decided > NF / 2);
    }
}

/* The point of it all: with a DC null requested the running sum of the line
 * signal stays small, and with a Nyquist null the alternating sum does. */
static void test_shape_effect(void)
{
    static v90_map_t m;
    int Sr, ld, nyq;
    for (nyq = 0; nyq < 2; nyq++)
    for (Sr = 1; Sr <= 3; Sr++) {
        double shaped[4];
        double plain = 0;
        for (ld = 0; ld <= 3; ld++) {
            enum { NF = 6000 };
            uint32_t seed = 1000 + (uint32_t)Sr;         /* same data for every ld */
            int F = 6 / Sr, j, k, n = 0;
            static int32_t lin[(NF + 4) * 6];
            static uint8_t t[(NF + 4) * 6];
            v90_shaper_t sh;
            double acc = 0, accp = 0, sum = 0, sump = 0;
            memset(&m, 0, sizeof m);
            m.Sr = (uint8_t)Sr; m.ld = (uint8_t)ld; m.a1 = nyq ? -64 : 64;
            for (j = 0; j < (NF + 4) * F; j++) {
                lin[j] = v90_ucode_linear(0, 40 + lcg(&seed) % 60);
                t[j] = (uint8_t)(lcg(&seed) & 1);
            }
            v90_shaper_init(&sh);
            for (j = 0; j < NF; j++) {
                uint8_t sign[6];
                (void)v90_shape(&sh, &m, lin + j * F, t + j * F, 4, sign);
                for (k = 0; k < F; k++, n++) {
                    double alt = (nyq && (n & 1)) ? -1.0 : 1.0;
                    sum  += alt * (sign[k] ? lin[j * F + k] : -lin[j * F + k]);
                    sump += alt * (t[j * F + k] ? lin[j * F + k] : -lin[j * F + k]);
                    acc += sum * sum; accp += sump * sump;
                }
            }
            shaped[ld] = acc / n;
            plain = accp / n;
        }
        /* far below the unshaped random walk, and deeper lookahead does not hurt */
        CHECK(shaped[0] < plain / 20);
        CHECK(shaped[1] < shaped[0]);
        CHECK(shaped[3] < shaped[1] * 1.02);
        printf("  shaping %s Sr=%d: mean square sum unshaped %.3g, ld0..3 %.3g %.3g %.3g %.3g\n",
               nyq ? "Nyquist" : "DC", Sr, plain, shaped[0], shaped[1], shaped[2], shaped[3]);
    }
}

/* ---- frames through the reference decoder ---------------------------------- */

static void test_roundtrip(void)
{
    static v90_map_t m;
    uint32_t seed = 31337;
    int trial, bad = 0, frames = 0;
    for (trial = 0; trial < 240; trial++) {
        enum { NF = 60 };
        static uint8_t in[NF][V90_MAX_D], u[NF + 4][6], t[NF + 4][6];
        uint8_t out[V90_MAX_D];
        v90_diff_t df;
        v90_shaper_t sh;
        v90dec_t dec;
        uint64_t prod = 1;
        int i, j, k, K = 0, D, Sr = trial % 4, ld = (trial / 4) % 4, alaw = (trial / 16) & 1;
        memset(&m, 0, sizeof m);
        for (i = 0; i < 6; i++) {
            int M = 1 + (int)(lcg(&seed) % 128);
            spread(&m, i, M, M > 124 ? 127 : 127 - (int)(lcg(&seed) % 4));
            prod *= (uint64_t)m.M[i];
        }
        while (K < 39 && ((uint64_t)1 << (K + 1)) <= prod) K++;
        m.K = (uint8_t)K; m.Sr = (uint8_t)Sr; m.ld = (uint8_t)ld;
        m.a1 = (int8_t)((int)(lcg(&seed) % 129) - 64); m.a2 = (int8_t)((int)(lcg(&seed) % 129) - 64);
        m.b1 = (int8_t)((int)(lcg(&seed) % 129) - 64); m.b2 = (int8_t)((int)(lcg(&seed) % 129) - 64);
        if (v90_map_check(&m) != 0) { bad++; continue; }
        D = V90_MAP_D(&m);
        v90_diff_init(&df);
        for (j = 0; j < NF; j++) {
            for (k = 0; k < D; k++) in[j][k] = (uint8_t)(lcg(&seed) & 1);
            v90_frame_encode(&m, &df, in[j], u[j], t[j]);
        }
        v90_shaper_init(&sh);
        v90dec_init(&dec, &m, alaw);
        for (j = 0; j < NF; j++) {
            uint8_t sign[6], oct[6];
            if (Sr == 0) {
                memcpy(sign, t[j], 6);
            } else {
                /* shape the Sr shaping frames of this data frame, looking
                 * ahead into the following data frames as far as they exist */
                int F = 6 / Sr, f;
                for (f = 0; f < Sr; f++) {
                    int32_t lin[24];
                    uint8_t tt[24];
                    int nf = 0, pos = j * 6 + f * F;
                    while (nf < 4 && pos + (nf + 1) * F <= NF * 6) {
                        for (k = 0; k < F; k++) {
                            int p = pos + nf * F + k;
                            lin[nf * F + k] = v90_ucode_linear(alaw, u[p / 6][p % 6]);
                            tt[nf * F + k] = t[p / 6][p % 6];
                        }
                        nf++;
                    }
                    (void)v90_shape(&sh, &m, lin, tt, nf, sign + f * F);
                }
            }
            for (i = 0; i < 6; i++) oct[i] = v90_pcm_octet(alaw, u[j][i], sign[i]);
            if (v90dec_frame(&dec, oct, out) != D) bad++;
            else if (memcmp(out, in[j], (size_t)D) != 0) bad++;
            frames++;
        }
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(frames, 240 * 60);
}

/* ---- mapping parameters from CP ------------------------------------------- */

static void test_from_cp(void)
{
    static v90_cp_t cp;
    static v90_map_t m;
    int i, u;

    /* CP: drn = 22 -> D = 42 (56000 bit/s); Sr = 0 -> S = 6, K = 36; six
     * 64-point constellations (2^36 exactly). Intervals use constellation 1
     * except interval 3 which uses constellation 0. */
    memset(&cp, 0, sizeof cp);
    cp.is_cp = 1; cp.drn = 22; cp.sr = 0; cp.ld = 2;
    cp.a1 = 10; cp.a2 = -20; cp.b1 = 30; cp.b2 = -40;
    for (i = 0; i < 6; i++) cp.idx[i] = 1;
    cp.idx[3] = 0;
    for (u = 0; u < 128; u += 2) setbit(cp.mask[0], u);          /* even Ucodes */
    for (u = 1; u < 128; u += 2) setbit(cp.mask[1], u);          /* odd Ucodes */
    memset(&m, 0xEE, sizeof m);
    CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 36); CHECK_EQ_I(m.Sr, 0); CHECK_EQ_I(m.ld, 2);
    CHECK_EQ_I(m.a1, 10); CHECK_EQ_I(m.a2, -20); CHECK_EQ_I(m.b1, 30); CHECK_EQ_I(m.b2, -40);
    for (i = 0; i < 6; i++) CHECK_EQ_I(m.M[i], 64);
    CHECK_EQ_I(m.C[0][0], 127); CHECK_EQ_I(m.C[0][1], 125); CHECK_EQ_I(m.C[0][63], 1);
    CHECK_EQ_I(m.C[3][0], 126); CHECK_EQ_I(m.C[3][63], 0);
    CHECK_EQ_I(V90_MAP_D(&m), 42);
    CHECK_EQ_I(v90_map_check(&m), 0);

    /* Sr = 3 -> S = 3, K = 39 > 36 bits of constellation: refused */
    cp.sr = 3; CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
    /* drn = 19 -> D = 39, K = 36 with Sr = 3: fits again */
    cp.drn = 19; CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 36); CHECK_EQ_I(m.Sr, 3);
    /* drn = 1 -> D = 21 (28000), Sr = 0 -> K = 15, the first row of Table 2 */
    cp.drn = 1; cp.sr = 0; CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 15);
    cp.sr = 3; CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 18);
    /* cleardown and out of range */
    cp.drn = 0;  CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_MAP_CLEARDOWN);
    cp.drn = 23; CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
    /* an empty constellation */
    cp.drn = 1; memset(cp.mask[0], 0, 16);
    CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
    for (u = 0; u < 128; u += 2) setbit(cp.mask[0], u);

    /* CPt: D = drn + 8. drn = 1, Sr = 3: D = 9, K = 6 (first row of Table 17) */
    cp.is_cp = 0; cp.drn = 1; cp.sr = 3;
    CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 6); CHECK_EQ_I(V90_MAP_D(&m), 9);
    /* drn = 1, Sr = 0: K = 3, below Table 17 */
    cp.sr = 0; CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
    /* drn = 22, Sr = 0: D = 30, K = 24 (last row); Sr = 3: K = 27, above it */
    cp.drn = 22; CHECK_EQ_I(v90_map_from_cp(&m, &cp), 0);
    CHECK_EQ_I(m.K, 24);
    cp.sr = 3; CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
    /* an index naming a constellation that is not there cannot happen after
     * a parse (the count is max index + 1) but is caught for hand-made input */
    cp.sr = 0; cp.idx[2] = 6;
    CHECK_EQ_I(v90_map_from_cp(&m, &cp), V90_ERR_RANGE);
}

int main(void)
{
    test_map();
    test_modulus();
    test_precode();
    test_frame();
    test_filter();
    test_shape_hand();
    test_shape_props();
    test_shape_effect();
    test_roundtrip();
    test_from_cp();
    T_REPORT("test_enc");
}
