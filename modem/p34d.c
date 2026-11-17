#ifdef V42_XID_TRACE
#include <stdio.h>
#endif
/* V.90 Phases 3 and 4, digital modem. See p34d.h. */
#include <string.h>
#include "p34d.h"
#include "probe.h"
#include "sintab.h"
#include "../v90/v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)

/* our choices for the lengths the Recommendation leaves open */
#define TRN_KNOWN      512      /* symbols of TRN used as a known reference (9.3.1.2) */
#define TRN1D_SYMS     4800     /* TRN1d: >= 2040 T; 600 ms gives the echo canceller time */
#define EC_GUARD_MS    20       /* after TRN1d began + round trip: the caller has stopped Ja */
#define EC_FAST        3000     /* samples at full NLMS step */
#define RI_MIN_UNITS   32       /* Ri >= 192 T */
/* TRN2d before MP. 9.4.1 asks for at least 2040 T (340 data frames) and 400
 * frames, 0.3 s, were enough for two modems. A U.S. Robotics 56K of 1998
 * (flash 5.1.4) was still training on it as a known sequence then: it went
 * on repeating CPt through our MP and asked for a retrain 0.7 s later, each
 * time; with 1200 frames, 0.9 s, it answers with CP. */
#define TRN2D_FRAMES   1200

/* time limits */
#define T_FIRST_S_MS   6000     /* INFO1a to the first S: 70 ms nominal */
#define T_JA_MS        6000     /* S-bar to a valid DIL descriptor */
#define T_S_AFTER_JD   5100     /* 9.3.1.5, from the start of TRN1d, plus a round trip */
#define T_DIL_MS       6000     /* 9.3.2.10: 5000 ms from the first S-bar */
#define T_PHASE4_MS    15000    /* 9.4.1: B1 within 15 s (+ 5 round trips) */
#define CD_LOSS_SYMS   1600     /* about half a second without signal in data mode */

/* rate renegotiation (9.6) */
#define RD_UNITS       64       /* Rd for 384 T (units completed when Rd-bar is asked for) */
#define RENEG_TRN2D    48       /* data frames of TRN2d: optional, <= 2000 ms; 36 ms here */
#define T_RENEG_S_MS   300      /* S heard, but no S-bar: send Rd all the same */
#define T_RENEG_MS     6000     /* 9.6.1: E within 5000 ms + 2 round trips of Rd-bar */
#define CLEAR_MPS      4        /* MP sequences with drn = 0 before we go silent */

/* is the line still good enough for the upstream rate? (rx.mse in data mode
 * is the error power over the mean signal power, Q16) */
#define Q_SNR0_DB10    90       /* the rule of TRN2d: 9 dB + 3 dB per bit per symbol ... */
#define Q_SLACK_DB10   45       /* ... is missed by this much. The rule keeps about 3 dB in
                                   hand, and in data mode the error is some dB above what
                                   TRN2d promised (our own louder signal's echo through
                                   the codec): a rate that works must not be given up. */
#define Q_MARGIN_DB10  30       /* wrong decisions hide noise: allow for it in the new rate */
#define Q_BAD_SYMS     3200     /* for about a second */
#define Q_DEAD_MSE     (65536 / 8)  /* -9 dB: nothing is being received */

#ifdef P34D_TRACE
void p34d_trace(const p34d_t *p, int16_t rxs, int16_t x);
void p34d_trace_bit(const p34d_t *p, int bit);
#endif

enum { RX_IDLE, RX_KNOWN, RX_SEQ, RX_HOLD, RX_SRES, RX_DATA };

static void fail(p34d_t *p, int err)
{
    p->state = P34_FAILED; p->error = err; p->t = 0;
    p->tx_ready = p->rx_ready = 0;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
}

static void enter(p34d_t *p, p34d_state s)
{
#ifdef V42_XID_TRACE
    if (p->in_reneg) fprintf(stderr, "P34 state %d left after %d samples for %d\n", (int)p->state, (int)p->t, (int)s);
#endif
    p->state = s; p->t = 0;
}

