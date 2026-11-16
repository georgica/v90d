/* Test channel for V.92 PCM upstream: what lies between the analogue modem's
 * symbols and the linear samples the digital modem gets from the network.
 *
 *   analogue modem symbol (an impulse at its own instant, which it may move
 *   by a fraction of a symbol) -> transmit filter, line, codec filters (one
 *   tabulated pulse, 32 points per sample) -> + echo of the downstream signal
 *   through the hybrid -> + noise -> G.711 encoder (mu-law or A-law) sampling
 *   on the network clock -> optional digital loss pad (decode, scale,
 *   encode again) -> linear sample.
 *
 * The analogue modem's symbol clock is the network clock (6.2/V.92): there is
 * no frequency offset, only the phase the modem chooses. The downstream
 * direction is not modelled here (the simulated analogue modem reads the
 * digital modem's octets as sent). Host only (double precision). */
#ifndef V90D_SIM_V92CHAN_H
#define V90D_SIM_V92CHAN_H
#include <stdint.h>
#include "noise.h"

#define V92CH_OS    32
#define V92CH_LEN   96          /* pulse length in samples */

typedef struct {
    int    alaw;
    int    rx_linear;           /* the network codec's A/D is 16-bit linear: no G.711 encoder
                                   (and no upstream pad) between the line and the digital modem */
    int    shape;               /* 0: short line; 1: long line (more slope, more
                                   low frequency loss); 2: as 1 with a notch-like dip */
    double level;               /* RMS at the codec of a signal of unit symbols, linear */
    double noise;               /* RMS of the line noise at the codec, linear */
    double echo_gain;           /* hybrid: echo over downstream amplitude */
    int    echo_delay;          /* samples before the echo begins */
    double echo_tail;           /* a slow tail of the echo: the downstream through a
                                   first order low-pass at 6.4 Hz (25 ms), times this (an FXS
                                   port's coupling; seen with a real modem, reading 53) */
    double dn_pad_db;           /* digital pad in the downstream path (lowers the echo) */
    double up_pad_db;           /* digital pad in the upstream path, 0 = none */
    int    up_delay;            /* extra upstream delay, samples (< 512) */
    uint32_t seed;
} v92chan_cfg;

typedef struct {
    v92chan_cfg cfg;
    double pulse[V92CH_LEN * V92CH_OS + 1];
    double acc[128]; int pos;
    double dl[512]; int dpos;           /* upstream delay */
    double el[1024]; int epos;          /* downstream history for the echo */
    double dn_gain, up_gain;
    double etail;                       /* state of the slow echo tail */
    rng_t  rng;
} v92chan_t;

void    v92chan_init(v92chan_t *c, const v92chan_cfg *cfg);
/* One network sample. ds_octet: what the digital modem sends. valid != 0: the
 * analogue modem emits a symbol of amplitude amp (1 = LU) frac (0 <= frac <
 * 1) of a sample after this instant. Returns what the digital modem
 * receives. */
int16_t v92chan_tick(v92chan_t *c, uint8_t ds_octet, int valid, double amp, double frac);
/* The pulse at time t (samples), for tests. */
double  v92chan_pulse(const v92chan_t *c, double t);

#endif
