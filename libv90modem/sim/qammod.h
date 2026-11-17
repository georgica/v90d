/* Test peer: QAM modulator at a V.34 symbol rate (double precision).
 * Complex symbols in, 8 kHz real pass-band samples out; root-raised-cosine
 * shaping with a small excess bandwidth, as V.34 transmitters use. */
#ifndef V90D_SIM_QAMMOD_H
#define V90D_SIM_QAMMOD_H

#define QAMMOD_SPAN 16          /* pulse half-length in symbols */
#define QAMMOD_DELAY_SYM QAMMOD_SPAN

typedef void (*qammod_sym_fn)(void *user, double *re, double *im);

typedef struct {
    double sps;                 /* samples per symbol (with any clock offset) */
    double fc, alpha, gain;
    double hr[2 * QAMMOD_SPAN + 1], hi[2 * QAMMOD_SPAN + 1];   /* symbol history, [0] newest */
    double frac;                /* time since the newest symbol instant, in symbols */
    double ph;
    long   n;                   /* samples produced */
    qammod_sym_fn sym; void *user;
} qammod_t;

/* sym_rate in Hz (3000, 3200), ppm = symbol clock offset, fc in Hz (may be
 * offset), gain = line amplitude per unit symbol magnitude */
void   qammod_init(qammod_t *m, double sym_rate, double ppm, double fc, double gain,
                   qammod_sym_fn sym, void *user);
double qammod_tick(qammod_t *m);
/* The first symbol pulled is centred on the line this many samples after the
 * first tick (QAMMOD_SPAN symbols). */
double qammod_delay(const qammod_t *m);

#endif
