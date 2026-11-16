/* Additive white Gaussian noise for the channel simulator (x86 test peer).
 * Deterministic from a seed so captures replay identically. */
#ifndef V90D_SIM_NOISE_H
#define V90D_SIM_NOISE_H
#include <stdint.h>

typedef struct { uint32_t s; int has_spare; double spare; } rng_t;

void   rng_seed(rng_t *r, uint32_t seed);
double rng_uniform(rng_t *r);           /* [0,1) */
double rng_gauss(rng_t *r);             /* ~N(0,1) */

/* Add white Gaussian noise to buf[0..n), at `dbfs` dB relative to a full-scale
 * sinusoid of amplitude `full_scale` (a full-scale sine has RMS fs/sqrt(2)). */
void awgn_add(rng_t *r, double *buf, int n, double dbfs, double full_scale);

#endif