int p34d_init(p34d_t *p, const p34d_cfg *cfg)
{
    int r, k;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->state = P34_WAIT_S;
    r = v90tx_init(&p->tx, cfg->alaw, cfg->uinfo);
    ec_init(&p->ec); p->ec_mu = -1;
    sdet_init(&p->sdet, cfg->sym_rate, cfg->car_num, cfg->car_den);
    qamrx_init(&p->rx, cfg->sym_rate, cfg->car_num, cfg->car_den);
    v34trn_init(&p->ref, V34T_GPA);
    v34_tdem_init(&p->tdem, V34_GPA, 4);
    p->rxmode = RX_IDLE;
    p->b1_left = -1;

    p->jd.rates = cfg->down_rates ? cfg->down_rates : ((1u << V90_NUM_RATES) - 1);
    p->jd.cp16_train = 0; p->jd.cp16_reneg = 0; p->jd.max_ld = 3;

    p->mp.type = 0; p->mp.trellis = 0; p->mp.nonlinear = 0; p->mp.shaping = 0; p->mp.ack = 0;
    p->mp.rates = 0;
    for (k = 0; k < 13; k++) {          /* bit k = 4800 + 2400 k bit/s */
        v34_map_t m;
        unsigned rate = 4800u + 2400u * (unsigned)k;
        if (cfg->max_up_rate && rate > (unsigned)cfg->max_up_rate) break;
        if (v34_map_params((unsigned)cfg->sym_rate, rate, 0, &m) == 0) p->mp.rates |= (uint16_t)(1u << k);
    }
    if (cfg->echo_delay >= P34_ECDL - 128) p->cfg.echo_delay = P34_ECDL - 129;
    p->echo_delay = p->cfg.echo_delay > 0 ? p->cfg.echo_delay : 0;
    if (r != 0) { fail(p, P34_ERR_UINFO); return -1; }
    if (!p->mp.rates || (cfg->sym_rate != 3000 && cfg->sym_rate != 3200)) { fail(p, P34_ERR_RATE); return -1; }
    return 0;
}

/* ---- sequences: frame sync hunt, parse ---- */

static void seq_reset(p34d_t *p, int want_cp)
{
    p->fn = 0; p->ones = 0; p->want_cp = want_cp;
}

/* highest upstream rate both sides enable, not above drn * 2400; 0 if none */
static int pick_up_rate(const p34d_t *p, int drn)
{
    unsigned common = (unsigned)p->mp.rates & (unsigned)p->peer_up_rates;
    int k;
    for (k = 12; k >= 0; k--)
        if ((common & (1u << k)) && 4800 + 2400 * k <= drn * 2400) return 4800 + 2400 * k;
    return 0;
}

static void on_cp(p34d_t *p)
{
    const v90_cp_t *cp = &p->cp;
    int r;
    if (cp->drn == 0) { fail(p, P34_ERR_CLEARDOWN); return; }
    if (cp->silence) {                          /* CPs: parameters not to be used (8.5.2) */
        p->got_cps = 1;
        if (cp->ack) p->got_cps_ack = 1;
        return;
    }
    p->peer_up_rates = cp->rates;
    if (!cp->is_cp) {
        if (p->got_cpt) return;
        r = v90tx_load_cp(&p->tx, cp);
        if (r != 0) { fail(p, P34_ERR_CPT); return; }
        p->got_cpt = 1;
        /* what the equaliser made of the 4-point CPt: mse is |e|^2 / |d|^2 in Q16 */
        p->snr_db10 = probe_db10(65536, p->rx.mse > 0 ? p->rx.mse : 1);
        return;
    }
    if (!p->got_cp) {
        r = v90tx_load_cp(&p->tx, cp);
        if (r != 0) { fail(p, P34_ERR_CP); return; }
        p->got_cp = 1;
        p->down_rate = (cp->drn + 20) * 4000 / 3;
    }
    if (cp->ack) p->got_cp_ack = 1;
}

static void seq_try(p34d_t *p)
{
    int r;
    if (p->fn < 40) return;
    if (!p->want_cp) {
        if (p->state != P34_TRAIN) return;
        r = v90_dil_parse(p->fbuf, p->fn, &p->dil);
        if (r > 0) {
            p->fn = 0;
            if (v90tx_load_dil(&p->tx, &p->dil) != 0 || v90tx_set_jd(&p->tx, &p->jd) != 0) { fail(p, P34_ERR_JA); return; }
            (void)v90tx_select(&p->tx, V90TX_SD);
            /* our own signal now reaches the receiver and the canceller knows
             * nothing yet: hold everything until it does */
            p->rxmode = RX_HOLD;
            enter(p, P34_SD);
        }
    } else {
        r = v90_cp_parse(p->fbuf, p->fn, &p->cp);
        if (r > 0) { p->fn = 0; on_cp(p); }
    }
}

