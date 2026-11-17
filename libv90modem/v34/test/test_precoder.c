/* Round trip with non-zero precoder coefficients (V.34 9.6.2): the decoder
 * must work on y(n) = u(n) + c(n), undo c(n) and still deliver the data,
 * for arbitrary coefficients. Clean first, then with noise in the model the
 * precoder is meant for: the receiver's whitening filter 1 + sum h(p) D^p
 * turns x(n) + coloured noise into y(n) + white noise. */
#include "test.h"
#include "testutil.h"

#ifdef __arm__
#define NFRAMES 40
#define NSYMS   20000UL
#else
#define NFRAMES 120
#define NSYMS   250000UL
#endif

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;

/* h(1..3) {re, im} in 2.14 */
static const int16_t H_MILD[3][2]   = { { -9830, 3277 }, { 4096, -1638 }, { -819, 410 } };
    /* -0.6+0.2j, 0.25-0.1j, -0.05+0.025j */
static const int16_t H_STRONG[3][2] = { { -16384, 8192 }, { 3441, -8192 }, { 0, 1720 } };
    /* (1 - 0.7D)(1 - 0.3D)(1 + 0.5jD): minimum phase, zeros at 0.7, 0.3, -0.5j */
static const int16_t H_REAL[3][2]   = { { 14746, 0 }, { 0, 0 }, { 0, 0 } };
    /* 1 + 0.9 D */
static const int16_t H_NONMIN[3][2] = { { -24576, 0 }, { 0, 0 }, { 0, 0 } };
    /* 1 - 1.5 D: NOT minimum phase, 1/H is unstable; legal for the encoder */
static const int16_t H_BIG[3][2]    = { { 21299, -8192 }, { -13107, 9830 }, { 6554, 3277 } };
    /* 1.3-0.5j, -0.8+0.6j, 0.4+0.2j */

static int setup(unsigned S, unsigned R, int trellis, int expanded, const int16_t h[3][2])
{
    v34_cfg_t c;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.expanded = (uint8_t)expanded; c.scrambler = V34_GPA;
    if (h) memcpy(c.h, h, sizeof c.h);
    return v34_params_init(&p, &c);
}

static unsigned long clean(unsigned S, unsigned R, int trellis, int expanded,
                           const int16_t h[3][2], int ydomain)
{
    tu_src_t src;
    tu_sink_t sink;
    unsigned long n, nsym;
    unsigned tent_bad = 0, differs = 0;

    CHECK_EQ_I(setup(S, R, trellis, expanded, h), 0);
    CHECK_EQ_I(p.precode, 1);
    tu_pair_init(&src, &sink, 0x5EED0001u + R);
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, tu_putbit, &sink);
    nsym = (unsigned long)NFRAMES * 8 * p.map.P;
    for (n = 0; n < nsym; n++) {
        v34_pt_t x, y, t;
        v34_tx_symbol(&tx, &x, &y);
        if (x.re != y.re || x.im != y.im) differs++;
        if (ydomain) {
            v34_rx_put_y(&rx, y, &t);
            if (t.re != y.re || t.im != y.im) tent_bad++;
        } else {
            v34_rx_put(&rx, x, &t);
            if (t.re != x.re || t.im != x.im) tent_bad++;
        }
    }
    v34_rx_flush(&rx);
    CHECK(differs > nsym / 2);              /* the precoder really is active */
    CHECK_EQ_I(tent_bad, 0);
    CHECK_EQ_I(rx.b1_errors, 0);
    CHECK_EQ_I(rx.bad_points, 0);
    CHECK_EQ_I(rx.reseeds, 0);
    CHECK_EQ_I(sink.n, (unsigned long)(NFRAMES - 1) * p.map.N);
    return sink.errs;
}

/* BER with noise. mode 0: white noise on y, v34_rx_put_y; mode 1: the same
 * noise passed through 1/H and added to x, v34_rx_put (decoder whitens);
 * hard: trellis decoder bypassed. */
