#include "filt.h"

void fir_init(fir_t *f, const double *taps, int ntaps, double *statebuf)
{
    int i;
    f->h = taps;
    f->ntaps = ntaps;
    f->z = statebuf;
    f->pos = 0;
    for (i = 0; i < ntaps; i++) f->z[i] = 0.0;
}

double fir_push(fir_t *f, double x)
{
    int k, idx = f->pos;
    double acc = 0.0;
    f->z[f->pos] = x;            /* newest sample */
    for (k = 0; k < f->ntaps; k++) {
        acc += f->h[k] * f->z[idx];   /* h[0] * newest, h[1] * previous, ... */
        if (--idx < 0) idx += f->ntaps;
    }
    if (++f->pos >= f->ntaps) f->pos = 0;
    return acc;
}
