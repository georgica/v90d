/* Direct-form FIR filter for the channel simulator's loop/line response.
 * Caller owns the taps and the delay-line storage (no allocation here). */
#ifndef V90D_SIM_FILT_H
#define V90D_SIM_FILT_H

typedef struct {
    const double *h;   /* taps, h[0] applies to the newest sample */
    int ntaps;
    double *z;         /* delay line, ntaps doubles, caller-provided */
    int pos;           /* index of the newest sample in z */
} fir_t;

void   fir_init(fir_t *f, const double *taps, int ntaps, double *statebuf);
double fir_push(fir_t *f, double x);  /* push one input, return one output */

#endif
