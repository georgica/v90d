/* Tests for the clock-offset resampler. */
#include "test.h"
#include "../sim/resamp.h"

#define MAXO 8

int main(void)
{
    double out[MAXO];

    /* ratio 1.0 is (near) pass-through: feed a ramp, get the ramp back */
    {
        resamp_t r;
        int i, k;
        double got[64];
        int ng = 0;
        resamp_init(&r, 1.0);
        for (i = 0; i < 20; i++) {
            k = resamp_push(&r, (double)i, out, MAXO);
            { int j; for (j = 0; j < k; j++) got[ng++] = out[j]; }
        }
        /* first output is x0=0, then 1,2,... */
        CHECK(ng >= 18);
        CHECK_NEAR(got[0], 0.0, 1e-9);
        CHECK_NEAR(got[1], 1.0, 1e-9);
        CHECK_NEAR(got[5], 5.0, 1e-9);
    }

    /* DC preserved under an offset: constant in -> constant out */
    {
        resamp_t r;
        int i, k, bad = 0;
        resamp_init(&r, ppm_ratio(200.0));
        for (i = 0; i < 1000; i++) {
            k = resamp_push(&r, 7.5, out, MAXO);
            { int j; for (j = 0; j < k; j++) if (out[j] < 7.4999 || out[j] > 7.5001) bad = 1; }
        }
        CHECK(bad == 0);
    }

    /* upsample 2x: ~2 outputs per input, midpoints interpolate linearly */
    {
        resamp_t r;
        int total = 0, i, k;
        double last2[2] = {0,0};
        resamp_init(&r, 2.0);
        for (i = 0; i < 100; i++) {
            k = resamp_push(&r, (double)i, out, MAXO);
            total += k;
            if (k >= 2) { last2[0] = out[k-2]; last2[1] = out[k-1]; }
        }
        /* ~2 outputs per input */
        CHECK_NEAR((double)total, 200.0, 3.0);
        /* midpoint between consecutive integers is a half-integer */
        CHECK_NEAR(last2[1] - last2[0], 0.5, 1e-9);
    }

    /* output count tracks the ratio over a long run (+100 ppm) */
    {
        resamp_t r;
        long i, total = 0;
        int k;
        resamp_init(&r, ppm_ratio(100.0));
        for (i = 0; i < 1000000; i++) { k = resamp_push(&r, 1.0, out, MAXO); total += k; }
        /* expect ~ 1,000,000 * 1.0001 = 1,000,100 outputs (+/- a couple) */
        CHECK_NEAR((double)total, 1000100.0, 3.0);
    }

    /* windowed-sinc variant: a 2400 Hz tone through +200 ppm comes out as the
     * ideally time-shifted tone, error more than 70 dB down */
    {
        sresamp_t r;
        double ratio = ppm_ratio(200.0), w = 2.0 * 3.14159265358979323846 * 2400.0 / 8000.0;
        double e2 = 0, s2 = 0;
        long i, k = 0;
        int n, j;
        sresamp_init(&r, ratio);
        for (i = 0; i < 40000; i++) {
            n = sresamp_push(&r, sin(w * (double)i), out, MAXO);
            for (j = 0; j < n; j++, k++) {
                double want = sin(w * ((double)k / ratio));
                if (k > 200) { e2 += (out[j] - want) * (out[j] - want); s2 += want * want; }
            }
        }
        CHECK_NEAR((double)k, 40000.0 * ratio, 3.0 + SRESAMP_TAPS);
        CHECK(e2 < s2 * 1e-7);
    }

    T_REPORT("resamp");
}
