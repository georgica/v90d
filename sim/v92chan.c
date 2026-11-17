/* Test channel for V.92 PCM upstream. See v92chan.h. */
#include <string.h>
#include <math.h>
#include "v92chan.h"
#include "../v90/v90_pcm.h"
#include "../modem/pcmenc.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;

/* second order low-pass (hp = 0) or high-pass section, corner f, quality q,
 * at sampling rate fs (bilinear transform) */
static void bq_set(bq_t *b, int hp, double f, double q, double fs)
{
    double w = 2 * M_PI * f / fs, al = sin(w) / (2 * q), c = cos(w), a0 = 1 + al;
    if (hp) { b->b0 = (1 + c) / 2 / a0; b->b1 = -(1 + c) / a0; b->b2 = b->b0; }
    else    { b->b0 = (1 - c) / 2 / a0; b->b1 = (1 - c) / a0;  b->b2 = b->b0; }
    b->a1 = -2 * c / a0; b->a2 = (1 - al) / a0; b->z1 = b->z2 = 0;
}
static double bq_run(bq_t *b, double x)
{
    double y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

void v92chan_init(v92chan_t *c, const v92chan_cfg *cfg)
{
    const double fs = 8000.0 * V92CH_OS;
    bq_t s[8];
    int ns = 0, i, k, n = V92CH_LEN * V92CH_OS;
    double lp = 0, e = 0, alpha, mix;

    memset(c, 0, sizeof *c);
    c->cfg = *cfg;
    rng_seed(&c->rng, cfg->seed ? cfg->seed : 1);
    /* transmit reconstruction filter and codec anti-alias filter: 6th order
     * Butterworth at 3550 Hz; codec high-pass: 2nd order at 140 Hz */
    bq_set(&s[ns++], 0, 3550, 0.5176, fs);
    bq_set(&s[ns++], 0, 3550, 0.7071, fs);
    bq_set(&s[ns++], 0, 3550, 1.9319, fs);
    bq_set(&s[ns++], 1, 140, 0.7071, fs);
    if (cfg->shape >= 1) bq_set(&s[ns++], 1, 260, 0.6, fs);         /* transformers */
    if (cfg->shape >= 2) bq_set(&s[ns++], 0, 2900, 1.6, fs);        /* a bump and early roll-off */
    /* the line: a first order roll-off mixed with the direct path */
    alpha = 1 - exp(-2 * M_PI * (cfg->shape ? 1500.0 : 3000.0) / fs);
    mix = cfg->shape ? 0.7 : 0.3;
    for (i = 0; i <= n; i++) {
        double x = i == 0 ? (double)V92CH_OS : 0.0;
        lp += alpha * (x - lp);
        x = (1 - mix) * x + mix * lp;
        for (k = 0; k < ns; k++) x = bq_run(&s[k], x);
        c->pulse[i] = x;
    }
    for (i = 0; i < V92CH_LEN; i++) e += c->pulse[i * V92CH_OS] * c->pulse[i * V92CH_OS];
    for (i = 0; i <= n; i++) c->pulse[i] *= cfg->level / sqrt(e);
    c->dn_gain = pow(10.0, -cfg->dn_pad_db / 20.0);
    c->up_gain = pow(10.0, -cfg->up_pad_db / 20.0);
}

double v92chan_pulse(const v92chan_t *c, double t)
{
    double x = t * V92CH_OS;
    int i = (int)floor(x);
    if (x < 0 || i >= V92CH_LEN * V92CH_OS) return 0;
    return c->pulse[i] + (x - i) * (c->pulse[i + 1] - c->pulse[i]);
}

/* the hybrid: a short dispersive leak */
static const double echo_taps[] = { 0.55, -0.9, 0.6, -0.35, 0.2, -0.12, 0.07, -0.04, 0.02, -0.01 };

int16_t v92chan_tick(v92chan_t *c, uint8_t ds_octet, int valid, double amp, double frac)
{
    const v92chan_cfg *cfg = &c->cfg;
    double y, echo = 0;
    int i, alaw = cfg->alaw;
    uint8_t o;

    if (valid && amp != 0.0)
        for (i = 0; i < V92CH_LEN; i++) c->acc[(c->pos + i) & 127] += amp * v92chan_pulse(c, i - frac);
    y = c->acc[c->pos]; c->acc[c->pos] = 0; c->pos = (c->pos + 1) & 127;
    c->dl[c->dpos] = y; y = c->dl[(c->dpos - cfg->up_delay) & 511]; c->dpos = (c->dpos + 1) & 511;

    c->el[c->epos] = c->dn_gain * (double)v90_octet_linear(alaw, ds_octet);
    for (i = 0; i < (int)(sizeof echo_taps / sizeof echo_taps[0]); i++)
        echo += echo_taps[i] * c->el[(c->epos - cfg->echo_delay - i) & 1023];
    c->etail += (c->el[(c->epos - cfg->echo_delay) & 1023] - c->etail) / 200.0;
    echo += cfg->echo_tail / (cfg->echo_gain != 0.0 ? cfg->echo_gain : 1.0) * c->etail;
    c->epos = (c->epos + 1) & 1023;

    y += cfg->echo_gain * echo + cfg->noise * rng_gauss(&c->rng);
    if (cfg->wander != 0.0 && c->wn >= cfg->wander_from) y += cfg->wander * sin(6.283185307179586 * cfg->wander_hz * (double)c->wn / 8000.0);
    c->wn++;
    if (y > 32767) y = 32767;
    if (y < -32767) y = -32767;
    if (cfg->rx_linear) return (int16_t)floor(y + 0.5);
    o = pcm_encode(alaw, (int16_t)floor(y + 0.5));
    if (cfg->up_pad_db != 0.0)
        o = pcm_encode(alaw, (int16_t)floor(c->up_gain * (double)v90_octet_linear(alaw, o) + 0.5));
    return (int16_t)v90_octet_linear(alaw, o);
}
