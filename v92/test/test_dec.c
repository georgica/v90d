/* Upstream decoder (digital modem side).
 *
 * The first two tests feed sequences worked out by hand (they are the
 * encoder's worked examples of test_enc.c, derived there from the
 * Recommendation), so that the decoder is not only checked against the
 * encoder of this library. */
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_enc.h"
#include "v92_dec.h"

#define MAXBITS 200000
static uint8_t g_tx[MAXBITS], g_rx[MAXBITS];
static int g_ntx, g_nrx;
static uint32_t g_seed;

static int get_rand(void *u)
{
    int b = (int)(tu_lcg(&g_seed) & 1);
    (void)u;
    if (g_ntx < MAXBITS) g_tx[g_ntx] = (uint8_t)b;
    g_ntx++;
    return b;
}
static int get_zeros(void *u) { (void)u; return 0; }
static void put_bit(void *u, int b)
{
    (void)u;
    if (g_nrx < MAXBITS) g_rx[g_nrx] = (uint8_t)b;
    g_nrx++;
}

static int count_diff(int from, int to)
{
    int i, n = 0;
    for (i = from; i < to; i++) if (g_tx[i] != g_rx[i]) n++;
    return n;
}

static const uint8_t M21[12] = { 4, 4, 4, 2, 4, 4, 4, 2, 4, 4, 4, 2 };

/* ---- slicer --------------------------------------------------------------- */

static void test_slice(void)
{
    v92_map_t m;
    tu_map_uniform(&m, 21, 0, M21, 4, 100);   /* -700 .. 700 step 200 */
    m.cidx[1] = 1; m.lc[1] = 3;
    m.pt[1][0] = 10; m.pt[1][1] = 50; m.pt[1][2] = 1000;
    CHECK_EQ_I(v92_slice(&m, 0, 100), 0);
    CHECK_EQ_I(v92_slice(&m, 0, 199), 0);
    CHECK_EQ_I(v92_slice(&m, 0, 201), 1);
    CHECK_EQ_I(v92_slice(&m, 0, 200), 0);     /* tie: lower index */
    CHECK_EQ_I(v92_slice(&m, 0, 0), -1);      /* tie between -100 and 100 */
    CHECK_EQ_I(v92_slice(&m, 0, 1), 0);
    CHECK_EQ_I(v92_slice(&m, 0, -1), -1);
    CHECK_EQ_I(v92_slice(&m, 0, -399), -2);
    CHECK_EQ_I(v92_slice(&m, 0, -401), -3);
    CHECK_EQ_I(v92_slice(&m, 0, 32767), 3);
    CHECK_EQ_I(v92_slice(&m, 0, -100000), -4);
    /* interval 1 and 7 use the uneven set -1000 -50 -10 10 50 1000 */
    CHECK_EQ_I(v92_slice(&m, 1, 29), 0);
    CHECK_EQ_I(v92_slice(&m, 1, 31), 1);
    CHECK_EQ_I(v92_slice(&m, 7, 524), 1);
    CHECK_EQ_I(v92_slice(&m, 7, 526), 2);
    CHECK_EQ_I(v92_slice(&m, 7, -526), -3);
    CHECK_EQ_I(v92_slice(&m, 6, 526), 2);     /* interval 6 is set 0 again: 500 */
}

/* ---- hand-derived sequences ----------------------------------------------- */