static void upstream_start(p34d_t *p);

static void seq_bit(p34d_t *p, int bit)
{
#ifdef P34D_TRACE
    p34d_trace_bit(p, bit);
#endif
    if (p->fn > 0 && p->fn < P34_FBUF) p->fbuf[p->fn++] = (uint8_t)bit;
    if (bit) {
        p->ones++;
        /* E: 20 ones. Inside a sequence a start bit breaks every run at 17;
         * SCR (all ones) only comes before the first CP (9.4.2.2). */
        if (p->want_cp && p->got_cp && p->ones == 20 && !p->got_e) {
            p->got_e = 1;
            upstream_start(p);
        }
        return;
    }
    if (p->ones >= 17) {
        /* frame sync and its start bit. What was collected before may be a
         * complete sequence that was not tried yet. */
        if (p->fn > 18) { p->fn -= 18; seq_try(p); if (p->state == P34_FAILED || p->rxmode != RX_SEQ) return; }
        memset(p->fbuf, 1, 17); p->fbuf[17] = 0; p->fn = 18;
    } else if (p->fn > 0 && (p->fn & 15) == 0) {
        seq_try(p);
    }
    p->ones = 0;
    if (p->fn >= P34_FBUF) p->fn = 0;
}

/* ---- upstream data mode ---- */

static void put_data(void *user, int bit)
{
    p34d_t *p = (p34d_t *)user;
    if (p->rx_ready && p->cfg.putbit) p->cfg.putbit(p->cfg.user, bit);
}

/* E has just ended: B1 starts with the next symbol */
static void upstream_start(p34d_t *p)
{
    v34_cfg_t c;
    int rate = pick_up_rate(p, p->mp.max_rate);
    memset(&c, 0, sizeof c);
    if (!rate) { fail(p, P34_ERR_RATE); return; }
    c.symbol_rate = (uint16_t)p->cfg.sym_rate; c.data_rate = (uint16_t)rate;
    c.trellis = V34_TRELLIS_16; c.expanded = 0; c.nonlinear = 0; c.scrambler = V34_GPA;
    if (v34_params_init(&p->vpar, &c) != 0) { fail(p, P34_ERR_RATE); return; }
    v34_rx_init(&p->vrx, &p->vpar, put_data, p);
    p->vgain = (int32_t)v34_gain_q14(p->vpar.energy_tx_q8);
    p->up_rate = rate;
    {
        /* the error this rate tolerates: SNR = 9 dB + 3 dB per bit per symbol,
         * less the slack; 3 dB is a factor of two */
        int32_t d = Q_SNR0_DB10 + 30 * rate / p->cfg.sym_rate - Q_SLACK_DB10;
        p->mse_limit = 65536 >> (d / 30);
        p->mse_limit -= p->mse_limit * (d % 30) / 60;
        p->q_bad = p->q_dead = 0;
    }
    p->rxmode = RX_DATA;
    p->b1_left = 0;
    p->quiet = 0;
    sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.car_num, p->cfg.car_den);  /* watches for S from here on */
}

/* ---- retrain ---- */

static void retrain(p34d_t *p, int ours)
{
    p->tx_ready = p->rx_ready = 0;
    p->retrain_ours = ours;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
    enter(p, P34_RETRAIN);
}

/* Tone A, 2400 Hz, for more than 50 ms (9.5.1.2). Each 10 ms block: the
 * share of the block's power that sits at 2400 Hz. The caller's data signal
 * leaves a few per cent there; Tone A with its guard tone about 80 %. */
#define TA_BLOCK   80
#define TA_BLOCKS  6
static int tone_a(p34d_t *p, int16_t x)
{
    int32_t c = sintab[((p->ta_ph >> 22) + SIN_N / 4) & SIN_MASK], s = sintab[(p->ta_ph >> 22) & SIN_MASK];
    p->ta_ph += 0x4CCCCCCDu;                    /* 2400 / 8000 of a turn */
    p->ta_re += (x * c) >> 15; p->ta_im += (x * s) >> 15;
    p->ta_pw += (int32_t)x * x;
    if (++p->ta_n < TA_BLOCK) return 0;
    {
        /* a tone of power P gives |sum|^2 = N^2 P / 2 */
        int64_t m = p->ta_re * p->ta_re + p->ta_im * p->ta_im;
        int on = p->ta_pw > (int64_t)TA_BLOCK * 400 && m * 4 > p->ta_pw * TA_BLOCK;     /* more than half */
        p->ta_blocks = on ? p->ta_blocks + 1 : 0;
        p->ta_n = 0; p->ta_re = p->ta_im = p->ta_pw = 0;
    }
    return p->ta_blocks >= TA_BLOCKS;
}

