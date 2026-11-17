/* End to end: CPd -> upstream encoder (precoder and prefilter running) ->
 * a channel that undoes the two filters -> mu-law quantiser -> decoder.
 *
 * The channel is a floating point model written from the equations of 6.4.2
 * solved for u(n), independent of the encoder's fixed point recursions:
 *   v(n) = out(n) / G
 *   x(n) = (v(n) - sum p2(k) v(n-k) - sum(k>=1) z2(k) x(n-k)) / z2(0)
 *   u(n) = x(n) - sum p1(k) x(n-k) - sum z1(k) u(n-k)
 * so with no noise the decoder input must be u(n) again.
 *
 * With noise the trellis decoder is compared with symbol by symbol
 * decisions on the same received values.
 *
 * "test_e2e count <trellis> <mode> <nsymbols>" runs the encoder (mode 0) or
 * the encoder and the decoder (mode 1) for the instruction counts of
 * "make cost-arm"; no floating point is used on that path. */
#include <stdlib.h>
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_enc.h"
#include "v92_dec.h"
#include "v92_seq.h"
#include "v92_info.h"
#include "v90_pcm.h"

#ifdef __arm__
#define NOISY_FRAMES 500        /* qemu: keep the Monte Carlo runs short */
#else
#define NOISY_FRAMES 4000
#endif
#define MAXFRAMES 4000
#define KBITS 54                /* 36000 bit/s: drn = 10 */

static uint8_t g_tx[MAXFRAMES * KBITS], g_rx[MAXFRAMES * KBITS];
static int g_ntx, g_nrx;
static uint32_t g_seed;

/* B1u (48 data frames of ones, 8.7.1), then random data */
static int get_bit(void *u)
{
    int b = g_ntx < V92_B1U_FRAMES * KBITS ? 1 : (int)(tu_lcg(&g_seed) & 1);
    (void)u;
    if (g_ntx < MAXFRAMES * KBITS) g_tx[g_ntx] = (uint8_t)b;
    g_ntx++;
    return b;
}

static void put_bit(void *u, int b)
{
    (void)u;
    if (g_nrx < MAXFRAMES * KBITS) g_rx[g_nrx] = (uint8_t)b;
    g_nrx++;
}

/* ---- parameters a digital modem might send in CPd ------------------------ */

/* Positive mu-law levels (Table 1/V.90, linear) at least dmin apart, the
 * first at least dmin / 2 from zero, lc of them. */
static void make_set(v92_map_t *m, int set, int dmin, int lc)
{
    int u, n = 0, last = -dmin / 2;
    for (u = 0; u < 128 && n < lc; u++) {
        int v = (int)v90_ucode_linear(0, (unsigned)u);
        if (v - last >= dmin) { m->pt[set][n++] = (uint16_t)v; last = v; }
    }
    m->lc[set] = (uint8_t)n;
}

/* Two constellation sets used alternately: 64 points (LC = 32) and 48
 * points (LC = 24). Each equivalence class has two members, which is the
 * precoder's freedom: Mi = N / 2 where k = 0, 1, 2 and Mi = N / 4 where
 * k = 3. Intervals 0..11 use sets 0 1 0 1 ..., so
 *   M = 32 24 32 12  32 24 32 12  32 24 32 12
 * and log2 of the product is 6 * 5 + 3 * 4.585 + 3 * 3.585 = 54.5:
 * K = 54 bits per data frame, 36000 bit/s. */
