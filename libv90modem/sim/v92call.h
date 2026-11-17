/* Test peer: a whole V.92 analogue modem call with its line, as the digital
 * modem's G.711 interface sees it. One octet from the digital modem in, the
 * octet the network hands it out.
 *
 *   V.8 (sim/v8call, CM with the PCM category) -> Phase 2 (sim/p2a, INFO0a
 *   with or without the V.92 bit) -> by its own INFO1a either
 *     PCM upstream:  sim/v92a through sim/v92chan (V.92 Phases 3 and 4, data),
 *     V.34 upstream: sim/p34a through sim/channel (V.90 Phases 3 and 4, data).
 *   A retrain from either side (V.92 9.7.2): 70 ms of silence, Tone A, Phase 2
 *   again, and the same choice from what that INFO1d allows.
 *
 * So one object plays a V.92 modem with PCM upstream enabled or disabled
 * and a plain V.90 modem. V.8, Phase 2 and V.90 run over sim/channel (the
 * network is mu-law there; an A-law codec is put on it by converting at its
 * ports, as dce/test/harness does); the V.92 phases over sim/v92chan. Both
 * analogue modem models read the digital modem's octets as sent. Host only. */
#ifndef V90D_SIM_V92CALL_H
#define V90D_SIM_V92CALL_H
#include <stdint.h>
#include "v8call.h"
#include "p2a.h"
#include "p34a.h"
#include "v92a.h"
#include "v92chan.h"
#include "channel.h"

typedef struct {
    int alaw;               /* law of the digital modem's codec */
    int rx_linear;          /* that codec's A/D is 16-bit linear (v92call_tick_lin); its D/A
                               still puts the G.711 levels of our octets on the line */
    double us_noise_dbfs;   /* sim/channel: upstream noise; 0 = -60 dBFS */
    double echo_gain_pcm;   /* hybrid of the V.92 line model; 0 = echo_gain / 2 */
    int v92;                /* INFO0a bit 26: a V.92 modem */
    int pcm_up;             /* select PCM upstream when INFO0d and INFO1d allow it */
    int level;              /* RMS of V.8, Phase 2 and the V.34 upstream; 0 = 4000 */
    int lapm;               /* CM carries the protocol category with LAPM */
    /* the line */
    double echo_gain;       /* hybrid, both line models; 0 = none */
    int    echo_delay;      /* samples before the echo begins (a driver's buffers) */
    int    shape;           /* v92chan line shape, 0..2 */
    double up_level;        /* v92chan: RMS of unit symbols at the codec; 0 = 4000 */
    double up_noise;        /* v92chan: line noise RMS; 0 = 3 */
    /* the V.92 modem */
    int dil_n;              /* DIL-segments asked for (0 = SCR instead) */
    uint32_t up_rates;      /* Ja: upstream rates, 0 = all */
    int mute_pcm;           /* fault: the first n PCM upstream start-ups send nothing at all */
    int  (*getbit)(void *user);
    void (*putbit)(void *user, int bit);
    void *user;
} v92call_cfg;

typedef enum {
    VCL_V8, VCL_P2,
    VCL_PCM,                /* V.92 Phases 3 and 4, data: PCM upstream */
    VCL_V90,                /* V.90 Phases 3 and 4, data: V.34 upstream */
    VCL_RTSIL,              /* the 70 ms of silence that begin a retrain */
    VCL_NO_PCM,             /* the JM had no PCM mode: nothing more is done */
    VCL_FAILED
} v92call_stage;

typedef struct {
    v92call_cfg cfg;
    v92call_stage stage;
    v8call_t  v8;
    p2a_t     p2;
    p34a_t    pa;
    v92a_t    va;
    channel_t ch;
    v92chan_t vch;
    double    ctx;              /* our line signal of the last sample */
    uint8_t   dly[32]; int dpos;    /* the octets on their way to the V.92 model */
    dpskrx_t  toneb;            /* Tone B in data mode asks for a retrain */
    int       rt_n;
    int       d_v92;            /* INFO0d bit 27 of the first Phase 2 */
    int       phase2s;          /* Phase 2 runs completed */
    int       pcm_starts, v90_starts;   /* times each Phase 3 was begun */
    int       retrains;         /* retrains begun (either side's) */
    int       retraining;
    int       hung;             /* on hook */
    long      n;
    int16_t   rx_lin;           /* what the network hands the digital modem, linear */
} v92call_t;

void    v92call_init(v92call_t *c, const v92call_cfg *cfg);
uint8_t v92call_tick(v92call_t *c, uint8_t ds_octet);
/* the same at a codec with a linear A/D (cfg.rx_linear): the received sample
 * on the 16-bit scale */
int16_t v92call_tick_lin(v92call_t *c, uint8_t ds_octet);
/* 1 while both directions carry user data */
int     v92call_data(const v92call_t *c);
/* from data mode: start a retrain (silence, then Tone A) */
void    v92call_retrain(v92call_t *c);
/* from data mode: a rate renegotiation (V.92 9.8.2 or V.90 9.6.2), or with
 * cleardown the orderly end (9.11, V.90 9.7). Returns 0, or -1 */
int     v92call_renegotiate(v92call_t *c, int cleardown);
/* go on-hook: silence from now on */
void    v92call_hangup(v92call_t *c);

#endif