/* ---- rate renegotiation ---- */

/* Rd begins on the next data frame boundary; Rd-bar and TRN2d follow by themselves */
static void start_rd(p34d_t *p)
{
    p->tx_ready = 0;
    p->in_reneg = 1;
    (void)v90tx_select(&p->tx, V90TX_RD);
    enter(p, P34_RD);
}

/* The receiver's error has been above what the rate needs for a second:
 * renegotiate with a lower ceiling in MP (9.6.1.1), or retrain when there
 * is nothing lower. */
static void rate_down(p34d_t *p)
{
    int32_t snr = probe_db10(65536, p->rx.mse > 0 ? p->rx.mse : 1) - Q_MARGIN_DB10;
    int32_t b10 = (snr - Q_SNR0_DB10) * 10 / 30;
    int drn = (int)(((int64_t)p->cfg.sym_rate * b10) / 10) / 2400, now = p->up_rate / 2400;
#ifdef V42_XID_TRACE
    fprintf(stderr, "P34 rate_down: snr %d, now %d, wanted %d\n", (int)snr, now, drn);
#endif
    if (drn >= now) drn = now - 1;
    if (drn < 2) { retrain(p, 1); return; }
    p->mp.max_rate = (uint8_t)drn;
    p->rate_drops++;
    start_rd(p);
}

/* S in data mode (9.6.1.2.1): stop delivering data, follow S to its S-bar */
static void s_heard(p34d_t *p)
{
#ifdef V42_XID_TRACE
    fprintf(stderr, "P34 S heard in data mode (state %d), mse %d\n", (int)p->state, (int)p->rx.mse);
#endif
    p->rx_ready = 0;
    p->in_reneg = 1;
    sres_resume(&p->sres);
    p->rxmode = RX_SRES;
    if (p->state == P34_DATA) enter(p, P34_RENEG_S);
}

/* ---- echo delay ---- */

/* How late is our echo? TRN1d is white and the caller is silent, so the
 * received samples of a short window, correlated with what we sent at every
 * candidate distance before them, peak where the echo path is strongest.
 * The window holds the change from each sample to the next: a modem's line
 * transformer can return the low frequencies of TRN1d as a tail tens of
 * milliseconds long, larger in sum than the start of the echo, and on plain
 * samples that tail put the peak anywhere (U.S. Robotics 5686: found 37 ms
 * for an echo at 34, the canceller then sat beside the echo). The difference
 * takes a flat tail away and leaves the start standing.
 * A few lags per sample keep the cost of any one sample small. ecpos has
 * moved on since the snapshot; the history still holds what is needed
 * (P34_EC_MAXLAG + P34_EC_WIN + the samples the search takes < P34_ECDL). */
#define EC_SEARCH_STEP 4

static void echo_search(p34d_t *p)
{
    int k, i;
    for (k = 0; k < EC_SEARCH_STEP && p->ecs_lag <= P34_EC_MAXLAG; k++, p->ecs_lag++) {
        int64_t acc = 0;
        /* window sample i was received when ecpos was ecs_pos - P34_EC_WIN + 1 + i;
         * what we sent `lag` samples before it sits lag + 1 behind that write position
         * (the sample of the same tick is stored before the canceller runs) */
        int base = p->ecs_pos - P34_EC_WIN - p->ecs_lag;
        for (i = 0; i < P34_EC_WIN; i++)
            acc += (int32_t)p->ecwin[i] * p->ecdl[(base + i) & (P34_ECDL - 1)];
        if (acc < 0) acc = -acc;
        if (acc > p->ecs_best) { p->ecs_best = acc; p->ecs_best_lag = p->ecs_lag; }
    }
    if (p->ecs_lag > P34_EC_MAXLAG) {
        /* a little room before the peak for the start of the echo path */
        p->echo_delay = p->ecs_best_lag > 8 ? p->ecs_best_lag - 8 : 0;
        ec_init(&p->ec);
    }
}

/* ---- one received symbol ---- */

