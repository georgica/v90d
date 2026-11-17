/* Tests for the FIR filter. */
#include "test.h"
#include "../sim/filt.h"

int main(void)
{
    /* impulse response equals the taps, then zero */
    {
        static const double h[3] = { 0.5, 0.25, 0.1 };
        double z[3];
        fir_t f;
        fir_init(&f, h, 3, z);
        CHECK_NEAR(fir_push(&f, 1.0), 0.5, 1e-12);
        CHECK_NEAR(fir_push(&f, 0.0), 0.25, 1e-12);
        CHECK_NEAR(fir_push(&f, 0.0), 0.1, 1e-12);
        CHECK_NEAR(fir_push(&f, 0.0), 0.0, 1e-12);
    }

    /* unity tap is a pass-through */
    {
        static const double h[1] = { 1.0 };
        double z[1];
        fir_t f;
        int i;
        fir_init(&f, h, 1, z);
        for (i = 0; i < 5; i++) CHECK_NEAR(fir_push(&f, (double)i), (double)i, 1e-12);
    }

    /* steady-state DC gain equals sum of taps */
    {
        static const double h[4] = { 0.1, 0.2, 0.3, 0.4 };
        double z[4];
        fir_t f;
        int i;
        double y = 0;
        fir_init(&f, h, 4, z);
        for (i = 0; i < 20; i++) y = fir_push(&f, 1.0);
        CHECK_NEAR(y, 1.0, 1e-12); /* sum = 1.0 */
    }

    /* linearity / convolution: y[n] = sum h[k] x[n-k] for a short sequence */
    {
        static const double h[2] = { 1.0, -1.0 }; /* first difference */
        double z[2];
        fir_t f;
        fir_init(&f, h, 2, z);
        CHECK_NEAR(fir_push(&f, 3.0), 3.0, 1e-12);   /* 3 - 0 */
        CHECK_NEAR(fir_push(&f, 7.0), 4.0, 1e-12);   /* 7 - 3 */
        CHECK_NEAR(fir_push(&f, 7.0), 0.0, 1e-12);   /* 7 - 7 */
        CHECK_NEAR(fir_push(&f, 2.0), -5.0, 1e-12);  /* 2 - 7 */
    }

    T_REPORT("filt");
}
