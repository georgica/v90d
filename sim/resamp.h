/* Streaming fractional resampler: models the client modem's sample clock
 * running at a small offset from the network 8 kHz clock.
 *
 * ratio = output samples per input sample = fs_out/fs_in = 1 + ppm*1e-6.
 * Linear interpolation for now (adequate to model clock drift; the modem's
 * timing recovery tracks it). Upgradeable to windowed-sinc without API change.
 */
#ifndef V90D_SIM_RESAMP_H
#define V90D_SIM_RESAMP_H

typedef struct {
    double ratio;   /* out/in */
    double inv;     /* 1/ratio: input-time step per output sample */
    double prev;    /* previous input sample */
    double t;       /* input-time of the next output sample */
    long   idx;     /* input index of the next incoming sample */
    int    have;    /* seen at least one input */
} resamp_t;

void resamp_init(resamp_t *r, double ratio);
/* Push one input sample; write up to maxout output samples; return count. */
int  resamp_push(resamp_t *r, double x, double *out, int maxout);

/* High-quality variant: Hann-windowed sinc, SRESAMP_TAPS taps. Same contract;
 * output k is the input signal at input-time k/ratio (it is merely emitted
 * SRESAMP_DELAY input samples later). Use
 * this one when interpolation noise would otherwise limit a QAM/PCM receiver
 * test (ratios near 1 only: there is no anti-alias scaling). */
#define SRESAMP_TAPS  48
#define SRESAMP_DELAY (SRESAMP_TAPS / 2)
typedef struct {
    double inv, t;
    long   idx;
    double h[SRESAMP_TAPS];   /* ring of the last inputs */
    int    pos;
} sresamp_t;

void sresamp_init(sresamp_t *r, double ratio);
int  sresamp_push(sresamp_t *r, double x, double *out, int maxout);

/* Convenience for ppm offset. */
static inline double ppm_ratio(double ppm) { return 1.0 + ppm * 1e-6; }

#endif
