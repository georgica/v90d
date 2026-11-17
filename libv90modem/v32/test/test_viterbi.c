/* Trellis encoder -> noise -> Viterbi decoder -> differential decoder. */
#include "test.h"
#include "v32_priv.h"
#include "noise.h"

#define NSYM 20000
static int qbuf[NSYM];

/* returns bit errors; *hard_err = symbol errors of plain nearest-point decisions.
 * rot = quarter turns applied to every transmitted point (the line's phase
 * ambiguity); sigma = noise per dimension, signal space units. */
static int run(int rate, double sigma, int rot, int nsym, int *hard_err, int skip)
{
    v32_enc_t e = { 0, 0 };
    v32_vit_t v;
    rng_t rng;
    uint32_t l = 12345u + (uint32_t)rate;
    int k, errs = 0, yprev = 0, n = 2 + rate, herr = 0;
    rng_seed(&rng, 77u + (uint32_t)rate);
    v32_vit_init(&v, rate);
    for (k = 0; k < nsym + V32_VDEPTH; k++) {
        int x, y, t, i, label, hard, out;
        l = l * 1103515245u + 12345u;
        if (k < NSYM) qbuf[k] = (l >> 12) & ((1 << n) - 1);
        label = v32_encode(&e, rate, k < NSYM ? qbuf[k] : 0, &x, &y);
        for (i = 0; i < rot; i++) { t = x; x = -y; y = t; }
        out = v32_vit_step(&v, (int32_t)lrint((x + sigma * rng_gauss(&rng)) * V32_UNIT),
                               (int32_t)lrint((y + sigma * rng_gauss(&rng)) * V32_UNIT), &hard);
        if (rot == 0 && hard != label) herr++;
        if (out >= 0) {
            int q = v32_decode_bits(rate, out, &yprev), j = k - (V32_VDEPTH - 1), d;
            if (j >= skip && j < nsym) { d = q ^ qbuf[j]; while (d) { errs += d & 1; d >>= 1; } }
        } else CHECK(k < V32_VDEPTH - 1);
    }
    if (hard_err) *hard_err = herr;
    return errs;
}

int main(void)
{
    int rate, rot;
    /* no noise: exact, every rate, every rotation of the constellation (the
     * first symbol's quadrant bits have no reference) */
    for (rate = 1; rate < V32_NRATES; rate++)
        for (rot = 0; rot < 4; rot++)
            CHECK_EQ_I(run(rate, 0.0, rot, 4000, 0, rot ? 40 : 0), 0);
    /* coding gain: at a noise level where plain decisions are wrong for a few
     * symbols in a hundred, the decoder is nearly clean */
    for (rate = 1; rate < V32_NRATES; rate++) {
        /* minimum distance: sqrt(2) on the staggered grids, 2 on the square
         * ones, in each figure's own scale (x2 for 7200 and 9600) */
        double dmin = (rate == 2) ? 2 * 1.41421356 : (rate == 4) ? 1.41421356 : (rate == 1) ? 4.0 : 2.0;
        double sigma = dmin / 2 / 2.3;
        int herr, errs = run(rate, sigma, 0, NSYM, &herr, 0);
        printf("  %5d bit/s, sigma %.3f: plain decisions %d symbol errors in %d, decoder %d bit errors in %d bits\n",
               V32_BITRATE(rate), sigma, herr, NSYM, errs, NSYM * (2 + rate));
        CHECK(herr > NSYM / 50);
        CHECK(errs * 20 < herr);
    }
    T_REPORT("test_viterbi");
}