static void make_params(v92_cpd_t *c, int trellis, unsigned gain4)
{
    static const uint8_t M[12] = { 32, 24, 32, 12, 32, 24, 32, 12, 32, 24, 32, 12 };
    int j;
    memset(c, 0, sizeof *c);
    c->has_mod = c->has_filt = c->has_const = 1;
    c->drn = 10;
    c->map.K = KBITS;
    c->map.trellis = (uint8_t)trellis;
    memcpy(c->map.M, M, 12);
    for (j = 0; j < 6; j++) c->map.cidx[j] = (uint8_t)(j & 1);
    make_set(&c->map, 0, 200, 32);
    make_set(&c->map, 1, 260, 24);
    /* 1 + Z1 = 1 + 0.3 z^-1 - 0.1 z^-2 (zeros at 0.2 and -0.5),
     * 1 - P1 = 1 - 0.5 z^-1 + 0.2 z^-2, Z2 = 0.9 + 0.3 z^-1,
     * 1 - P2 = 1 - 0.2 z^-1: all minimum phase, so the channel that undoes
     * them is stable. */
    c->filt.lz1 = 2; c->filt.lp1 = 2; c->filt.lz2 = 2; c->filt.lp2 = 1;
    c->filt.coef[0] = 9830;      /* z1(1) =  0.3  Q0.15 */
    c->filt.coef[1] = -3277;     /* z1(2) = -0.1 */
    c->filt.coef[2] = 8192;      /* p1(1) =  0.5  Q1.14 */
    c->filt.coef[3] = -3277;     /* p1(2) = -0.2 */
    c->filt.coef[4] = 29491;     /* z2(0) =  0.9  Q0.15 */
    c->filt.coef[5] = 9830;      /* z2(1) =  0.3 */
    c->filt.coef[6] = 3277;      /* p2(1) =  0.2  Q1.14 */
    c->filt.gain4 = (uint16_t)gain4;
}

/* ---- the channel ---------------------------------------------------------- */

typedef struct {
    double z1[2], p1[2], z2[2], p2[1], g;
    double vh[2], xh[2], uh[2];     /* [0] = one symbol ago */
} chan_t;

static void chan_init(chan_t *ch, const v92_filt_t *f)
{
    memset(ch, 0, sizeof *ch);
    ch->z1[0] = f->coef[0] / 32768.0; ch->z1[1] = f->coef[1] / 32768.0;
    ch->p1[0] = f->coef[2] / 16384.0; ch->p1[1] = f->coef[3] / 16384.0;
    ch->z2[0] = f->coef[4] / 32768.0; ch->z2[1] = f->coef[5] / 32768.0;
    ch->p2[0] = f->coef[6] / 16384.0;
    ch->g = f->gain4 / 262144.0;                /* 4G in Q0.16 */
}

/* out: G * v(n) in Q14. Returns u(n) as the far end of the channel sees it. */
static double chan_step(chan_t *ch, int32_t out)
{
    double v = out / 16384.0 / ch->g;
    double x = (v - ch->p2[0] * ch->vh[0] - ch->z2[1] * ch->xh[0]) / ch->z2[0];
    double u = x - ch->p1[0] * ch->xh[0] - ch->p1[1] * ch->xh[1]
                 - ch->z1[0] * ch->uh[0] - ch->z1[1] * ch->uh[1];
    ch->vh[0] = v;
    ch->xh[1] = ch->xh[0]; ch->xh[0] = x;
    ch->uh[1] = ch->uh[0]; ch->uh[0] = u;
    return u;
}

/* zero mean, unit variance (Box-Muller on the test LCG) */
static double gauss(uint32_t *seed)
{
    double a = ((tu_lcg(seed) & 0xFFFFFF) + 1) / 16777217.0;
    double b = (tu_lcg(seed) & 0xFFFFFF) / 16777216.0;
    return sqrt(-2.0 * log(a)) * cos(6.283185307179586 * b);
}

/* the codec's A/D converter: the nearest mu-law level */
static int32_t mulaw_quantise(double r)
{
    static int32_t lev[128];
    double a = fabs(r);
    int lo = 0, hi = 127;
    if (!lev[127]) { int u; for (u = 0; u < 128; u++) lev[u] = v90_ucode_linear(0, (unsigned)u); }
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (a >= lev[mid]) lo = mid; else hi = mid;
    }
    if (a - lev[lo] > lev[hi] - a) lo = hi;
    return r < 0 ? -lev[lo] : lev[lo];
}

/* ---- one run --------------------------------------------------------------- */

