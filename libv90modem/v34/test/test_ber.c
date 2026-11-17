/* Bit error rate with white Gaussian noise on the points: the Viterbi
 * decoder against symbol-by-symbol slicing of the same received points.
 * Prints the curves; requires the coded curve to be "a few dB" better.
 *
 * Es/N0 = (mean energy of the constellation) / (2 sigma^2), sigma^2 the
 * noise variance per dimension. */
#include "test.h"
#include "testutil.h"

#ifdef __arm__                  /* under qemu: keep it short */
#define NBITS 60000UL
#else
#define NBITS 1000000UL
#endif
#define STEP_DB 1.0
#define ENOUGH_ERRS 500         /* stop a point early once both decoders have these */

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rxv, rxs;

/* SNR (dB) at which a curve crosses `target`, by log-linear interpolation;
 * snr[] ascending. NAN if it does not cross. */
static double cross(const double *snr, const double *ber, int n, double target)
{
    for (int i = 1; i < n; i++)
        if (ber[i - 1] >= target && ber[i] < target) {
            /* an error-free point counts as half an error in NBITS */
            double a = log(ber[i - 1]), b = log(ber[i] > 0 ? ber[i] : 0.5 / NBITS), t = log(target);
            return snr[i - 1] + (snr[i] - snr[i - 1]) * (a - t) / (a - b);
        }
    return NAN;
}

static double curve(unsigned S, unsigned R, int trellis, int expanded, double lo_db, double hi_db)
{
    v34_cfg_t c;
    double snr[64], bv[64], bs[64], gain3, gain4 = NAN, v3, s3, v4, s4;
    int n = 0;

    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.expanded = (uint8_t)expanded; c.scrambler = V34_GPA;
    CHECK_EQ_I(v34_params_init(&p, &c), 0);
    printf("  %u sym/s, %u bit/s, %d states, %s shaping: L = %u, mean energy %.2f\n",
           S, R, 16 << trellis, expanded ? "expanded" : "minimum", p.map.L, p.energy_q8 / 256.0);
    printf("    Es/N0 dB   BER slicer    BER Viterbi   bits       v34_rx_quality (dB below signal)\n");
    for (double db = lo_db; db <= hi_db + 1e-9 && n < 64; db += STEP_DB) {
        tu_src_t src;
        tu_sink_t kv, ks;
        double sigma = sqrt(p.energy_q8 / 256.0 / (2 * pow(10, db / 10)));
        unsigned long nsym = (unsigned long)((double)NBITS * S / R) + 8 * p.map.P, i;
        tu_pair_init(&src, &kv, 0xBEEF0000u + (unsigned)n);
        tu_pair_init(&src, &ks, 0xBEEF0000u + (unsigned)n);
        kv.skip = ks.skip = 0;
        v34_tx_init(&tx, &p, tu_getbit, &src);
        v34_rx_init(&rxv, &p, tu_putbit, &kv);
        v34_rx_init(&rxs, &p, tu_putbit, &ks);
        v34_rx_set_hard(&rxs, 1);
        for (i = 0; i < nsym; i++) {
            v34_pt_t x, r;
            v34_tx_symbol(&tx, &x, NULL);
            r = tu_add_noise(x, sigma);
            v34_rx_put(&rxv, r, NULL);
            v34_rx_put(&rxs, r, NULL);
            if ((i & 4095) == 4095 && kv.errs >= ENOUGH_ERRS && ks.errs >= ENOUGH_ERRS)
                break;
        }
        v34_rx_flush(&rxv);
        snr[n] = db;
        bv[n] = (double)kv.errs / (double)kv.n;
        bs[n] = (double)ks.errs / (double)ks.n;
        printf("    %7.1f    %.3e     %.3e     %-9lu  %.1f\n", db, bs[n], bv[n], kv.n,
               10 * log10((double)p.energy_q8 / (v34_rx_quality(&rxv) + 1e-9)));
        n++;
    }
    v3 = cross(snr, bv, n, 1e-3); s3 = cross(snr, bs, n, 1e-3);
    v4 = cross(snr, bv, n, 1e-4); s4 = cross(snr, bs, n, 1e-4);
    gain3 = s3 - v3;
    if (!isnan(v4) && !isnan(s4)) gain4 = s4 - v4;
    printf("    BER 1e-3 at %.2f dB (slicer) / %.2f dB (Viterbi): gain %.2f dB", s3, v3, gain3);
    if (!isnan(gain4)) printf("; at 1e-4: %.2f / %.2f, gain %.2f dB", s4, v4, gain4);
    printf("\n");
    /* the curves must be ordered everywhere in the waterfall region, and
     * decrease */
    for (int i = 0; i < n; i++)
        if (bs[i] > 1e-5 && bs[i] < 0.05) CHECK(bv[i] < bs[i]);
    return gain3;
}

int main(void)
{
    double g;
    /* the V.90 server's normal choice: 16 states, minimum shaping */
    g = curve(3200, 19200, V34_TRELLIS_16, 0, 17.0, 29.0);
    CHECK(g > 2.5);
    g = curve(3200, 28800, V34_TRELLIS_16, 0, 26.0, 38.0);
    CHECK(g > 2.5);
    g = curve(3000, 9600, V34_TRELLIS_16, 0, 8.0, 20.0);
    CHECK(g > 2.5);
#ifndef __arm__
    /* expanded shaping, and the optional codes */
    g = curve(3200, 19200, V34_TRELLIS_16, 1, 17.0, 29.0);
    CHECK(g > 2.5);
    g = curve(3200, 19200, V34_TRELLIS_32, 0, 17.0, 29.0);
    CHECK(g > 2.5);
    g = curve(3200, 19200, V34_TRELLIS_64, 0, 17.0, 29.0);
    CHECK(g > 2.5);
#endif
    T_REPORT("test_ber");
}