static double noisy(const int16_t h[3][2], double esn0_db, int mode, int hard, unsigned long *reseeds)
{
    tu_src_t src;
    tu_sink_t sink;
    double sigma, nh[3][2] = { { 0, 0 }, { 0, 0 }, { 0, 0 } };
    unsigned long n;

    CHECK_EQ_I(setup(3200, 19200, V34_TRELLIS_16, 0, h), 0);
    /* same noise level as the unprecoded constellation at this Es/N0 */
    sigma = sqrt(p.energy_q8 / 256.0 / (2 * pow(10, esn0_db / 10)));
    tu_pair_init(&src, &sink, 0x77AA0000u);
    tu_seed = 0x13572468u;
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, tu_putbit, &sink);
    v34_rx_set_hard(&rx, hard);
    for (n = 0; n < NSYMS; n++) {
        v34_pt_t x, y, r;
        double wr = sigma * tu_gauss(), wi = sigma * tu_gauss();
        v34_tx_symbol(&tx, &x, &y);
        if (mode == 0) {
            r.re = (int16_t)lrint(y.re + wr * V34_ONE);
            r.im = (int16_t)lrint(y.im + wi * V34_ONE);
            v34_rx_put_y(&rx, r, NULL);
        } else {
            /* coloured noise nx = w / H(D): nx(n) = w(n) - sum h(k) nx(n-k) */
            double nr = wr, ni = wi;
            for (int k = 0; k < 3; k++) {
                double hr = p.cfg.h[k][0] / 16384.0, hi = p.cfg.h[k][1] / 16384.0;
                nr -= hr * nh[k][0] - hi * nh[k][1];
                ni -= hr * nh[k][1] + hi * nh[k][0];
            }
            nh[2][0] = nh[1][0]; nh[2][1] = nh[1][1];
            nh[1][0] = nh[0][0]; nh[1][1] = nh[0][1];
            nh[0][0] = nr; nh[0][1] = ni;
            r.re = (int16_t)lrint(x.re + nr * V34_ONE);
            r.im = (int16_t)lrint(x.im + ni * V34_ONE);
            v34_rx_put(&rx, r, NULL);
        }
    }
    v34_rx_flush(&rx);
    if (reseeds) *reseeds = rx.reseeds;
    return (double)sink.errs / (double)sink.n;
}

int main(void)
{
    static const struct { const int16_t (*h)[2]; const char *name; } sets[] = {
        { H_MILD, "mild" }, { H_STRONG, "strong" }, { H_REAL, "1+0.9D" },
        { H_NONMIN, "1-1.5D (non-minimum phase)" }, { H_BIG, "big" } };
    static const unsigned rates[] = { 4800, 9600, 14400, 19200, 24000, 28800, 31200 };
    unsigned i, r;

    /* ---- clean channel: zero errors for every coefficient set, w = 1 and
     * w = 2 (b >= 56 from 24000 bit/s at 3200 sym/s), all trellis codes ---- */
    for (i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        unsigned long errs = 0;
        for (r = 0; r < sizeof rates / sizeof rates[0]; r++) {
            errs += clean(3200, rates[r], V34_TRELLIS_16, (int)(r & 1), sets[i].h, 0);
            errs += clean(3200, rates[r], V34_TRELLIS_16, (int)(r & 1), sets[i].h, 1);
        }
        errs += clean(3000, 21600, V34_TRELLIS_32, 0, sets[i].h, 0);
        errs += clean(3000, 26400, V34_TRELLIS_64, 1, sets[i].h, 0);
        CHECK_EQ_I(errs, 0);
        printf("  clean round trip, h = %-28s: %lu bit errors\n", sets[i].name, errs);
    }

    /* ---- with noise: 3200 sym/s, 19200 bit/s, 16 states ---- */
    {
        static const double snr[] = { 21.0, 22.0, 23.0 };
        printf("  BER with precoding, 3200/19200/16 states (noise level of the same Es/N0 without precoding)\n");
        printf("    Es/N0 dB  h        no precoding  white on y    coloured on x  slicer on y   reseeds\n");
        for (i = 0; i < 2; i++) {
            const int16_t (*h)[2] = i ? H_STRONG : H_MILD;
            for (r = 0; r < 3; r++) {
                unsigned long rs = 0;
                double b0 = noisy(NULL, snr[r], 0, 0, NULL);
                double by = noisy(h, snr[r], 0, 0, NULL);
                double bx = noisy(h, snr[r], 1, 0, &rs);
                double bh = noisy(h, snr[r], 0, 1, NULL);
                printf("    %6.1f    %-7s  %.3e     %.3e     %.3e      %.3e     %lu\n",
                       snr[r], i ? "strong" : "mild", b0, by, bx, bh, rs);
                /* the trellis decoder must still deliver its gain on the
                 * precoded lattice */
                CHECK(by < bh / 10);
                CHECK(bx < bh / 10);
                if (r == 2) { CHECK(by < 2e-4); CHECK(bx < 2e-4); }
            }
        }
    }
    T_REPORT("test_precoder");
}
