/* Non-linear encoder, V.34 9.7: (9-33) to (9-35) against a double-precision
 * model, the inverse, and the encoder applying it when MP bit 31 asks. */
#include "test.h"
#include "testutil.h"

static int src(void *u) { return tu_src_bit((tu_src_t *)u); }

int main(void)
{
    static v34_params_t p0, p1;
    v34_cfg_t c;
    int re, im, worst_e = 0, worst_d = 0;
    memset(&c, 0, sizeof c);
    c.symbol_rate = 3200; c.data_rate = 28800; c.scrambler = V34_GPA;
    CHECK_EQ_I(v34_params_init(&p0, &c), 0);
    c.nonlinear = 1;
    CHECK_EQ_I(v34_params_init(&p1, &c), 0);        /* was -3: not implemented */
    {
        /* Theta = 0: nothing changes */
        v34_pt_t x = { 1234, -777 }, y = v34_nl_encode(&p0, x);
        CHECK_EQ_I(y.re, 1234); CHECK_EQ_I(y.im, -777);
        y = v34_nl_decode(&p0, x);
        CHECK_EQ_I(y.re, 1234); CHECK_EQ_I(y.im, -777);
    }
    {
        /* a point of the mean energy: zeta = 0.3125, Phi = 1 + 0.3125 / 6 + 0.3125^2 / 120 */
        double avg = p1.energy_q8 / 256.0, r = sqrt(avg);
        v34_pt_t x = { (int16_t)floor(r * 128 + 0.5), 0 }, y = v34_nl_encode(&p1, x);
        CHECK_NEAR(y.re / (double)x.re, 1.0 + 0.3125 / 6 + 0.3125 * 0.3125 / 120, 0.0005);
        CHECK_EQ_I(y.im, 0);
    }
    /* every grid point out to twice the RMS radius, and half-way points (the
     * precoder leaves the grid) */
    for (re = -45 * 128; re <= 45 * 128; re += 64) for (im = -45 * 128; im <= 45 * 128; im += 64) {
        double avg = p1.energy_q8 / 256.0, xr = re / 128.0, xi = im / 128.0;
        double z = 0.3125 * (xr * xr + xi * xi) / avg, phi = 1 + z / 6 + z * z / 120;
        v34_pt_t x, y, b;
        int e;
        if (xr * xr + xi * xi > 4 * avg) continue;      /* no constellation reaches further */
        x.re = (int16_t)re; x.im = (int16_t)im;
        y = v34_nl_encode(&p1, x);
        e = abs(y.re - (int)floor(xr * phi * 128 + 0.5)); if (e > worst_e) worst_e = e;
        e = abs(y.im - (int)floor(xi * phi * 128 + 0.5)); if (e > worst_e) worst_e = e;
        b = v34_nl_decode(&p1, y);
        e = abs(b.re - re); if (e > worst_d) worst_d = e;
        e = abs(b.im - im); if (e > worst_d) worst_d = e;
    }
    printf("  encoder against (9-33): worst %d / 128 of a unit; decoder(encoder(x)) - x: worst %d / 128\n", worst_e, worst_d);
    CHECK(worst_e <= 1); CHECK(worst_d <= 1);
    {
        /* the data mode encoder: with MP bit 31 its output is the same signal, projected */
        static v34_tx_t t0, t1;
        tu_src_t s0 = { 99, 0 }, s1 = { 99, 0 };
        double e0 = 0, e1 = 0;
        int n, bad = 0;
        v34_tx_init(&t0, &p0, src, &s0); v34_tx_init(&t1, &p1, src, &s1);
        for (n = 0; n < 20000; n++) {
            v34_pt_t x0, x1, w;
            v34_tx_symbol(&t0, &x0, NULL); v34_tx_symbol(&t1, &x1, NULL);
            w = v34_nl_encode(&p1, x0);
            if (w.re != x1.re || w.im != x1.im) bad++;
            e0 += (double)x0.re * x0.re + (double)x0.im * x0.im;
            e1 += (double)x1.re * x1.re + (double)x1.im * x1.im;
        }
        CHECK_EQ_I(bad, 0);
        printf("  power of the encoded signal: %+.2f dB\n", 10 * log10(e1 / e0));
        CHECK(e1 > e0); CHECK(10 * log10(e1 / e0) < 1.5);
        /* The encoder raises the mean power, and a transmitter has to give
         * that back to stay at its level: a Conexant CX93001 sends its encoded
         * signal 0.72 dB down (measured on the box, 2026-11-15: its B1 decodes
         * with the receiver's scale at 0.92 and not at 1). energy_tx_q8 is the
         * mean energy of what is sent, for v34_gain_q14. */
        CHECK_EQ_I(p0.energy_tx_q8, p0.energy_q8);
        printf("  energy_tx / energy: %+.2f dB by the parameters\n", 10 * log10((double)p1.energy_tx_q8 / p1.energy_q8));
        CHECK_NEAR(p1.energy_tx_q8 / 256.0, e1 / n / 16384.0, 0.01 * e1 / n / 16384.0);
        CHECK_NEAR(p0.energy_q8 / 256.0, e0 / n / 16384.0, 0.01 * e0 / n / 16384.0);
        CHECK_NEAR(20 * log10((double)v34_gain_q14(p1.energy_tx_q8) / v34_gain_q14(p1.energy_q8)), -0.72, 0.12);
    }
    T_REPORT("nl");
}