static void rx_symbol(p34d_t *p)
{
    qamrx_t *r = &p->rx;
    int32_t dr, di;
    int n = r->nsym - 1;

    p->zpw += (((int64_t)r->zr * r->zr + (int64_t)r->zi * r->zi) - p->zpw) / 16;

    switch (p->rxmode) {
    case RX_KNOWN:
        if (n < V34_PP_LEN) v34_pp(n, &dr, &di); else v34trn_next(&p->ref, &dr, &di);
        qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
        if (n >= V34_PP_LEN + TRN_KNOWN - 1) { p->rxmode = RX_SEQ; seq_reset(p, 0); }
        break;

    case RX_SEQ: {
        /* 4-point decisions; TRN, Ja, SCR, CPt, CP and E all look like this */
        v34_pt_t pt; unsigned bits;
        dr = r->zr < 0 ? -QRX_REF : QRX_REF; di = r->zi < 0 ? -QRX_REF : QRX_REF;
        qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING, 12);
        pt.re = (int16_t)(dr < 0 ? -V34_ONE : V34_ONE); pt.im = (int16_t)(di < 0 ? -V34_ONE : V34_ONE);
        bits = v34_tdem_put(&p->tdem, pt);
        seq_bit(p, (int)(bits & 1));
        if (p->rxmode == RX_SEQ) seq_bit(p, (int)((bits >> 1) & 1));
        break;
    }

    case RX_SRES:
        if (sres_symbol(&p->sres, r)) {
            if (p->in_reneg) {
                /* CP follows. 8.5.2 resets the caller's scrambler and
                 * differential encoder before the first CPt only, so here they
                 * are in a state we do not know: the descrambler finds its way
                 * within 23 bits, and CP is repeated */
                seq_reset(p, 1);
                p->got_cp = p->got_cp_ack = p->got_e = 0;
                p->got_cps = p->got_cps_ack = 0;
                p->rxmode = RX_SEQ;
                break;
            }
            p->s_events++;
            if (p->dil.n == 0 || p->s_events >= 2) {
                /* Phase 3 is over; CPt follows with scrambler and differential
                 * encoder started from zero (8.5.2) */
                v34_tdem_init(&p->tdem, V34_GPA, 4);
                seq_reset(p, 1);
                p->rxmode = RX_SEQ;
            } else {
                sres_init(&p->sres);        /* SCR or silence, then the S that ends the DIL */
            }
        }
        break;

    case RX_DATA: {
        v34_pt_t in, tent;
        int64_t g = p->vgain;
        int32_t vr = (int32_t)(((int64_t)r->zr * 512) / g), vi = (int32_t)(((int64_t)r->zi * 512) / g);
        if (vr > 32767) vr = 32767;
        if (vr < -32768) vr = -32768;
        if (vi > 32767) vi = 32767;
        if (vi < -32768) vi = -32768;
        in.re = (int16_t)vr; in.im = (int16_t)vi;
        v34_rx_put(&p->vrx, in, &tent);
        dr = (int32_t)(((int64_t)tent.re * g) / 512); di = (int32_t)(((int64_t)tent.im * g) / 512);
        qamrx_update(r, dr, di, QRX_EQ | QRX_CARRIER | QRX_TIMING | QRX_DENSE, 13);
        if (!p->rx_ready && p->vrx.b1_bits >= p->vpar.map.N) {
            if (p->vrx.b1_errors * 8 > p->vrx.b1_bits) { fail(p, P34_ERR_B1); return; }
            p->rx_ready = 1;
        }
        if (p->rx_ready && !p->in_reneg) {
            if (r->mse > Q_DEAD_MSE) { if (++p->q_dead > Q_BAD_SYMS) { retrain(p, 1); return; } }
            else p->q_dead = 0;
            if (r->mse > p->mse_limit) { if (++p->q_bad > Q_BAD_SYMS) { p->q_bad = 0; rate_down(p); return; } }
            else p->q_bad = 0;
        }
        /* loss of the upstream signal */
        if (p->zpw < (int64_t)QRX_REF * QRX_REF / 8) {
            if (++p->quiet > CD_LOSS_SYMS) fail(p, P34_ERR_CARRIER);
        } else {
            p->quiet = 0;
        }
        break;
    }

    default:    /* RX_IDLE, RX_HOLD: keep the grid running, change nothing */
        qamrx_update(r, r->zr, r->zi, 0, 12);
        break;
    }
}

/* ---- the procedure ---- */

