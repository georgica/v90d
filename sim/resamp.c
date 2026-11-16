#include <math.h>
#include "resamp.h"

void resamp_init(resamp_t *r, double ratio)
{
    r->ratio = ratio;
    r->inv = 1.0 / ratio;   /* input-time advance per output sample */
    r->prev = 0.0;
    r->t = 0.0;
    r->idx = 0;
    r->have = 0;
}

/* Output sample k sits at input-time k/ratio. When input index `idx` arrives
 * (value x, with prev = input[idx-1]), emit every output whose time falls in
 * [idx-1, idx), linearly interpolating between prev and x. */
int resamp_push(resamp_t *r, double x, double *out, int maxout)
{
    int k = 0;
    double left;
    if (!r->have) {           /* first sample: becomes prev, nothing to emit */
        r->prev = x;
        r->have = 1;
        r->idx = 1;
        return 0;
    }
    left = (double)(r->idx - 1);          /* input-time of prev */
    while (r->t < (double)r->idx && k < maxout) {
        double frac = r->t - left;        /* in [0,1) */
        out[k++] = r->prev + frac * (x - r->prev);
        r->t += r->inv;
    }
    r->prev = x;
    r->idx++;
    return k;
}

void sresamp_init(sresamp_t *r, double ratio)
{
    int i;
    r->inv = 1.0 / ratio; r->t = 0.0; r->idx = 0; r->pos = 0;
    for (i = 0; i < SRESAMP_TAPS; i++) r->h[i] = 0.0;
}

/* After input idx is stored, the newest sample has index idx. An output at
 * input-time t is produced once samples up to floor(t)+TAPS/2 are in, i.e. it
 * is delayed by SRESAMP_DELAY; it interpolates around tc = t (delayed view). */
int sresamp_push(sresamp_t *r, double x, double *out, int maxout)
{
    const double pi = 3.14159265358979323846;
    int k = 0;
    r->h[r->pos] = x;
    r->pos = (r->pos + 1) % SRESAMP_TAPS;
    /* newest input index is r->idx; centre of the window is idx - DELAY */
    while (r->t <= (double)(r->idx - SRESAMP_DELAY) && r->t > (double)(r->idx - SRESAMP_DELAY) - 1.0 && k < maxout) {
        double acc = 0.0, wsum = 0.0;
        int i;
        for (i = 0; i < SRESAMP_TAPS; i++) {
            long si = r->idx - i;                              /* input index of this tap */
            double d = r->t - (double)si;                      /* distance in samples */
            double a = d / (double)SRESAMP_DELAY;              /* window argument */
            double w, sv;
            if (a <= -1.0 || a >= 1.0) continue;
            w = 0.5 + 0.5 * cos(pi * a);
            sv = (fabs(d) < 1e-12) ? 1.0 : sin(pi * d) / (pi * d);
            acc += r->h[(r->pos - 1 - i + 2 * SRESAMP_TAPS) % SRESAMP_TAPS] * sv * w;
            wsum += sv * w;
        }
        out[k++] = acc / wsum;
        r->t += r->inv;
    }
    /* outputs older than the window (start-up, or ratio > 1 catching up) */
    while (r->t <= (double)(r->idx - SRESAMP_DELAY) - 1.0) r->t += r->inv;
    r->idx++;
    return k;
}
