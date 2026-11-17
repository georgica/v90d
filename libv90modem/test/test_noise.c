/* Tests for the AWGN generator. */
#include "test.h"
#include "../sim/noise.h"

#define N 200000

static double buf[N];

int main(void)
{
    rng_t a, b;
    int i;
    double sum, sumsq, mean, var, rms;

    /* determinism: same seed -> identical gaussian sequence */
    rng_seed(&a, 12345);
    rng_seed(&b, 12345);
    {
        int same = 1;
        for (i = 0; i < 1000; i++)
            if (rng_gauss(&a) != rng_gauss(&b)) same = 0;
        CHECK(same);
    }

    /* different seeds -> different sequence (not all equal) */
    rng_seed(&a, 1);
    rng_seed(&b, 2);
    {
        int diff = 0;
        for (i = 0; i < 1000; i++)
            if (rng_gauss(&a) != rng_gauss(&b)) diff = 1;
        CHECK(diff);
    }

    /* N(0,1): mean ~ 0, variance ~ 1 */
    rng_seed(&a, 777);
    sum = sumsq = 0;
    for (i = 0; i < N; i++) { double g = rng_gauss(&a); sum += g; sumsq += g * g; }
    mean = sum / N;
    var = sumsq / N - mean * mean;
    CHECK_NEAR(mean, 0.0, 0.02);
    CHECK_NEAR(var, 1.0, 0.03);

    /* uniform in [0,1): mean ~ 0.5, all within range */
    rng_seed(&a, 42);
    sum = 0;
    {
        int inrange = 1;
        for (i = 0; i < N; i++) { double u = rng_uniform(&a); sum += u; if (u < 0.0 || u >= 1.0) inrange = 0; }
        CHECK(inrange);
    }
    CHECK_NEAR(sum / N, 0.5, 0.01);

    /* awgn level: at -20 dBFS vs full_scale=32124, RMS = (32124/sqrt2)*10^(-20/20) */
    rng_seed(&a, 9);
    for (i = 0; i < N; i++) buf[i] = 0.0;
    awgn_add(&a, buf, N, -20.0, 32124.0);
    sumsq = 0;
    for (i = 0; i < N; i++) sumsq += buf[i] * buf[i];
    rms = sqrt(sumsq / N);
    {
        double want = (32124.0 / sqrt(2.0)) * 0.1; /* -20 dB = x0.1 */
        CHECK_NEAR(rms, want, want * 0.03);        /* within 3% */
    }

    /* awgn adds to existing signal (does not overwrite) */
    rng_seed(&a, 9);
    for (i = 0; i < N; i++) buf[i] = 1000.0;
    awgn_add(&a, buf, N, -40.0, 32124.0);
    sum = 0;
    for (i = 0; i < N; i++) sum += buf[i];
    CHECK_NEAR(sum / N, 1000.0, 20.0); /* mean stays ~ signal mean */

    T_REPORT("noise");
}
