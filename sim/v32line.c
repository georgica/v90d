/* 2-wire line for echo-cancelling duplex modems. See v32line.h. */
#include <math.h>
#include <string.h>
#include "v32line.h"
#include "g711.h"

#define PI 3.14159265358979323846
#define FULL_SCALE 32124.0

/* a hybrid's echo path: short and dispersive, unit energy */
static const double hyb[8] = { 0.10, 0.62, -0.66, 0.31, -0.20, 0.12, -0.07, 0.03 };

double v32line_rms(double dbm0) { return FULL_SCALE / sqrt(2.0) * pow(10.0, (dbm0 - 3.17) / 20.0); }

static double lin(double db) { return db >= 200 ? 0.0 : pow(10.0, -db / 20.0); }

void v32line_init(v32line_t *l, const v32line_cfg *cfg)
{
    int i, peak = 0;
    memset(l, 0, sizeof *l);
    l->cfg = *cfg;
    l->flat[0] = 1.0;
    if (!l->cfg.taps || l->cfg.ntaps <= 0) { l->cfg.taps = l->flat; l->cfg.ntaps = 1; }
    for (i = 1; i < l->cfg.ntaps; i++) if (fabs(l->cfg.taps[i]) > fabs(l->cfg.taps[peak])) peak = i;
    rng_seed(&l->rng, cfg->seed);
    l->gain = lin(cfg->loss_db);
    l->g_near = lin(cfg->near_db); l->g_far = lin(cfg->far_db);
    l->g_cnear = lin(cfg->c_near_db); l->g_cfar = lin(cfg->c_far_db);
    if (cfg->mulaw) {
        l->chcfg.ds_pad_db = 0;
        l->chcfg.ds_noise_dbfs = -300;
        l->chcfg.ds_taps = l->cfg.taps; l->chcfg.ds_ntaps = l->cfg.ntaps;
        l->chcfg.us_noise_dbfs = cfg->noise_a_dbm0 - 3.17;
        l->chcfg.us_taps = l->cfg.taps; l->chcfg.us_ntaps = l->cfg.ntaps;
        l->chcfg.echo_gain = l->g_near; l->chcfg.echo_delay = 2;
        l->chcfg.full_scale = FULL_SCALE;
        l->chcfg.seed = cfg->seed + 1;
        channel_init(&l->ch, &l->chcfg);
    } else {
        fir_init(&l->fa, l->cfg.taps, l->cfg.ntaps, l->za);
        fir_init(&l->fc, l->cfg.taps, l->cfg.ntaps, l->zc);
    }
    /* Hilbert transformer for the frequency shift */
    for (i = 0; i < V32LINE_HILB; i++) {
        int k = i - V32LINE_HILB / 2;
        l->hil[i] = (k & 1) ? 2.0 / (PI * k) * (0.54 + 0.46 * cos(2 * PI * k / (V32LINE_HILB - 1))) : 0.0;
    }
    sresamp_init(&l->r_ac, ppm_ratio(cfg->ppm));
    sresamp_init(&l->r_ca, 1.0 / ppm_ratio(cfg->ppm));
    l->fifo_n = 0; l->fifo_run = 0;
    l->oneway = cfg->delay + peak + (cfg->freq_offset != 0 ? V32LINE_HILB / 2 : 0) + SRESAMP_DELAY;
    l->oneway_back = l->oneway + 4;         /* the hand-over buffer, and the answerer reads it a tick later */
    l->peer_lag = SRESAMP_DELAY;
}

/* shift the spectrum of the stream in h[] (ring at hpos) by the line's offset */
static double shift(const v32line_t *l, const double *h)
{
    double im = 0, re = h[(l->hpos - V32LINE_HILB / 2 + V32LINE_HILB) % V32LINE_HILB];
    int i;
    for (i = 0; i < V32LINE_HILB; i++) im += l->hil[i] * h[(l->hpos - i + V32LINE_HILB) % V32LINE_HILB];
    return re * cos(l->ph) - im * sin(l->ph);
}

