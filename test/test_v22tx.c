/* Tests for the V.22bis scrambler, line coder and transmitter. */
#include <string.h>
#include "test.h"
#include "../modem/v22bis.h"

#define FS 8000
static int16_t buf[16000];

static double goertzel(const int16_t *x, int n, double f)
{
    double w = 2.0 * 3.14159265358979323846 * f / FS;
    double c = 2.0 * cos(w), s1 = 0, s2 = 0, s0;
    int i;
    for (i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return (s1 * s1 + s2 * s2 - c * s1 * s2) / ((double)n * n / 4.0);   /* ~amplitude^2 */
}

static uint32_t lfsr = 12345;
static int rndbit(void *u) { (void)u; lfsr = lfsr * 1664525u + 1013904223u; return (lfsr >> 24) & 1; }

/* power in [f0,f1] of a Hann-windowed block (window keeps leakage out of the measurement) */
static double band_power(const int16_t *x, int n, double f0, double f1)
{
    static int16_t w[8000];
    double p = 0, f;
    int i;
    for (i = 0; i < n; i++)
        w[i] = (int16_t)lround(x[i] * (0.5 - 0.5 * cos(2.0 * 3.14159265358979323846 * i / (n - 1))));
    for (f = f0; f <= f1; f += 2.0) p += goertzel(w, n, f);
    return p;
}

int main(void)
{
    scr_t s, d;
    int i, bad;

    /* scrambler from the all-zero state with all-ones input: Ds(n) = 1 ^ Ds(n-14) ^ Ds(n-17) */
    {
        int ref[200], got;
        scr_init(&s); bad = 0;
        for (i = 0; i < 200; i++) {
            ref[i] = 1 ^ (i >= 14 ? ref[i - 14] : 0) ^ (i >= 17 ? ref[i - 17] : 0);
            got = v22_scramble(&s, 1);
            if (got != ref[i]) bad++;
        }
        CHECK_EQ_I(bad, 0);
        CHECK_EQ_I(ref[13], 1); CHECK_EQ_I(ref[14], 0);   /* sanity of the reference */
    }
    /* descrambler inverts the scrambler for arbitrary data, and self-synchronises */
    scr_init(&s); scr_init(&d); bad = 0;
    d.reg = 0x1ABCD;                         /* wrong initial state */
    for (i = 0; i < 2000; i++) {
        int b = rndbit(NULL);
        int o = v22_descramble(&d, v22_scramble(&s, b));
        if (i >= 17 && o != b) bad++;
    }
    CHECK_EQ_I(bad, 0);
    /* 64-ones guard: from the all-ones (lock-up) state, all-ones input would
     * give ones forever; the 65th output must be forced to 0 */
    scr_init(&s); s.reg = 0xFFFFFFFFu; bad = 0;
    for (i = 0; i < 64; i++) if (v22_scramble(&s, 1) != 1) bad++;
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v22_scramble(&s, 1), 0);
    /* the descrambler guard undoes it */
    scr_init(&s); s.reg = 0xFFFFFFFFu; scr_init(&d); d.reg = 0xFFFFFFFFu; bad = 0;
    for (i = 0; i < 300; i++) if (v22_descramble(&d, v22_scramble(&s, 1)) != 1) bad++;
    CHECK_EQ_I(bad, 0);

    /* line coding, Figure 2: the four points of phase quadrant 1 */
    {
        int q, x, y;
        q = 0; v22_encode(&q, 1, 0, &x, &y); CHECK(q == 0 && x == 1 && y == 1);   /* 01 = no change */
        q = 0; v22_encode(&q, 1, 1, &x, &y); CHECK(x == 3 && y == 1);
        q = 0; v22_encode(&q, 1, 2, &x, &y); CHECK(x == 1 && y == 3);
        q = 0; v22_encode(&q, 1, 3, &x, &y); CHECK(x == 3 && y == 3);
        /* Table 1: 00 = +90, 11 = +270, 10 = +180 */
        q = 0; v22_encode(&q, 0, 1, &x, &y); CHECK(q == 1 && x == -1 && y == 3);   /* quadrant 2 "01" */
        q = 0; v22_encode(&q, 3, 1, &x, &y); CHECK(q == 3 && x == 1 && y == -3);   /* quadrant 4 "01" */
        q = 0; v22_encode(&q, 2, 1, &x, &y); CHECK(q == 2 && x == -3 && y == -1);  /* quadrant 3 "01" */
        q = 3; v22_encode(&q, 0, 2, &x, &y); CHECK(q == 0 && x == 1 && y == 3);    /* 4 -> 1 */
        q = 1; v22_encode(&q, 1, 3, &x, &y); CHECK(q == 1 && x == -3 && y == 3);   /* quadrant 2 "11" */
        q = 2; v22_encode(&q, 1, 2, &x, &y); CHECK(x == -1 && y == -3);            /* quadrant 3 "10" */
    }

    /* unscrambled binary 1 at 1200 bit/s = dibit 11 = -90 deg per symbol:
     * a line at fc - 150 Hz */
    {
        v22tx_t t;
        v22tx_init(&t, V22_FC_HIGH, 3000, rndbit, NULL);
        v22tx_set_mode(&t, V22_TX_USB1);
        for (i = 0; i < 8000; i++) buf[i] = v22tx_tick(&t);
        CHECK(goertzel(buf + 800, 7200, 2250.0) > 20.0 * goertzel(buf + 800, 7200, 2400.0));
        CHECK(goertzel(buf + 800, 7200, 2250.0) > 20.0 * goertzel(buf + 800, 7200, 2550.0));
    }
    /* S1 (00,11 alternating = +90,-90): lines at fc and fc +/- 300, none at fc +/- 150 */
    {
        v22tx_t t; double p0, pp, pm, pq;
        v22tx_init(&t, V22_FC_LOW, 3000, rndbit, NULL);
        v22tx_set_mode(&t, V22_TX_S1);
        for (i = 0; i < 8000; i++) buf[i] = v22tx_tick(&t);
        p0 = goertzel(buf + 800, 7200, 1200.0); pp = goertzel(buf + 800, 7200, 1500.0);
        pm = goertzel(buf + 800, 7200, 900.0);  pq = goertzel(buf + 800, 7200, 1050.0);
        CHECK(p0 > 100.0 * pq && pp > 100.0 * pq && pm > 100.0 * pq);
    }
    /* scrambled data: level, and spectrum confined to fc +/- 525 Hz */
    {
        int mode;
        for (mode = 0; mode < 2; mode++) {
            v22tx_t t; double rms = 0, pin, pout;
            v22tx_init(&t, V22_FC_HIGH, 3000, rndbit, NULL);
            v22tx_set_mode(&t, mode ? V22_TX_DATA_2400 : V22_TX_DATA_1200);
            for (i = 0; i < 16000; i++) buf[i] = v22tx_tick(&t);
            for (i = 800; i < 16000; i++) rms += (double)buf[i] * buf[i];
            rms = sqrt(rms / 15200.0);
            CHECK_NEAR(rms, 3000.0, 150.0);
            pin = band_power(buf + 800, 4000, 1875.0, 2925.0);
            pout = band_power(buf + 800, 4000, 600.0, 1700.0);     /* the other channel */
            printf("  v22tx mode %d: other-channel leakage %.1f dB\n", mode, 10.0 * log10(pout / pin));
            CHECK(pout < pin * 1e-4);                                /* > 40 dB down */
        }
    }
    /* silence is silent */
    {
        v22tx_t t; bad = 0;
        v22tx_init(&t, V22_FC_HIGH, 3000, rndbit, NULL);
        for (i = 0; i < 200; i++) if (v22tx_tick(&t) != 0) bad++;
        CHECK_EQ_I(bad, 0);
    }

    /* guard tone (2.1, 2.2): 1800 +/- 20 Hz at 6 +/- 1 dB below the data signal
     * (550 +/- 20 Hz at 3 +/- 1 dB), high channel only, total level unchanged */
    {
        static int16_t g[40000];
        int hz;
        for (hz = 1800; hz >= 550; hz -= 1250) {
            v22tx_t t; double ptot = 0, pg, pd, f, best = 0, fbest = 0;
            v22tx_init(&t, V22_FC_HIGH, 3000, rndbit, NULL);
            v22tx_set_guard(&t, hz);
            v22tx_set_mode(&t, V22_TX_DATA_2400);
            for (i = 0; i < 800; i++) v22tx_tick(&t);
            for (i = 0; i < 40000; i++) { g[i] = v22tx_tick(&t); ptot += (double)g[i] * g[i]; }
            ptot /= 40000.0;
            pg = goertzel(g, 40000, (double)hz) / 2.0;         /* tone power */
            pd = ptot - pg;                                    /* the rest is the data signal */
            for (f = hz - 30.0; f <= hz + 30.0; f += 0.5) { double q = goertzel(g, 8000, f); if (q > best) { best = q; fbest = f; } }
            printf("  v22tx guard tone %d Hz: found at %.1f Hz, %.2f dB below the data signal; data %.2f dB below a modem without it; total %.0f RMS\n",
                   hz, fbest, 10.0 * log10(pd / pg), -10.0 * log10(pd / 9.0e6), sqrt(ptot));
            CHECK_NEAR(fbest, (double)hz, 1.0);
            CHECK_NEAR(10.0 * log10(pd / pg), hz == 1800 ? 6.0 : 3.0, 0.5);
            CHECK_NEAR(sqrt(ptot), 3000.0, 150.0);
            if (hz == 1800) CHECK_NEAR(-10.0 * log10(pd / 9.0e6), 1.0, 0.4);   /* "approximately 1 dB lower" */
        }
        /* not in the low channel, and not while silent */
        {
            v22tx_t t; double ptot = 0; bad = 0;
            v22tx_init(&t, V22_FC_LOW, 3000, rndbit, NULL);
            v22tx_set_guard(&t, 1800);
            v22tx_set_mode(&t, V22_TX_DATA_2400);
            for (i = 0; i < 8000; i++) { g[i] = v22tx_tick(&t); ptot += (double)g[i] * g[i]; }
            CHECK(goertzel(g, 8000, 1800.0) / 2.0 < 1e-3 * ptot / 8000.0);
            v22tx_init(&t, V22_FC_HIGH, 3000, rndbit, NULL);
            v22tx_set_guard(&t, 1800);
            for (i = 0; i < 200; i++) if (v22tx_tick(&t) != 0) bad++;
            CHECK_EQ_I(bad, 0);
        }
    }

    T_REPORT("v22tx");
}
