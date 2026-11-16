/* Tests for the ANSam generator. */
#include "test.h"
#include "../modem/ansam.h"

#define FS 8000
#define N 16000   /* 2 s */

static int16_t buf[N];

static double goertzel(const int16_t *x, int n, double f, int fs)
{
    double w = 2.0 * 3.14159265358979323846 * f / fs;
    double c = 2.0 * cos(w), s1 = 0, s2 = 0, s0;
    int i;
    for (i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int main(void)
{
    ansam_t a;
    int i, ns;

    /* carrier at 2100 Hz dominates nearby frequencies */
    ansam_init(&a, FS, 8192, 0);
    ns = ansam_run(&a, buf, N);
    CHECK_EQ_I(ns, N);
    {
        double p2100 = goertzel(buf, ns, 2100.0, FS);
        double p1700 = goertzel(buf, ns, 1700.0, FS);
        double p2500 = goertzel(buf, ns, 2500.0, FS);
        CHECK(p2100 > p1700 * 20.0);
        CHECK(p2100 > p2500 * 20.0);
    }

    /* 15 Hz amplitude modulation: short-term power swings ~ (1.2/0.8)^2 = 2.25 */
    {
        double pmax = 0, pmin = 1e30;
        int w = 80; /* 10 ms window: averages the carrier, tracks the envelope */
        int k;
        for (k = 0; k + w <= ns; k += w) {
            double p = 0; int j;
            for (j = 0; j < w; j++) p += (double)buf[k + j] * buf[k + j];
            p /= w;
            if (p > pmax) pmax = p;
            if (p < pmin && p > 0) pmin = p;
        }
        CHECK_NEAR(pmax / pmin, 2.25, 0.6);
    }

    /* envelope average amplitude ~ requested amp (RMS of a sine = amp/sqrt2) */
    {
        double sumsq = 0;
        for (i = 0; i < ns; i++) sumsq += (double)buf[i] * buf[i];
        CHECK_NEAR(sqrt(sumsq / ns), 8192.0 / sqrt(2.0), 8192.0 * 0.08);
    }

    /* phase reversals: a 180-deg reversal at t=450 ms inverts the carrier, so
     * after the reversal the signal correlates NEGATIVELY with the non-reversed
     * run (buf, reversals=0), while before it they are identical. */
    {
        ansam_t b;
        static int16_t rbuf[N];
        int rev_at = (int)((double)ANSAM_REV_MS / 1000.0 * FS); /* 3600 */
        double c_before = 0, c_after = 0;
        int j;

        ansam_init(&b, FS, 8192, 1);
        ansam_run(&b, rbuf, N);
        for (j = 100; j < rev_at - 100; j++) c_before += (double)rbuf[j] * buf[j];
        for (j = rev_at + 100; j < 2 * rev_at - 100; j++) c_after += (double)rbuf[j] * buf[j];
        CHECK(c_before > 0.0);   /* identical before the reversal */
        CHECK(c_after < 0.0);    /* carrier inverted after the reversal */
    }

    T_REPORT("ansam");
}