typedef struct {
    int    errors;          /* bit errors after the descrambler */
    int    bits;            /* bits compared */
    double max_dev;         /* largest |channel output - u(n)| before noise */
    double out_ms;          /* mean square of G * v(n), 1.0 = design level */
    double x_ms, u_ms;      /* mean squares of x(n) and u(n), linear */
    unsigned bad_frames;
    unsigned quality;
} result_t;

static v92_cpd_t g_cpd, g_cpd_rx;
static uint8_t g_cpd_bits[V92_CPD_MAX_BITS];

static void run(int trellis, unsigned gain4, int hard, double sigma, int frames,
                result_t *res)
{
    static v92_enc_t e;
    static v92_dec_t d;
    chan_t ch;
    uint32_t nseed = 12345;
    int n, len;

    memset(res, 0, sizeof *res);
    /* the parameters travel as a CPd, as they would: 60 bits per 6 symbols */
    make_params(&g_cpd, trellis, gain4);
    len = v92_cpd_build(&g_cpd, 60, g_cpd_bits);
    CHECK(len > 0 && len % 60 == 0);
    CHECK_EQ_I(v92_cpd_parse(g_cpd_bits, len, 60, &g_cpd_rx), len);
    CHECK_EQ_I(v92_map_check(&g_cpd_rx.map), 0);
    CHECK_EQ_I(v92_filt_check(&g_cpd_rx.filt), 0);

    g_seed = 99u + (uint32_t)trellis; g_ntx = g_nrx = 0;
    CHECK_EQ_I(v92_enc_init(&e, &g_cpd_rx.map, &g_cpd_rx.filt, get_bit, NULL), 0);
    CHECK_EQ_I(v92_dec_init(&d, &g_cpd.map, put_bit, NULL), 0);
    if (hard) v92_dec_set_hard(&d, 1);
    chan_init(&ch, &g_cpd.filt);
    for (n = 0; n < 12 * frames; n++) {
        v92_sym_t s;
        int32_t out = v92_enc_symbol(&e, &s);
        double u = chan_step(&ch, out), dev = fabs(u - s.u);
        if (dev > res->max_dev) res->max_dev = dev;
        res->out_ms += (out / 16384.0) * (out / 16384.0);
        res->x_ms += (s.x / 256.0) * (s.x / 256.0);
        res->u_ms += (double)s.u * s.u;
        v92_dec_put(&d, mulaw_quantise(u + sigma * gauss(&nseed)), NULL);
    }
    v92_dec_flush(&d);
    res->out_ms /= 12 * frames;
    res->x_ms /= 12 * frames;
    res->u_ms /= 12 * frames;
    CHECK_EQ_I(g_ntx, KBITS * frames);
    CHECK_EQ_I(g_nrx, KBITS * frames);
    for (n = 0; n < KBITS * frames; n++) res->errors += g_tx[n] != g_rx[n];
    res->bits = KBITS * frames;
    res->bad_frames = d.bad_frames;
    res->quality = v92_dec_quality(&d);
}

/* ---- tests ----------------------------------------------------------------- */

static unsigned g_gain4;

static void test_params(void)
{
    v92_info1a_t a;
    int i;
    make_params(&g_cpd, 0, 100);
    CHECK_EQ_I(g_cpd.map.lc[0], 32);
    CHECK_EQ_I(g_cpd.map.lc[1], 24);
    CHECK_EQ_I(v92_map_check(&g_cpd.map), 0);
    CHECK_EQ_I(V92_UP_K(g_cpd.drn), KBITS);
    CHECK_EQ_I(V92_UP_BPS(g_cpd.drn), 36000);
    /* points are mu-law levels at least 200 (260) apart */
    for (i = 1; i < 32; i++) CHECK(g_cpd.map.pt[0][i] - g_cpd.map.pt[0][i - 1] >= 200);
    for (i = 1; i < 24; i++) CHECK(g_cpd.map.pt[1][i] - g_cpd.map.pt[1][i - 1] >= 260);
    CHECK(2 * g_cpd.map.pt[0][0] >= 200);
    CHECK(2 * g_cpd.map.pt[1][0] >= 260);
    /* the smallest analogue modem (p1 and z2 only, 192 taps) could not take
     * these filters, one with all four sections can */
    memset(&a, 0, sizeof a);
    a.kind = V92_INFO1A_PCM;
    CHECK_EQ_I(v92_filt_allowed(&g_cpd.filt, &a), 0);
    a.sections = 3;
    CHECK_EQ_I(v92_filt_allowed(&g_cpd.filt, &a), 1);
}

