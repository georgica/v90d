/* Shared by the V.34 engine tests: two v34dx engines (answer and call) over
 * a two-way path with delay, loss, each side's own echo, a far echo, and
 * noise; bit sources and error-counting sinks. Test side only (double). */
#ifndef V90D_TEST_DXLINE_H
#define V90D_TEST_DXLINE_H
#include <string.h>
#include <math.h>
#include "../modem/v34dx.h"
#include "../modem/p2ans.h"
#include "../sim/noise.h"

typedef struct { uint32_t s; } bsrc_t;
static int bsrc_bit(void *u)
{
    bsrc_t *g = (bsrc_t *)u;
    g->s ^= g->s << 13; g->s ^= g->s >> 17; g->s ^= g->s << 5;
    return (int)((g->s >> 7) & 1);
}
typedef struct { bsrc_t ref; long n, errs; } bsink_t;
static void bsink_bit(void *u, int bit)
{
    bsink_t *k = (bsink_t *)u;
    if (bsrc_bit(&k->ref) != bit) k->errs++;
    k->n++;
}
/* one end's user data: what it sends and what it expects. Every time data
 * mode begins anew in a direction (start-up, rate renegotiation) the
 * sequence begins anew too, so what is delivered after it must be exactly
 * what was sent after it. */
typedef struct {
    bsrc_t src; bsink_t sink; const v34dx_t *dx;
    uint32_t own, far; int seen_tx, seen_rx;
} bend_t;
static int bend_get(void *u)
{
    bend_t *e = (bend_t *)u;
    if (e->dx && e->dx->tx_starts != e->seen_tx) { e->seen_tx = e->dx->tx_starts; e->src.s = e->own; }
    return bsrc_bit(&e->src);
}
static void bend_put(void *u, int bit)
{
    bend_t *e = (bend_t *)u;
    if (e->dx && e->dx->rx_starts != e->seen_rx) { e->seen_rx = e->dx->rx_starts; e->sink.ref.s = e->far; }
    bsink_bit(&e->sink, bit);
}
static void bend_init(bend_t *e, const v34dx_t *dx, uint32_t own, uint32_t far)
{
    memset(e, 0, sizeof *e);
    e->dx = dx; e->own = own; e->far = far;
    e->src.s = own; e->sink.ref.s = far;
}

#define DXL_MAXD 4096
typedef struct {
    int d_ca, d_ac;             /* one-way delays, samples */
    double g_ca, g_ac;          /* path gains */
    double echo_a, echo_c;      /* each side's hybrid: own transmit into own receiver */
    int echo_a_delay;           /* extra delay of the answer side's own path (a driver) */
    double far_a;               /* answer side: echo from the far hybrid, a round trip late */
    double noise_a, noise_c;    /* RMS at each receiver */
    int mute_ca, mute_ac;       /* a direction dead */
    double tone_a_hz, tone_c_hz, tone_amp;  /* a tone into the answer / call side's receiver */
} dxline_cfg;
typedef struct {
    dxline_cfg c; rng_t rng;
    double la[DXL_MAXD], lc[DXL_MAXD]; int pos; long n;
} dxline_t;
static inline void dxline_init(dxline_t *l, const dxline_cfg *c)
{
    memset(l, 0, sizeof *l); l->c = *c; rng_seed(&l->rng, 4321);
}
static inline double dxl_at(const double *b, int pos, int d) { return b[(pos - d) & (DXL_MAXD - 1)]; }
static inline int16_t dxl_clip(double v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : floor(v + 0.5)); }
/* give what each end sent in this tick; get what each receives in the next */
static inline void dxline_step(dxline_t *l, int16_t atx, int16_t ctx, int16_t *arx, int16_t *crx)
{
    const dxline_cfg *c = &l->c;
    double a, k;
    l->la[l->pos] = atx; l->lc[l->pos] = ctx;
    a = (c->mute_ca ? 0 : c->g_ca) * dxl_at(l->lc, l->pos, c->d_ca)
      + c->echo_a * (0.6 * dxl_at(l->la, l->pos, c->echo_a_delay + 1) + 0.5 * dxl_at(l->la, l->pos, c->echo_a_delay + 3)
                     - 0.3 * dxl_at(l->la, l->pos, c->echo_a_delay + 7))
      + c->far_a * (0.7 * dxl_at(l->la, l->pos, c->d_ac + c->d_ca + c->echo_a_delay + 2)
                    + 0.4 * dxl_at(l->la, l->pos, c->d_ac + c->d_ca + c->echo_a_delay + 5))
      + c->noise_a * rng_gauss(&l->rng);
    k = (c->mute_ac ? 0 : c->g_ac) * dxl_at(l->la, l->pos, c->d_ac)
      + c->echo_c * (0.6 * dxl_at(l->lc, l->pos, 1) + 0.5 * dxl_at(l->lc, l->pos, 4))
      + c->noise_c * rng_gauss(&l->rng);
    if (c->tone_a_hz > 0) a += c->tone_amp * sin(6.283185307179586 * c->tone_a_hz * l->n / 8000.0);
    if (c->tone_c_hz > 0) k += c->tone_amp * sin(6.283185307179586 * c->tone_c_hz * l->n / 8000.0);
    l->n++;
    l->pos = (l->pos + 1) & (DXL_MAXD - 1);
    *arx = dxl_clip(a); *crx = dxl_clip(k);
}
#endif