static void test_worked(void)
{
    /* test_enc.c, test_worked_frame: with N = 8 points +-100..700,
     * M = 4 4 4 2 x 3, K = 21 and 16 states, a first data frame of 21 binary
     * ones is u = -100 -100 300 300  100 -300 -100 -100  300 100 100 -300,
     * and all-zero data is u = 100 for ever. Here the frame of ones is
     * followed by zeros going into the scrambler; what the transmitter then
     * sends comes from the encoder, only the first frame is checked. */
    static const int32_t u0[12] = { -100, -100, 300, 300, 100, -300, -100, -100,
                                    300, 100, 100, -300 };
    v92_map_t m;
    v92_dec_t d;
    int n, i, bad;
    int32_t tent;

    tu_map_uniform(&m, 21, 0, M21, 4, 100);

    /* all zeros */
    g_nrx = 0;
    CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
    for (n = 1; n <= V92_DEC_DELAY + 12 * 4; n++) {
        v92_dec_put(&d, 100, &tent);
        CHECK_EQ_I(tent, 100);
        /* data frame f comes out with symbol V92_DEC_DELAY + 12 f, not before */
        if (n == V92_DEC_DELAY - 1) CHECK_EQ_I(g_nrx, 0);
        if (n == V92_DEC_DELAY) CHECK_EQ_I(g_nrx, 21);
        if (n == V92_DEC_DELAY + 11) CHECK_EQ_I(g_nrx, 21);
        if (n == V92_DEC_DELAY + 12) CHECK_EQ_I(g_nrx, 42);
    }
    CHECK_EQ_I(g_nrx, 21 * 5);
    for (i = 0, bad = 0; i < g_nrx; i++) if (g_rx[i]) bad++;
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(d.frames, 5);
    CHECK_EQ_I(d.bad_frames, 0);
    CHECK_EQ_I(v92_dec_quality(&d), 0);

    /* the frame of ones, in hard mode: no delay, bits out with symbol 12 */
    g_nrx = 0;
    CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
    v92_dec_set_hard(&d, 1);
    for (n = 0; n < 12; n++) {
        CHECK_EQ_I(g_nrx, 0);
        v92_dec_put(&d, u0[n], NULL);
    }
    CHECK_EQ_I(g_nrx, 21);
    for (i = 0, bad = 0; i < 21; i++) if (!g_rx[i]) bad++;
    CHECK_EQ_I(bad, 0);

    /* the same through the Viterbi decoder; the tail is any valid
     * continuation (here: the encoder fed with ones then zeros) */
    {
        v92_enc_t e;
        v92_sym_t s;
        int ones = 21;
        g_nrx = 0;
        CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
        CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_zeros, NULL), 0);
        /* frame 0 by hand; the encoder is run in step only to continue the
         * sequence from the right state */
        {
            static const uint8_t k0[12] = { 3, 3, 1, 0, 0, 2, 3, 1, 1, 0, 0, 1 };
            v92_enc_force_k(&e, k0);
        }
        for (n = 0; n < 12; n++) {
            (void)v92_enc_symbol(&e, &s);
            CHECK_EQ_I(s.u, u0[n]);
            v92_dec_put(&d, u0[n], NULL);
        }
        for (n = 12; n < V92_DEC_DELAY; n++)
            v92_dec_put(&d, v92_enc_symbol(&e, NULL), NULL);
        CHECK_EQ_I(g_nrx, 21);
        for (i = 0, bad = 0; i < ones; i++) if (!g_rx[i]) bad++;
        CHECK_EQ_I(bad, 0);
    }
}

/* ---- noiseless round trips ------------------------------------------------ */

typedef struct { uint32_t seed; } pick_t;
static int pick_rand(void *user, int i, const int16_t *eta, int n)
{
    pick_t *p = (pick_t *)user;
    (void)i; (void)eta;
    return (int)(tu_lcg(&p->seed) % (uint32_t)n);
}

static void make_map2(v92_map_t *m, int tr)
{
    /* two sets, uneven moduli: as test_enc.c test_invariants */
    static const uint8_t M[12] = { 7, 5, 8, 4, 6, 3, 7, 6, 5, 8, 6, 8 };
    int e0;
    tu_map_uniform(m, 30, tr, M, 8, 50);
    m->cidx[1] = m->cidx[4] = 1;
    m->lc[1] = 6;
    for (e0 = 0; e0 < 6; e0++) m->pt[1][e0] = (uint16_t)(40 + 90 * e0 + e0 * e0);
}

/* sign = -1 inverts the line. skip = bits not compared at the start. */
static void run_clean(int tr, int randpick, int sign, int hard, int skip)
{
    v92_map_t m;
    v92_enc_t e;
    v92_dec_t d;
    pick_t pk;
    int n, frames = 400;

    make_map2(&m, tr);
    g_seed = 77u + (uint32_t)tr; g_ntx = g_nrx = 0;
    pk.seed = 3;
    CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_rand, NULL), 0);
    if (randpick) v92_enc_set_pick(&e, pick_rand, &pk);
    CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
    if (hard) v92_dec_set_hard(&d, 1);
    for (n = 0; n < 12 * frames; n++)
        v92_dec_put(&d, sign * v92_enc_symbol(&e, NULL), NULL);
    if (!hard) {
        CHECK_EQ_I(g_nrx, 30 * (frames - (V92_DEC_DELAY + 11) / 12 + 1));
        v92_dec_flush(&d);
    }
    CHECK_EQ_I(g_ntx, 30 * frames);
    CHECK_EQ_I(g_nrx, 30 * frames);
    CHECK_EQ_I(count_diff(skip, 30 * frames), 0);
    CHECK_EQ_I(d.frames, frames);
    if (sign > 0) CHECK_EQ_I(d.bad_frames, 0);
    if (!hard) CHECK_EQ_I(v92_dec_quality(&d), 0);
}

