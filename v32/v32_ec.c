/* V.32bis echo canceller.
 *
 * Both directions share the band, so everything we transmit comes back on top
 * of the wanted signal: at once from the local hybrid (near echo) and, a
 * round trip later, from the far end's hybrid (far echo). Two FIR models of
 * those paths at the sample rate, adapted together by NLMS on the one
 * residual: the near one on the last 12 ms of the reference, the far one on
 * a 12 ms window `bulk` samples back (placed from the round trip measured at
 * start-up). They are trained while the far end is silent (our first TRN
 * segment) and then frozen: during double talk the wanted signal is noise to
 * the adaptation.
 *
 * The reference is not the line signal itself. That has next to nothing at
 * the band edges, where the echo path must still be modelled, and a model
 * driven by it converged to about 27 dB in the 1280 symbols of TRN. The
 * transmitter supplies instead the same symbols on a wide, flat pulse
 * (v32tx_t.ref), six symbols ahead of the line signal so that the echo is
 * causal with respect to it; the model then is "flat reference -> echo" and
 * all of its modes adapt at the same speed (about 50 dB in 1280 symbols).
 *
 * (A canceller driven by the transmit symbols, ten interleaved symbol-spaced
 * filters, was tried first: it costs less per sample but each coefficient set
 * is touched only 800 times a second and it needed three times the TRN.) */
#include "v32_priv.h"

#define HMASK (V32_EC_HIST - 1)

void v32ec_init(v32ec_t *e)
{
    int i;
    for (i = 0; i < V32_EC_HIST; i++) e->hist[i] = 0;
    for (i = 0; i < 2 * V32_EC_NEAR; i++) e->xn[i] = 0;
    for (i = 0; i < 2 * V32_EC_FAR; i++) e->xf[i] = 0;
    for (i = 0; i < V32_EC_NEAR; i++) e->hn[i] = 0;
    for (i = 0; i < V32_EC_FAR; i++) e->hf[i] = 0;
    e->hpos = 0; e->npos = 0; e->fpos = 0;
    e->energy = 0;
    e->bulk = V32_EC_NEAR; e->near_delay = 0;
    e->pw_in = e->pw_out = 0;
}

void v32ec_set_near_delay(v32ec_t *e, int samples)
{
    if (samples < 0) samples = 0;
    if (samples > V32_EC_HIST / 2) samples = V32_EC_HIST / 2;
    e->near_delay = samples;
    if (e->bulk < samples + V32_EC_NEAR) e->bulk = samples + V32_EC_NEAR;
}

void v32ec_set_bulk(v32ec_t *e, int samples)
{
    int i;
    if (samples < e->near_delay + V32_EC_NEAR) samples = e->near_delay + V32_EC_NEAR;
    if (samples > V32_EC_HIST - V32_EC_FAR - 1) samples = V32_EC_HIST - V32_EC_FAR - 1;
    e->bulk = samples;
    e->fpos = 0;
    e->energy = 0;
    for (i = 0; i < V32_EC_FAR; i++) {
        int16_t x = e->hist[(e->hpos - samples - i) & HMASK];
        e->xf[i] = e->xf[i + V32_EC_FAR] = x;
        e->energy += (int32_t)x * x;
        e->hf[i] = 0;
    }
    for (i = 0; i < V32_EC_NEAR; i++) e->energy += (int32_t)e->xn[e->npos + i] * e->xn[e->npos + i];
}

int32_t v32ec_tick(v32ec_t *e, int16_t tx, int32_t rx, int mu_shift)
{
    const int16_t *xn, *xf;
    int64_t acc = 0;
    int32_t err, old, xfar, xnear;
    int i;

    /* slide both delay lines; keep the energy sum */
    e->hpos = (e->hpos + 1) & HMASK;
    e->hist[e->hpos] = tx;
    if (--e->npos < 0) e->npos = V32_EC_NEAR - 1;
    old = e->xn[e->npos];
    xnear = e->hist[(e->hpos - e->near_delay) & HMASK];
    e->energy += xnear * xnear - old * old;
    e->xn[e->npos] = e->xn[e->npos + V32_EC_NEAR] = (int16_t)xnear;
    if (--e->fpos < 0) e->fpos = V32_EC_FAR - 1;
    old = e->xf[e->fpos];
    xfar = e->hist[(e->hpos - e->bulk) & HMASK];
    e->energy += xfar * xfar - old * old;
    e->xf[e->fpos] = e->xf[e->fpos + V32_EC_FAR] = (int16_t)xfar;
    xn = &e->xn[e->npos]; xf = &e->xf[e->fpos];

    for (i = 0; i < V32_EC_NEAR; i++) acc += (int64_t)e->hn[i] * xn[i];
    for (i = 0; i < V32_EC_FAR; i++)  acc += (int64_t)e->hf[i] * xf[i];
    err = rx - (int32_t)((acc + (1 << 23)) >> 24);

    e->pw_in  += (((int64_t)rx * rx << 8) - e->pw_in) >> 9;
    e->pw_out += (((int64_t)err * err << 8) - e->pw_out) >> 9;

    if (mu_shift >= 0 && e->energy > 65536) {
        /* NLMS: h += 2^-mu . err . x / energy, h in Q24. The energy is
         * brought to 16 bits so that err / energy fits 32 bits as Q30. */
        int64_t en = e->energy;
        int32_t g, c = err;
        int sh = 0, s2;
        while (en >= 65536) { en >>= 1; sh++; }
        if (c > 32767) c = 32767; else if (c < -32768) c = -32768;
        g = (int32_t)(((int64_t)c << 30) / (int32_t)en);
        int64_t rnd;
        s2 = 6 + sh + mu_shift;
        rnd = (int64_t)1 << (s2 - 1);       /* rounded: truncation is a steady drift of every tap */
        for (i = 0; i < V32_EC_NEAR; i++) e->hn[i] += (int32_t)(((int64_t)g * xn[i] + rnd) >> s2);
        for (i = 0; i < V32_EC_FAR; i++)  e->hf[i] += (int32_t)(((int64_t)g * xf[i] + rnd) >> s2);
    }
    return err;
}