uint8_t p34d_tick(p34d_t *p, int16_t rxs)
{
    uint8_t octet;
    int16_t x;
    v90tx_sig_t sig;

    p->now++; p->t++;
    octet = v90tx_tick(&p->tx);
    /* the echo of this octet begins cfg.echo_delay samples from now: the
     * canceller is given what we sent that long ago */
    p->ecdl[p->ecpos] = (int16_t)v90_octet_linear(p->cfg.alaw, octet);
    x = ec_tick(&p->ec, p->ecdl[(p->ecpos - p->echo_delay) & (P34_ECDL - 1)], rxs, p->ec_mu);
    p->ecpos = (p->ecpos + 1) & (P34_ECDL - 1);
#ifdef P34D_TRACE
    p34d_trace(p, rxs, x);      /* live/replay.c: what the canceller left */
#endif
    sig = v90tx_signal(&p->tx);

    /* receiver */
    if (p->state == P34_WAIT_S) {
        (void)qamrx_tick(&p->rx, x);            /* input level for the AGC */
        if (sdet_tick(&p->sdet, x)) {
            if (p->cfg.md_len > 0 && p->s_events == 0) {
                p->s_events = 1;                /* (reused as "MD comes next" here) */
                enter(p, P34_MD);
            } else {
                /* PP(0) is centred 16.5 symbols after the S / S-bar boundary,
                 * and the event came SDET_DELAY samples after it */
                int32_t d = (int32_t)(((int64_t)132000 * 256) / p->cfg.sym_rate) - (SDET_DELAY(&p->sdet) + 1) * 256;
                qamrx_sync(&p->rx, d);
                p->s_events = 0;
                p->rxmode = RX_KNOWN;
                enter(p, P34_TRAIN);
            }
        } else if (p->t > MS(T_FIRST_S_MS) + MS(p->cfg.md_len * 35)) {
            fail(p, P34_ERR_NO_S);
        }
        return octet;
    }
    if (p->state == P34_MD) {
        (void)qamrx_tick(&p->rx, x);
        if (p->t >= MS(p->cfg.md_len * 35)) {
            sdet_init(&p->sdet, p->cfg.sym_rate, p->cfg.car_num, p->cfg.car_den);
            enter(p, P34_WAIT_S);
        }
        return octet;
    }
    if (p->state == P34_FAILED || p->state == P34_RETRAIN) return octet;

    if (qamrx_tick(&p->rx, x)) rx_symbol(p);
    if (p->state == P34_FAILED) return octet;
    if (p->rxmode == RX_DATA) {
        (void)sdet_tick(&p->sdet, x);
        if (p->sdet.present >= SDET_PRESENT) s_heard(p);
    }
    /* Tone A asks for a retrain in data mode and in Phase 4 alike: a caller
     * may give a start-up up there (a U.S. Robotics 56K did, 0.7 s into our
     * MP, and went unheard until T_PHASE4_MS). Not in Phase 3, where the
     * caller's own signals are not all known to stay clear of 2400 Hz. */
    if (p->state >= P34_RI && p->state <= P34_RT) {
        if (tone_a(p, x)) { retrain(p, 0); return octet; }
    } else if (p->in_reneg && tone_a(p, x)) {
        retrain(p, 0); return octet;
    }

    /* transmitter sequencing and time limits */
    switch (p->state) {
    case P34_TRAIN:
        if (p->t > MS(T_JA_MS)) fail(p, P34_ERR_JA);
        break;

    case P34_SD:
        if (sig == V90TX_TRN1D) {
            uint32_t n = v90tx_symbols(&p->tx);
            int32_t from = p->cfg.rtd + MS(EC_GUARD_MS);
            int32_t len = TRN1D_SYMS;
            if (n == 1) p->t_trn1d = p->now;
            if (p->cfg.echo_delay < 0) {
                /* first find out how late the echo is (see echo_search) */
                int32_t snap = from + P34_EC_MAXLAG + P34_EC_WIN;
                if ((int32_t)n == snap - P34_EC_WIN) p->ecw_last = rxs;
                else if ((int32_t)n > snap - P34_EC_WIN && (int32_t)n <= snap) {
                    /* the change from sample to sample, not the sample: see echo_search */
                    int32_t dv = (int32_t)rxs - p->ecw_last;
                    p->ecw_last = rxs;
                    p->ecwin[p->ecw_n++] = (int16_t)(dv > 32767 ? 32767 : dv < -32768 ? -32768 : dv);
                    if ((int32_t)n == snap) { p->ecs_pos = p->ecpos; p->ecs_lag = 0; p->ecs_best = -1; }
                } else if ((int32_t)n > snap && p->ecs_lag <= P34_EC_MAXLAG) {
                    echo_search(p);
                }
                from = snap + P34_EC_MAXLAG / EC_SEARCH_STEP + 2;
                len += from;
            }
            /* the caller is silent now: learn the echo of TRN1d */
            if ((int32_t)n < from) p->ec_mu = -1;
            else if ((int32_t)n < from + EC_FAST) p->ec_mu = 0;
            else p->ec_mu = 3;
            if ((int32_t)n >= len) {
                p->ec_mu = -1;
                (void)v90tx_select(&p->tx, V90TX_JD);
                sres_init(&p->sres);
                p->s_events = 0;
                p->rxmode = RX_SRES;
                enter(p, P34_JD);
            }
        }
        break;

    case P34_JD:
        if (sres_in_s(&p->sres) || p->s_events) {
            /* 9.3.1.5: complete the current Jd, then Jd'; the DIL (or Ri when
             * none was asked for) follows by itself */
            (void)v90tx_select(&p->tx, V90TX_JD_PRIME);
            enter(p, P34_DIL);
        } else if (p->now - p->t_trn1d > MS(T_S_AFTER_JD) + p->cfg.rtd) {
            fail(p, P34_ERR_NO_S2);
        }
        break;

    case P34_DIL:
        if (p->rxmode == RX_SEQ) {
            /* the S-bar that ends Phase 3 has passed */
            if (p->dil.n > 0) (void)v90tx_select(&p->tx, V90TX_RI);     /* at the end of this DIL-segment */
            enter(p, P34_RI);
        } else if (p->t > MS(T_DIL_MS) + MS(1000)) {
            fail(p, P34_ERR_DIL_END);
        }
        break;

    case P34_RI:
        if (p->got_cpt && sig == V90TX_RI && v90tx_units(&p->tx) >= RI_MIN_UNITS) {
            (void)v90tx_select(&p->tx, V90TX_RI_BAR);                   /* then TRN2d by itself */
            enter(p, P34_TRN2D);
        } else if (p->t > MS(T_PHASE4_MS)) {
            fail(p, P34_ERR_CPT);
        }
        break;

    case P34_TRN2D:
        if (sig == V90TX_TRN2D && v90tx_units(&p->tx) >= TRN2D_FRAMES) {
            /* upstream rate from what the line gave the 4-point signal: each
             * 3 dB above about 9 dB is worth one bit per symbol (16-state
             * trellis code, a margin of 3 dB) */
            int32_t b10;
            /* measured now, at the end of TRN2d, not when CPt arrived: the
             * loops have had 0.3 s more to settle after the re-acquisition */
            p->snr_db10 = probe_db10(65536, p->rx.mse > 0 ? p->rx.mse : 1);
            b10 = (p->snr_db10 - 90) * 10 / 30;                         /* bits per symbol x 10 */
            int32_t rate = (int32_t)(((int64_t)p->cfg.sym_rate * b10) / 10);
            int drn = rate / 2400;
            if (drn > 14) drn = 14;
            if (drn < 2) drn = 2;
            if (p->cfg.max_up_rate && drn * 2400 > p->cfg.max_up_rate) drn = p->cfg.max_up_rate / 2400;
            p->mp.max_rate = (uint8_t)drn;
            if (v90tx_set_mp(&p->tx, &p->mp) != 0 || v90tx_select(&p->tx, V90TX_MP) != 0) { fail(p, P34_ERR_RATE); break; }
            enter(p, P34_MP);
        }
        break;

    case P34_MP:
        if (p->clearing) {
            /* 9.7: the caller has been told; nothing more to wait for */
            if (sig == V90TX_MP && v90tx_units(&p->tx) >= CLEAR_MPS) fail(p, P34_ERR_HANGUP);
        } else if (sig == V90TX_MP && v90tx_units(&p->tx) >= 1 && (p->got_cp || p->got_cps)) {
            (void)v90tx_select(&p->tx, V90TX_MP_ACK);
            enter(p, P34_MP_ACK);
        } else if (p->in_reneg && p->t > MS(T_RENEG_MS)) {
            retrain(p, 1);                                              /* 9.6.1 */
        } else if (p->t > MS(T_PHASE4_MS)) {
            fail(p, P34_ERR_CP);
        }
        break;

    case P34_MP_ACK:
        if (sig == V90TX_MP_ACK && v90tx_units(&p->tx) >= 1 && !p->got_cp && p->got_cps_ack) {
            /* 9.6.1.2.5: Ed, then silence in data frame alignment */
            (void)v90tx_select(&p->tx, V90TX_ED);
            enter(p, P34_SIL);
        } else if (sig == V90TX_MP_ACK && v90tx_units(&p->tx) >= 1 && (p->got_cp_ack || p->got_e)) {
            v90tx_set_getbit(&p->tx, p->cfg.getbit, p->cfg.user);
            (void)v90tx_select(&p->tx, V90TX_ED);                       /* then B1d, then data */
            enter(p, P34_ED);
        } else if (p->in_reneg && p->t > MS(T_RENEG_MS)) {
            retrain(p, 1);                                              /* 9.6.1 */
        } else if (p->t > MS(T_PHASE4_MS)) {
            fail(p, P34_ERR_CP);
        }
        break;

    case P34_ED:
        if (sig == V90TX_DATA) p->tx_ready = 1;
        if (p->tx_ready && p->rx_ready) {
#ifdef V42_XID_TRACE
            fprintf(stderr, "P34 data again (reneg %d), state %d lasted %d samples, up %d\n", p->in_reneg, (int)p->state, (int)p->t, (int)p->up_rate);
#endif
            p->in_reneg = 0; enter(p, P34_DATA); }
        else if (p->in_reneg && p->t > MS(T_RENEG_MS)) retrain(p, 1);
        else if (p->t > MS(T_PHASE4_MS)) fail(p, P34_ERR_B1);
        break;

    case P34_SIL:
        if (sig == V90TX_ED && v90tx_pending(&p->tx) == V90TX_NONE) {
            (void)v90tx_select(&p->tx, V90TX_SILENCE);                  /* in place of B1d */
        } else if (sig == V90TX_SILENCE && p->got_cp) {
            /* 9.6.1.2.6: a CP without the request: Rt, Rt-bar, MP */
            (void)v90tx_select(&p->tx, V90TX_RT);
            enter(p, P34_RT);
        } else if (p->t > MS(T_RENEG_MS)) {
            retrain(p, 1);
        }
        break;

    case P34_RT:
        if (sig == V90TX_RT && v90tx_units(&p->tx) >= RD_UNITS && v90tx_pending(&p->tx) == V90TX_NONE) {
            (void)v90tx_select(&p->tx, V90TX_RT_BAR);                   /* then MP by itself */
        } else if (sig == V90TX_MP) {
            enter(p, P34_MP);
        } else if (p->t > MS(T_RENEG_MS)) {
            retrain(p, 1);
        }
        break;

    case P34_RENEG_S:
        /* 9.6.1.2.2: after the S-to-S-bar transition, Rd. If the S-bar is
         * missed the caller is waiting for Rd all the same. */
        if (p->rxmode == RX_SEQ || p->t > MS(T_RENEG_S_MS)) {
#ifdef V42_XID_TRACE
            fprintf(stderr, "P34 reneg: %s after %d samples, Rd\n", p->rxmode == RX_SEQ ? "S-bar found" : "no S-bar", (int)p->t);
#endif
            start_rd(p);
        }
        break;

    case P34_RD:
        if (sig == V90TX_RD && v90tx_units(&p->tx) >= RD_UNITS && v90tx_pending(&p->tx) == V90TX_NONE) {
            (void)v90tx_select(&p->tx, V90TX_RD_BAR);                   /* then TRN2d by itself */
        } else if (sig == V90TX_TRN2D && v90tx_units(&p->tx) >= RENEG_TRN2D) {
            if (p->clearing) p->mp.max_rate = 0;
            if (v90tx_set_mp(&p->tx, &p->mp) != 0 || v90tx_select(&p->tx, V90TX_MP) != 0) { fail(p, P34_ERR_RATE); break; }
            enter(p, P34_MP);
        } else if (p->t > MS(T_RENEG_MS)) {
            retrain(p, 1);
        }
        break;

    default:
        break;
    }
    return octet;
}


int p34d_renegotiate(p34d_t *p)
{
    if (p->state != P34_DATA) return -1;
    start_rd(p);
    return 0;
}

int p34d_cleardown(p34d_t *p)
{
    if (p->state != P34_DATA) return -1;
    p->clearing = 1;
    start_rd(p);
    return 0;
}

int p34d_retrain(p34d_t *p)
{
    if (p->state != P34_DATA && !p->in_reneg) return -1;
    retrain(p, 1);
    return 0;
}
