#include <math.h>
#include "channel.h"
#include "g711.h"

void channel_init(channel_t *c, const channel_cfg *cfg)
{
    c->cfg = *cfg;
    c->ds_pad_lin = pow(10.0, -cfg->ds_pad_db / 20.0);
    fir_init(&c->ds_loop, cfg->ds_taps, cfg->ds_ntaps, c->ds_z);
    fir_init(&c->us_loop, cfg->us_taps, cfg->us_ntaps, c->us_z);
    { int i; for (i = 0; i < CH_MAXDELAY; i++) c->echo_ring[i] = 0.0; }
    c->echo_pos = 0; c->us_lin = 0;
    rng_seed(&c->rng, cfg->seed);
}

void channel_tick(channel_t *c, uint8_t ds_code, double us_analog,
                  double *client_rx, uint8_t *server_rx)
{
    double ds_level, ds_padded, ds_filt, cr;
    double delayed, uf, us;
    int d;

    /* ---- downstream: server PCM -> client analog ---- */
    ds_level = (double)ulaw_to_linear(ds_code);
    ds_padded = ds_level * c->ds_pad_lin;
    ds_filt = fir_push(&c->ds_loop, ds_padded);
    cr = ds_filt;
    awgn_add(&c->rng, &cr, 1, c->cfg.ds_noise_dbfs, c->cfg.full_scale);
    if (client_rx) *client_rx = cr;

    /* line signal for the hybrid echo: the padded downstream analog */
    c->echo_ring[c->echo_pos] = ds_padded;
    d = c->cfg.echo_delay % CH_MAXDELAY;
    delayed = c->echo_ring[(c->echo_pos - d + CH_MAXDELAY) % CH_MAXDELAY];
    c->echo_pos = (c->echo_pos + 1) % CH_MAXDELAY;

    /* ---- upstream: client analog -> server PCM ---- */
    uf = fir_push(&c->us_loop, us_analog);
    us = uf + c->cfg.echo_gain * delayed;
    awgn_add(&c->rng, &us, 1, c->cfg.us_noise_dbfs, c->cfg.full_scale);
    {
        long q = lround(us);
        if (q > 32767) q = 32767; else if (q < -32768) q = -32768;
        c->us_lin = (int16_t)q;
        if (server_rx) *server_rx = linear_to_ulaw((int32_t)q);
    }
}