int16_t v32line_tick(v32line_t *l, int16_t a_tx, v32line_peer_fn peer, void *user)
{
    const v32line_cfg *c = &l->cfg;
    double fwd, back, at_c, at_a, out[4], rx, near = 0;
    int i, n, m = V32LINE_MAXDELAY - 1;
    uint8_t code = 0xFF;
    long q;

    /* what came back from the caller's end one line delay ago */
    back = l->d_ca[(l->dpos - c->delay) & m];

    /* ---- network side ---- */
    if (c->mulaw) {
        double crx;
        channel_tick(&l->ch, linear_to_ulaw(a_tx), back * l->gain, &crx, &code);
        fwd = crx;
        (void)near;
    } else {
        l->a_pos = (l->a_pos + 1) & 63; l->a_hist[l->a_pos] = a_tx;
        for (i = 0; i < 8; i++) near += hyb[i] * l->a_hist[(l->a_pos - 2 - i) & 63];
        fwd = fir_push(&l->fa, (double)a_tx);
    }

    /* ---- forward: delay, loss, frequency shift, to the caller's clock ---- */
    l->dpos = (l->dpos + 1) & m;
    l->d_ac[l->dpos] = fwd + l->g_cfar * l->last_a_arrive;       /* the answerer's hybrid reflects the caller */
    at_c = l->d_ac[(l->dpos - c->delay) & m] * l->gain;
    l->hpos = (l->hpos + 1) % V32LINE_HILB;
    l->ph += 2 * PI * c->freq_offset / 8000.0;
    if (l->ph > PI) l->ph -= 2 * PI;
    if (c->freq_offset != 0) { l->h_ac[l->hpos] = at_c; at_c = shift(l, l->h_ac); }
    l->last_c_arrive = at_c;
    n = sresamp_push(&l->r_ac, at_c, out, 4);
    for (i = 0; i < n; i++) {
        double cn = 0, ctx, o2[4];
        int k, n2;
        for (k = 0; k < 8; k++) cn += hyb[k] * l->c_hist[(l->c_pos - 1 - k) & 63];
        rx = out[i] + l->g_cnear * cn;
        if (c->noise_c_dbm0 > -200) awgn_add(&l->rng, &rx, 1, c->noise_c_dbm0 - 3.17, FULL_SCALE);
        ctx = peer(user, rx);
        l->c_pos = (l->c_pos + 1) & 63; l->c_hist[l->c_pos] = ctx;
        /* onto the line: the caller's signal plus its hybrid's reflection of ours */
        n2 = sresamp_push(&l->r_ca, ctx + l->g_far * out[i], o2, 4);
        for (k = 0; k < n2 && l->fifo_n < 16; k++) l->fifo[l->fifo_n++] = o2[k];
    }
    /* ---- back: one sample from the caller's side per network sample ---- */
    /* (the two clocks beat against each other: the buffer runs three deep
     * before the first sample is taken, so that it never runs dry and the
     * path's delay never moves) */
    if (l->fifo_n >= 3) l->fifo_run = 1;
    if (l->fifo_run && l->fifo_n > 0) {
        at_a = l->fifo[0];
        for (i = 1; i < l->fifo_n; i++) l->fifo[i - 1] = l->fifo[i];
        l->fifo_n--;
    } else at_a = 0;
    if (c->freq_offset != 0) { l->h_ca[l->hpos] = at_a; at_a = shift(l, l->h_ca); }
    l->d_ca[l->dpos] = at_a;

    /* ---- network side, receive ---- */
    if (c->mulaw) {
        l->last_a_arrive = back * l->gain;
        return (int16_t)ulaw_to_linear(code);
    }
    at_a = fir_push(&l->fc, back * l->gain);
    l->last_a_arrive = at_a;
    at_a += l->g_near * near;
    if (c->noise_a_dbm0 > -200) awgn_add(&l->rng, &at_a, 1, c->noise_a_dbm0 - 3.17, FULL_SCALE);
    q = lrint(at_a);
    return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q);
}