static void test_clean(void)
{
    int tr;
    for (tr = 0; tr < 3; tr++) {
        run_clean(tr, 0, 1, 0, 0);
        run_clean(tr, 1, 1, 0, 0);     /* any member of the class decodes */
        run_clean(tr, 1, 1, 1, 0);     /* hard decisions */
        /* inverted line: the differential sign of 6.4.1 takes one frame to
         * settle, the descrambler 23 more bits, and the trellis decoder was
         * started in the wrong state for the inverted sequence: allow four
         * data frames */
        run_clean(tr, 1, -1, 1, 4 * 30);
        run_clean(tr, 1, -1, 0, 4 * 30);
    }
}

/* ---- error correction ----------------------------------------------------- */

/* A single symbol pushed 60 % of the way to its neighbour is a symbol error
 * for the slicer; the trellis decoder corrects it (the parity of the index
 * sum alone already detects a single step). */
static void test_single_error(void)
{
    v92_map_t m;
    v92_enc_t e;
    v92_dec_t dv, dh;
    static uint8_t rxv[MAXBITS];
    int tr, n, nv, errs_h, errs_v, i;

    for (tr = 0; tr < 3; tr++) {
        make_map2(&m, tr);
        g_seed = 1000u + (uint32_t)tr; g_ntx = 0;
        CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_rand, NULL), 0);
        CHECK_EQ_I(v92_dec_init(&dv, &m, put_bit, NULL), 0);
        CHECK_EQ_I(v92_dec_init(&dh, &m, put_bit, NULL), 0);
        v92_dec_set_hard(&dh, 1);
        /* Viterbi run, keeping its output */
        g_nrx = 0;
        {
            v92_enc_t e2 = e;
            uint32_t seed2 = g_seed;
            int ntx2 = g_ntx;
            for (n = 0; n < 12 * 300; n++) {
                int32_t u = v92_enc_symbol(&e, NULL);
                if (n % 240 == 100 + (n / 240) % 12) u += 60;   /* spacing 100 or ~90 */
                v92_dec_put(&dv, u, NULL);
            }
            v92_dec_flush(&dv);
            nv = g_nrx;
            memcpy(rxv, g_rx, (size_t)nv);
            /* same symbols through the slicer */
            g_seed = seed2; g_ntx = ntx2; g_nrx = 0;
            for (n = 0; n < 12 * 300; n++) {
                int32_t u = v92_enc_symbol(&e2, NULL);
                if (n % 240 == 100 + (n / 240) % 12) u += 60;
                v92_dec_put(&dh, u, NULL);
            }
        }
        CHECK_EQ_I(nv, 30 * 300);
        CHECK_EQ_I(g_nrx, 30 * 300);
        errs_h = count_diff(0, 30 * 300);
        for (i = 0, errs_v = 0; i < nv; i++) if (rxv[i] != g_tx[i]) errs_v++;
        CHECK(errs_h > 15);            /* 15 damaged symbols, each a frame of errors */
        CHECK_EQ_I(errs_v, 0);
        CHECK_EQ_I(dv.bad_frames, 0);
        CHECK(v92_dec_quality(&dv) > 0);
    }
}

/* ---- loss of alignment shows in the quality figure ------------------------ */

static void test_quality(void)
{
    v92_map_t m;
    v92_enc_t e;
    v92_dec_t d;
    int n;
    uint32_t q_ok, q_slip;

    make_map2(&m, 0);
    g_seed = 5; g_ntx = g_nrx = 0;
    CHECK_EQ_I(v92_enc_init(&e, &m, NULL, get_rand, NULL), 0);
    CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
    for (n = 0; n < 2400; n++) v92_dec_put(&d, v92_enc_symbol(&e, NULL) + (n & 1 ? 7 : -7), NULL);
    q_ok = v92_dec_quality(&d);
    CHECK(q_ok >= 30 && q_ok <= 60);           /* 7^2 = 49 */
    (void)v92_enc_symbol(&e, NULL);            /* drop one symbol: frame slip */
    for (n = 0; n < 2400; n++) v92_dec_put(&d, v92_enc_symbol(&e, NULL), NULL);
    q_slip = v92_dec_quality(&d);
    CHECK(q_slip > 10 * q_ok);
}

static void test_init(void)
{
    v92_map_t m;
    v92_dec_t d;
    int tr;
    for (tr = 0; tr < 3; tr++) {
        make_map2(&m, tr);
        CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), 0);
        CHECK_EQ_I(d.nstates, 16 << tr);
        CHECK_EQ_I(d.nin, 4 << tr);
    }
    m.K = 31;
    CHECK_EQ_I(v92_dec_init(&d, &m, put_bit, NULL), V92_ERR_RANGE);
}

int main(void)
{
    test_slice();
    test_worked();
    test_clean();
    test_single_error();
    test_quality();
    test_init();
    T_REPORT("test_dec");
}