/* 8.8.3: the digital modem designs G so that G * v(n) has a mean square of
 * 1. Measure v(n) with a trial gain, then set 4G. */
static void test_gain(void)
{
    result_t r;
    double g;
    run(0, 1000, 0, 0.0, 300, &r);
    g = (1000 / 262144.0) / sqrt(r.out_ms);
    g_gain4 = (unsigned)(4 * g * 65536.0 + 0.5);
    CHECK(g_gain4 > 20 && g_gain4 < 65536);
    run(0, g_gain4, 0, 0.0, 300, &r);
    CHECK_NEAR(r.out_ms, 1.0, 0.05);
    printf("  4G = %u, mean square of G*v = %.4f, rms u = %.0f, rms x = %.0f\n",
           g_gain4, r.out_ms, sqrt(r.u_ms), sqrt(r.x_ms));
}

static void test_clean(void)
{
    result_t r;
    int tr, i, bad;
    for (tr = 0; tr < 3; tr++) {
        run(tr, g_gain4, 0, 0.0, 200, &r);
        /* the float channel recovers u(n) from the fixed point encoder:
         * what is left is rounding (v to 1/256, the output to Q14) */
        CHECK(r.max_dev < 1.0);
        CHECK_EQ_I(r.errors, 0);
        CHECK_EQ_I((int)r.bad_frames, 0);
        CHECK_EQ_I((int)r.quality, 0);
        /* B1u came out as ones */
        for (i = 0, bad = 0; i < V92_B1U_FRAMES * KBITS; i++) bad += !g_rx[i];
        CHECK_EQ_I(bad, 0);
        /* the precoder's freedom is used: x(n) is not simply u(n), and its
         * power stays near that of u(n) */
        CHECK(r.x_ms < 1.5 * r.u_ms);
        run(tr, g_gain4, 1, 0.0, 200, &r);
        CHECK_EQ_I(r.errors, 0);
    }
    printf("  no noise: max |channel output - u(n)| = %.3f linear units\n", r.max_dev);
}

static void test_noise(void)
{
    static const double sigma[2] = { 40.0, 55.0 };
    result_t hard, soft;
    int tr, s;
    for (s = 0; s < 2; s++) {
        for (tr = 0; tr < 3; tr++) {
            run(tr, g_gain4, 1, sigma[s], NOISY_FRAMES, &hard);
            run(tr, g_gain4, 0, sigma[s], NOISY_FRAMES, &soft);
            printf("  sigma %.0f, %d states: bit errors %d hard, %d trellis of %d\n",
                   sigma[s], 16 << tr, hard.errors, soft.errors, soft.bits);
            CHECK(hard.errors > 100);
            CHECK(soft.errors * 5 < hard.errors);
        }
    }
}

/* ---- instruction counting (make cost-arm) --------------------------------- */

static int get_lcg(void *u) { (void)u; return (int)(tu_lcg(&g_seed) & 1); }
static void put_null(void *u, int b) { (void)u; g_nrx += b; }

static int count_main(int trellis, int mode, int nsym)
{
    static v92_enc_t e;
    static v92_dec_t d;
    int n;
    make_params(&g_cpd, trellis, 87);
    g_seed = 5;
    if (v92_enc_init(&e, &g_cpd.map, &g_cpd.filt, get_lcg, NULL)) return 2;
    if (v92_dec_init(&d, &g_cpd.map, put_null, NULL)) return 2;
    for (n = 0; n < nsym; n++) {
        v92_sym_t s;
        (void)v92_enc_symbol(&e, &s);
        if (mode) v92_dec_put(&d, s.u, NULL);
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "count"))
        return count_main(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    test_params();
    test_gain();
    test_clean();
    test_noise();
    T_REPORT("test_e2e");
}
