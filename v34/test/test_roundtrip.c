/* Encoder -> decoder on clean points: every data rate of Table 8 that is a
 * multiple of 2400 bit/s, at every symbol rate, both shaping choices, all
 * three trellis codes. Zero bit errors required. */
#include "test.h"
#include "testutil.h"

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;

/* returns bit errors; checks the bookkeeping on the way */
static unsigned long run(unsigned S, unsigned R, int trellis, int expanded, int poly,
                         unsigned frames, int use_put_y, double *energy)
{
    v34_cfg_t c;
    tu_src_t src;
    tu_sink_t sink;
    unsigned long n, nsym;
    unsigned tent_bad = 0;
    double e = 0;

    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.expanded = (uint8_t)expanded; c.scrambler = (uint8_t)poly;
    if (v34_params_init(&p, &c)) { CHECK(0); return 1; }
    tu_pair_init(&src, &sink, 0xA5A5F00Du ^ (S * 31 + R));
    v34_tx_init(&tx, &p, tu_getbit, &src);
    v34_rx_init(&rx, &p, tu_putbit, &sink);
    nsym = (unsigned long)frames * 8 * p.map.P;
    for (n = 0; n < nsym; n++) {
        v34_pt_t x, t;
        v34_tx_symbol(&tx, &x, NULL);
        e += (double)x.re * x.re + (double)x.im * x.im;
        if (use_put_y) v34_rx_put_y(&rx, x, &t);
        else           v34_rx_put(&rx, x, &t);
        if (t.re != x.re || t.im != x.im) tent_bad++;
    }
    v34_rx_flush(&rx);
    if (energy) *energy = e / nsym / (V34_ONE * V34_ONE);
    CHECK_EQ_I(tent_bad, 0);                       /* tentative decisions */
    CHECK_EQ_I(rx.b1_bits, p.map.N);               /* B1 = one data frame */
    CHECK_EQ_I(rx.b1_errors, 0);
    CHECK_EQ_I(rx.bad_points, 0);
    CHECK_EQ_I(sink.n, (unsigned long)(frames - 1) * p.map.N);   /* every bit delivered */
    CHECK_EQ_I(src.n, sink.n);                     /* and nothing left in the encoder */
    CHECK_EQ_I(v34_rx_quality(&rx), 0);
    return sink.errs + (sink.n != (unsigned long)(frames - 1) * p.map.N);
}

int main(void)
{
    unsigned s, R, e, cases = 0;
    unsigned long errs_total = 0;

    for (s = 0; s < 6; s++) {
        unsigned S = tu_srates[s];
        for (R = tu_min_rate(S); R <= tu_max_rate(S); R += 2400)
            for (e = 0; e < 2; e++) {
                /* B1 + 15 data frames: more than two superframes, so the bit
                 * inversion pattern wraps */
                unsigned long errs = run(S, R, V34_TRELLIS_16, (int)e, V34_GPA, 16, 0, NULL);
                CHECK_EQ_I(errs, 0);
                if (errs) printf("  %u sym/s %u bit/s expanded=%u: %lu bit errors\n", S, R, e, errs);
                errs_total += errs;
                cases++;
            }
    }
    printf("  round trip, 16 states: %u (symbol rate, data rate, shaping) cases, %lu bit errors\n",
           cases, errs_total);

    /* the same for the 32- and 64-state codes at the two V.90 symbol rates */
    for (int tr = V34_TRELLIS_32; tr <= V34_TRELLIS_64; tr++) {
        cases = 0; errs_total = 0;
        for (s = 3; s < 5; s++)
            for (R = 4800; R <= tu_max_rate(tu_srates[s]); R += 2400) {
                unsigned long errs = run(tu_srates[s], R, tr, (R / 2400) & 1, V34_GPA, 16, 0, NULL);
                CHECK_EQ_I(errs, 0);
                errs_total += errs;
                cases++;
            }
        printf("  round trip, %d states: %u cases, %lu bit errors\n", 16 << tr, cases, errs_total);
    }
    /* other scrambler polynomial; the y-domain entry point */
    CHECK_EQ_I(run(3200, 26400, V34_TRELLIS_16, 1, V34_GPC, 16, 0, NULL), 0);
    CHECK_EQ_I(run(3200, 26400, V34_TRELLIS_16, 1, V34_GPA, 16, 1, NULL), 0);
    CHECK_EQ_I(run(3000, 4800, V34_TRELLIS_16, 0, V34_GPA, 16, 1, NULL), 0);

    /* nominal energy (params.energy_q8, from the shell mapper's ring
     * statistics) against the energy actually produced with random data */
    {
        double worst = 0;
        for (s = 3; s < 5; s++)
            for (R = 4800; R <= tu_max_rate(tu_srates[s]); R += 2400)
                for (e = 0; e < 2; e++) {
                    double meas, nom, rel;
                    CHECK_EQ_I(run(tu_srates[s], R, V34_TRELLIS_16, (int)e, V34_GPA, 150, 0, &meas), 0);
                    nom = p.energy_q8 / 256.0;
                    rel = fabs(meas - nom) / nom;
                    if (rel > worst) worst = rel;
                    CHECK(rel < 0.03);
                    if (rel >= 0.03)
                        printf("  energy %u/%u/%u: nominal %.2f measured %.2f\n", tu_srates[s], R, e, nom, meas);
                }
        printf("  nominal vs measured mean energy, 3000 and 3200 sym/s: worst deviation %.2f %%\n", 100 * worst);
    }
    T_REPORT("test_roundtrip");
}
