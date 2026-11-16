#include <math.h>
#include "noise.h"

#define TWO_PI 6.283185307179586476925286766559

void rng_seed(rng_t *r, uint32_t seed)
{
    r->s = seed ? seed : 1u;
    r->has_spare = 0;
    r->spare = 0.0;
}

double rng_uniform(rng_t *r)
{
    r->s = r->s * 1664525u + 1013904223u;
    return (double)(r->s >> 8) / 16777216.0; /* 24 bits -> [0,1) */
}

/* Box-Muller, caching the second variate. */
double rng_gauss(rng_t *r)
{
    double u1, u2, mag;
    if (r->has_spare) { r->has_spare = 0; return r->spare; }
    do { u1 = rng_uniform(r); } while (u1 <= 1e-12); /* avoid log(0) */
    u2 = rng_uniform(r);
    mag = sqrt(-2.0 * log(u1));
    r->spare = mag * sin(TWO_PI * u2);
    r->has_spare = 1;
    return mag * cos(TWO_PI * u2);
}

void awgn_add(rng_t *r, double *buf, int n, double dbfs, double full_scale)
{
    /* full-scale sine RMS = full_scale/sqrt(2); target noise RMS scales by 10^(dbfs/20) */
    double sigma = (full_scale / sqrt(2.0)) * pow(10.0, dbfs / 20.0);
    int i;
    for (i = 0; i < n; i++) buf[i] += sigma * rng_gauss(r);
}
