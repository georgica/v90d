/* V.92 Phases 3 and 4 with PCM upstream, digital modem. See v92_p34d.h. */
#include <string.h>
#include "v92_p34d.h"
#include "sintab.h"
#include "v90_pcm.h"

#define MS(ms) ((int32_t)(ms) * 8)

/* our choices for the lengths the Recommendation leaves open */
#define TRN1D_SYMS     4800     /* TRN1d: >= 2040 T; 600 ms gives the echo canceller time */
#define EC_GUARD_MS    20       /* after TRN1d began + round trip: the caller has stopped Ja */
#define EC_FAST        3000     /* samples at full NLMS step */
#define NOISE_SYMS     1000     /* end of TRN1d: noise floor measurement */
#define TRN_LEAD       4        /* the training receiver starts this early */
#define TRAINED_SYMS   2400     /* no DIL: second TRN1u before Ri (9.5.1.1.13) */
#define TRN2D_FRAMES   400      /* TRN2d >= 2040 T = 340 data frames */
#define REFINE_SYMS    2400     /* TRN2u before the design starts */
#define RENEG_TRN2D    48       /* data frames of TRN2d in a rate renegotiation */
#define RENEG_REFINE   600
#define RD_UNITS       64       /* Rd for 384 T */
#define CLEAR_CPDS     4        /* CPd sequences with drn = 0 before we go silent */
#define RU_FLOOR       24       /* linear RMS below which Ru / Su are not looked for */
#define DOWN_RMS       5600     /* downstream data power assumed for the echo: -9 dBm0 */

/* time limits */
#define T_RU_MS        6000     /* INFO1a to Ru: 70 ms nominal */
#define T_JA_MS        6000     /* 9.5.1.2.1: 4500 ms + round trip from the end of INFO1a */
#define T_SU_MS        5100     /* 9.5.1.2.2, from the start of TRN1d, plus a round trip */
#define T_TRAIN_MS     8000     /* 9.5.2.1.11: CPt within 5000 ms of Su-bar */
#define T_PHASE4_MS    20000    /* 9.6.1.2.1: B1u within 20 s (+ 6 round trips) */
#define T_RENEG_MS     8000
#define T_PACE_MS      2500     /* more for each sample the design waits between steps */

#define CD_BLOCK       80       /* carrier watch: 10 ms */
#define CD_BLOCKS      50       /* about half a second without signal in data mode */
#define CD_SHIFT       6        /* "without": 18 dB under the power of B1u */

#define TA_BLOCK       80
#define TA_BLOCKS      6        /* Tone A for more than 50 ms (9.7.1.2) */

static void fail(v92d_t *p, int err)
{
    p->state = V92D_FAILED; p->error = err; p->t = 0;
    p->tx_ready = p->rx_ready = 0; p->rx_on = 0; p->trn_on = 0;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
}

static void enter(v92d_t *p, v92d_state s) { p->state = s; p->t = 0; }

static int32_t pace_ms(const v92d_t *p) { return p->cfg.dsn_pace > 1 ? (int32_t)T_PACE_MS * (p->cfg.dsn_pace - 1) : 0; }

static void retrain(v92d_t *p, int ours)
{
    p->tx_ready = p->rx_ready = 0; p->rx_on = 0; p->trn_on = 0;
    p->retrain_ours = ours;
    (void)v90tx_select(&p->tx, V90TX_SILENCE);
    enter(p, V92D_RETRAIN);
}

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}

int v92d_init(v92d_t *p, const v92d_cfg *cfg)
{
    int r;
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->state = V92D_WAIT_RU;
    r = v90tx_init(&p->tx, cfg->alaw, cfg->uinfo);
    ec_init(&p->ec); p->ec_mu = -1;
    v92_p6_init(&p->p6);
    /* Table 21: Jd as in V.90 with bits 47 and 48 zero */
    p->jd.rates = cfg->down_rates ? cfg->down_rates : ((1u << V90_NUM_RATES) - 1);
    p->jd.cp16_train = 0; p->jd.cp16_reneg = 0; p->jd.max_ld = 3;
    if (r != 0) { fail(p, V92D_ERR_UINFO); return -1; }
    if (cfg->echo_delay > V92D_ECDL - 129) { fail(p, V92D_ERR_UINFO); return -1; }
    p->echo_delay = cfg->echo_delay > 0 ? cfg->echo_delay : 0;
    return 0;
}

/* ---- echo delay ---- */

/* How late is our echo? The method of ../modem/p34d: TRN1d is white and the
 * caller is silent (9.5.2.1.3: it stops Ja on our Sd-bar), so a short window
 * of received samples, correlated with what we sent at every candidate
 * distance before them, peaks where the echo path is strongest. A few lags
 * per sample keep the cost of any one sample small. ecpos has moved on since
 * the snapshot; the history still holds what is needed (V92D_EC_MAXLAG +
 * V92D_EC_WIN + the samples the search takes < V92D_ECDL). */
#define EC_SEARCH_STEP 4

static void echo_search(v92d_t *p)
{
    int k, i;
    for (k = 0; k < EC_SEARCH_STEP && p->ecs_lag <= V92D_EC_MAXLAG; k++, p->ecs_lag++) {
        int64_t acc = 0;
        /* window sample i was received when ecpos was ecs_pos - V92D_EC_WIN + 1 + i;
         * what we sent `lag` samples before it sits lag + 1 behind that write
         * position (the sample of the same tick is stored before the canceller runs) */
        int base = p->ecs_pos - V92D_EC_WIN - p->ecs_lag;
        for (i = 0; i < V92D_EC_WIN; i++)
            acc += (int32_t)p->ecwin[i] * p->ecdl[(base + i) & (V92D_ECDL - 1)];
        if (acc < 0) acc = -acc;
        if (acc > p->ecs_best) { p->ecs_best = acc; p->ecs_best_lag = p->ecs_lag; }
    }
    if (p->ecs_lag > V92D_EC_MAXLAG) {
        /* a little room before the peak for the start of the echo path */
        p->echo_delay = p->ecs_best_lag > 8 ? p->ecs_best_lag - 8 : 0;
        ec_init(&p->ec);
        p->et_g[0] = p->et_g[1] = 0;
    }
}

/* Tone A, 2400 Hz, for more than 50 ms (9.7.1.2), as ../modem/p34d does */
static int tone_a(v92d_t *p, int16_t x)
{
    int32_t c = sintab[((p->ta_ph >> 22) + SIN_N / 4) & SIN_MASK], s = sintab[(p->ta_ph >> 22) & SIN_MASK];
    p->ta_ph += 0x4CCCCCCDu;                    /* 2400 / 8000 of a turn */
    p->ta_re += (x * c) >> 15; p->ta_im += (x * s) >> 15;
    p->ta_pw += (int32_t)x * x;
    if (++p->ta_n < TA_BLOCK) return 0;
    {
        int64_t m = p->ta_re * p->ta_re + p->ta_im * p->ta_im;
        int on = p->ta_pw > (int64_t)TA_BLOCK * 400 && m * 4 > p->ta_pw * TA_BLOCK;     /* more than half */
        p->ta_blocks = on ? p->ta_blocks + 1 : 0;
        p->ta_n = 0; p->ta_re = p->ta_im = p->ta_pw = 0;
    }
    return p->ta_blocks >= TA_BLOCKS;
}

/* ---- data mode receiver ---- */

static void put_data(void *user, int bit)
{
    v92d_t *p = (v92d_t *)user;
    if (p->b1_left > 0) {                       /* B1u: 48 data frames of ones (8.7.1) */
        if (!bit) p->b1_bad++;
        if (--p->b1_left == 0) {
            if (p->b1_bad > 8) fail(p, V92D_ERR_B1);
            else {
                /* the carrier watch's reference: the power B1u came with
                 * (which points the moduli use is not ours to guess) */
                p->sig_ms = p->cd_n ? (uint32_t)(p->cd_acc / p->cd_n) : 0;
                p->cd_acc = 0; p->cd_n = 0; p->cd_quiet = 0;
                p->rx_ready = 1;
            }
        }
    } else if (p->rx_ready && p->cfg.putbit && !p->cd_quiet) {
        /* (nothing is delivered from a line that has gone quiet: what the
         * decoder makes of silence is not data) */
        p->cfg.putbit(p->cfg.user, bit);
    }
}

/* E2u is over: symbol m is the first of B1u */
static void upstream_start(v92d_t *p, int32_t m)
{
    if (p->dsn_state != 2 || v92_dec_init(&p->dec, &p->cpd.map, put_data, p) != 0) { fail(p, V92D_ERR_B1); return; }
    v92_dec_any_start(&p->dec);
    p->m_b1u = m;
    p->data_k = m + p->dsn.dly;
    p->b1_left = (int32_t)V92_B1U_FRAMES * p->cpd.map.K; p->b1_bad = 0;
    p->b1_sym = 0; p->b1_leads = 0; p->b1_zero = 0;
    p->up_rate = V92_UP_BPS(p->cpd.drn);
    p->bad_seen = 0; p->q_bad = 0;
    p->rx_on = 1;
    p->trn_on = 2;
    p->sig_ms = 0; p->cd_acc = 0; p->cd_n = 0; p->cd_quiet = 0;
}

/* The first data frame after E2u, sample by sample. A real analogue modem
 * (reading 57) sends one data frame with every Ki = 0 before the 48 data
 * frames of B1u (its encoder is one data frame late with the scrambled
 * ones), and what follows is as if that frame had not been: n = 0 of 8.7.1
 * for the mapping is the first symbol after it. Of twelve symbols of
 * scrambled ones no ten have Ki = 0; a first data frame that has is passed
 * over and the decoder starts again with the next one. */
static void b1u_lead(v92d_t *p, int16_t y)
{
    const v92_map_t *m = &p->cpd.map;
    int i = p->b1_sym, set = m->cidx[i % V92_CFRAME], lc = m->lc[set], j;
    int32_t a = y < 0 ? -(int32_t)y : y;

    for (j = 0; j + 1 < lc && 2 * a > (int32_t)m->pt[set][j] + m->pt[set][j + 1]; j++) ;
    if (v92_eta_to_k(m, i, y < 0 ? -j - 1 : j) == 0) p->b1_zero++;
    if (++p->b1_sym < V92_DFRAME) return;
    if (p->b1_zero < V92_DFRAME - 2 || p->b1_leads >= V92D_B1U_LEAD) { p->b1_sym = -1; return; }
    if (v92_dec_init(&p->dec, m, put_data, p) != 0) { fail(p, V92D_ERR_B1); return; }
    v92_dec_any_start(&p->dec);
    p->m_b1u += V92_DFRAME;
    p->b1_left = (int32_t)V92_B1U_FRAMES * m->K; p->b1_bad = 0;
    p->b1_sym = 0; p->b1_zero = 0; p->b1_leads++;
    p->cd_acc = 0; p->cd_n = 0;
}

/* ---- sequences: frame sync hunt, parse ---- */

enum { W_JA, W_CPT, W_PH4 };

static void seq_reset(v92d_t *p, int want)
{
    p->fn = 0; p->ones = 0; p->zeros = 0; p->want = want; p->after_seq = -1;
}

static void on_cpt(v92d_t *p)
{
    v90_cp_t cp;
    if (p->got_cpt) return;
    v92_cpu_to_v90(&p->cpx, &cp);
    if (p->cpx.drn == 0 || v90tx_load_cp(&p->tx, &cp) != 0) { fail(p, V92D_ERR_CPT); return; }
    p->got_cpt = 1;
    /* 9.5.1.1.12: Ri on CPt; 9.5.1.1.13: Ri is on already, Ri-bar on CPt */
    if (p->ja.dil.n > 0) { (void)v90tx_select(&p->tx, V90TX_RI); p->ri_sent = 1; }
    else (void)v90tx_select(&p->tx, V90TX_RI_BAR);
}

static void on_cpu(v92d_t *p)
{
    v90_cp_t cp;
    if (p->cpx.drn == 0) { fail(p, V92D_ERR_CLEARDOWN); return; }          /* 9.11 */
    v92_cpu_to_v90(&p->cpx, &cp);
    if (v90tx_load_cp(&p->tx, &cp) != 0) { fail(p, V92D_ERR_CPU); return; }
    p->got_cpu = 1;
    if (p->cpx.ack) p->got_ack = 1;
    p->down_rate = (p->cpx.drn + 20) * 8000 / 6;
}

/* a complete candidate may be in fbuf: 1 = taken, 0 = need more, -1 = not one */
static int seq_try(v92d_t *p)
{
    int r;
    if (p->want == W_JA) {
        if (p->fn < 276 || p->fn % 12) return 0;
        r = v92_ja_parse(p->fbuf, p->fn, &p->ja);
        if (r > 0) p->got_ja = 1;
    } else if (p->want == W_CPT) {
        if (p->fn < 288 || p->fn % 12) return 0;
        r = v92_up_kind(p->fbuf, p->fn) == V92_UP_CPT ? v92_cpu_parse(p->fbuf, p->fn, 12, &p->cpx) : V92_ERR_RANGE;
        if (r > 0) on_cpt(p);
    } else {
        int kind;
        if (p->fn < V92_SUV_BITS || p->fn % 24) return 0;
        kind = v92_up_kind(p->fbuf, p->fn);
        if (kind == V92_UP_SUVU) {
            v92_suvu_t s;
            r = v92_suvu_parse(p->fbuf, p->fn, 24, &s);
            if (r > 0) { p->got_suvu = 1; if (s.ack) p->got_ack = 1; }
        } else if (kind == V92_UP_CPU) {
            r = v92_cpu_parse(p->fbuf, p->fn, 24, &p->cpx);
            if (r > 0) on_cpu(p);
        } else {
            r = V92_ERR_RANGE;                  /* CPus: fast parameter exchange only */
        }
    }
    return r > 0 ? 1 : r == V92_ERR_SHORT ? 0 : -1;
}

/* one received bit of a sequence; m is the number of the symbol it came in */
static void seq_bit(v92d_t *p, int bit, int32_t m, int last_of_symbol)
{
    /* E1u / E2u: a data frame of zeros right after a complete sequence */
    if (p->after_seq >= 0) {
        int elen = p->want == W_PH4 ? V92_E2U_BITS(2, 0) : V92_E1U_BITS;
        if (bit) p->after_seq = -1;
        else if (++p->after_seq == elen && last_of_symbol) {
            p->after_seq = -1;
            if (p->want == W_CPT) {
                p->got_e1u = 1;
                p->m_trn2u = m + 1;
            } else if (p->want == W_PH4) {
                p->got_e2u = 1;
                upstream_start(p, m + 1);
            }
            p->fn = 0; p->ones = 0;
            return;
        }
    }
    if (p->fn == 0) {
        if (bit) { p->ones++; return; }
        if (p->ones >= 17) { memset(p->fbuf, 1, 17); p->fbuf[17] = 0; p->fn = 18; }
        p->ones = 0;
        return;
    }
    p->fbuf[p->fn++] = (uint8_t)bit;
    {
        int r = seq_try(p);
        if (r > 0) { p->fn = 0; p->ones = 0; p->after_seq = 0; }
        else if (r < 0 || p->fn >= V92D_FBUF) { p->fn = 0; p->ones = 0; }
    }
}

/* ---- our Phase 4 sequences ---- */

#ifdef V92D_PROBE
/* Bench only, never in the product: replace the designed filters by trivial
 * ones, so that a recording of B1u from a real modem shows how it reads CPd
 * one layer at a time. The call itself cannot work.
 *   1: z2 = 1, p1 = 0            2: z2 = 1, p1(1) = 1/2
 *   3: z2 = 0 0 0 1 1/2, p1 = 0  4: as 1 with twice the gain */
static void probe_cpd(v92_cpd_t *c)
{
    v92_filt_t *f = &c->filt;
    int k;

    if (V92D_PROBE == 7) {              /* the design, but p1 = 0 */
        for (k = 0; k < f->lp1; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 8) {              /* the design, but z2 = one tap at 64 */
        for (k = 0; k < f->lz2; k++) f->coef[f->lp1 + k] = (int16_t)(k == 64 ? 32767 : 0);
        return;
    }
    if (V92D_PROBE == 10) {             /* the design, z2 three taps later */
        for (k = f->lz2 - 1; k >= 0; k--) f->coef[f->lp1 + k] = k >= 3 ? f->coef[f->lp1 + k - 3] : 0;
        return;
    }
    if (V92D_PROBE == 11 || V92D_PROBE == 12) {     /* the design with LP1 = 48; 12: and p1 = 0 */
        for (k = 0; k < f->lz2; k++) f->coef[48 + k] = f->coef[f->lp1 + k];
        f->lp1 = 48;
        if (V92D_PROBE == 12) for (k = 0; k < 48; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 13 || V92D_PROBE == 14) {     /* the design with LZ2 = 96 (taps 16..111); 13: and p1 = 0 */
        for (k = 0; k < 96; k++) f->coef[f->lp1 + k] = f->coef[f->lp1 + 16 + k];
        f->lz2 = 96;
        if (V92D_PROBE == 13) for (k = 0; k < f->lp1; k++) f->coef[k] = 0;
        return;
    }
    if (V92D_PROBE == 9) {              /* the design at half the gain */
        f->gain4 = (uint16_t)(f->gain4 / 2);
        return;
    }
    f->lz1 = f->lp2 = 0; f->lp1 = 1; f->lz2 = 1;
    for (k = 0; k < 8; k++) f->coef[k] = 0;
    f->coef[1] = 32767;
    if (V92D_PROBE == 2) f->coef[0] = 8192;
    if (V92D_PROBE == 3) { f->lz2 = 5; f->coef[1] = 0; f->coef[4] = 32767; f->coef[5] = 16384; }
    if (V92D_PROBE == 4) f->gain4 = (uint16_t)(f->gain4 * 2);
    if (V92D_PROBE >= 5) {      /* 5: the designed lengths, z2 = one tap at 64, p1 = 0;
                                   6: ... and p1(2) = 1/2 */
        f->lp1 = 64; f->lz2 = 128;
        for (k = 0; k < 192; k++) f->coef[k] = 0;
        f->coef[64 + 64] = 32767;
        if (V92D_PROBE == 6) f->coef[1] = 8192;
    }
}
#endif

static void build_seqs(v92d_t *p)
{
    int D = V90_MAP_D(&p->tx.map[V90TX_MAP_TRAIN]), a, n;
    for (a = 0; a < 2; a++) {
        v92_suvd_t s;
        v92_cpd_t *c = &p->cpd;
        s.silence = 0; s.ack = (uint8_t)a;
        n = v92_suvd_build(&s, D, p->sbits);
        v90_pack_bits(p->sbits, n, p->xsuvd[a]);
        (void)v90tx_set_xseq(&p->tx, a ? V90TX_XSEQ_SUVD_ACK : V90TX_XSEQ_SUVD, p->xsuvd[a], n);
        c->ack = (uint8_t)a;
        n = v92_cpd_build(c, D, p->sbits);
        if (n <= 0 || n > V92D_SBUF) { fail(p, V92D_ERR_DESIGN); return; }
        v90_pack_bits(p->sbits, n, p->xcpd[a]);
        (void)v90tx_set_xseq(&p->tx, a ? V90TX_XSEQ_CPD_ACK : V90TX_XSEQ_CPD, p->xcpd[a], n);
        p->ncpd = n;
    }
}

/* the receive codec's quantising noise at this sample: none to speak of
 * when it hands us 16-bit linear samples (1/12 of a unit squared) */
static uint32_t qvar(const v92d_t *p, int32_t a)
{
    return p->cfg.rx_linear ? 0 : v92_dsn_qvar(p->cfg.alaw, a);
}

static void design_start(v92d_t *p)
{
    v92_dsn_cfg c;
    int64_t ge = 0;
    uint32_t pred;
    int j;
    memset(&c, 0, sizeof c);
    c.alaw = p->cfg.alaw;
    for (j = 0; j < V92_CH_NTAP; j++) c.h[j] = v92_trn_h(&p->trn, j);
    c.rx_linear = p->cfg.rx_linear;
    c.noise = p->noise; c.q_quiet = p->q_quiet;
    /* the echo in data mode: what the canceller's taps make of the assumed
     * downstream power, if that is more than TRN1d gave */
    for (j = 0; j < EC_NTAP; j++) { int64_t v = p->ec.h[j] >> 12; ge += v * v; }
    pred = (uint32_t)(((uint64_t)isqrt64((uint64_t)ge) * (uint32_t)(p->cfg.down_rms > 0 ? p->cfg.down_rms : DOWN_RMS)) >> 12);
    c.echo_rms = pred > p->echo_rms ? pred : p->echo_rms;
    c.res = p->trn.res; c.q_trn = p->q_trn;
    c.ltot = (uint16_t)p->cfg.ltot; c.lmax = (uint16_t)p->cfg.lmax;
    c.rates = p->ja.rates;
    c.max_rate = p->cfg.max_up_rate;
    if (p->rate_drops && p->up_rate) {
        int lim = p->up_rate - 4000 * p->rate_drops;
        if (!c.max_rate || lim < c.max_rate) c.max_rate = lim;
    }
    c.kappa_q4 = p->cfg.kappa_q4;
    v92_dsn_start(&p->dsn, &c);
    p->dsn_state = 1;
    /* the design has its copy: the estimator's work is not needed until the
     * next rate renegotiation, and the processor's time is (reading 56) */
    v92_trn_hold(&p->trn, 1);
}

static void sel(v92d_t *p, v90tx_sig_t s)
{
    if (v90tx_signal(&p->tx) != s && v90tx_pending(&p->tx) != s) (void)v90tx_select(&p->tx, s);
}

/* ---- rate renegotiation ---- */

static void reneg_begin(v92d_t *p, int ours)
{
    /* our own: what the caller sends until it has seen Rd is still data */
    p->tx_ready = 0; if (!ours) p->rx_ready = 0;
    p->in_reneg = 1; p->reneg_ours = ours; p->revs = 0;
    p->got_suvu = p->got_cpu = p->got_ack = p->got_e2u = 0;
    p->cpd_sent = p->cpd_repeat = 0; p->t_cpd = 0;
    p->dsn_state = 0;
    if (ours) (void)v90tx_select(&p->tx, V90TX_RD);     /* on the next data frame boundary */
    enter(p, V92D_RENEG);
}

/* the caller's Ru has just turned into Ru-bar: TRN2u is 24 symbols away */
static void reneg_resume(v92d_t *p)
{
    int32_t kr = p->trn.k - 1 - V92_P6_REV_LAG;                     /* first sample of Ru-bar */
    int32_t mt = kr - (p->trn.cpk - V92_CH_LEAD) + 24, d;
    /* Ru began on a data frame boundary (9.8.2.1.1) */
    d = (mt - p->m_b1u) % 12; if (d < 0) d += 12;
    mt += d < 6 ? -d : 12 - d;
    p->rx_on = 0;
    v92_trn_resume(&p->trn, mt);
    v92_trn_hold(&p->trn, 0);
    p->trn_on = 1; p->four = 1; p->m_trn2u = mt;
    v92_ml_init(&p->ml, p->jp.pt8_reneg, v92_ru_sbit(23, 1));
    seq_reset(p, W_PH4);
}

/* Loss of the upstream signal in data mode: far below what B1u brought, for
 * about half a second. One sample in; 1 = lost. */
static int carrier_lost(v92d_t *p, int16_t x)
{
    p->cd_acc += (int32_t)x * x;
    if (++p->cd_n < CD_BLOCK) return 0;
    if ((uint64_t)(p->cd_acc / CD_BLOCK) < (p->sig_ms >> CD_SHIFT)) p->cd_quiet++; else p->cd_quiet = 0;
    p->cd_acc = 0; p->cd_n = 0;
    return p->cd_quiet >= CD_BLOCKS;
}

/* ---- the tick ---- */

uint8_t v92d_tick(v92d_t *p, int16_t rxs)
{
    uint8_t octet;
    int16_t x;
    v90tx_sig_t sig;
    int r;

    p->now++; p->t++;
    octet = v90tx_tick(&p->tx);
    p->ecdl[p->ecpos] = (int16_t)v90_octet_linear(p->cfg.alaw, octet);
#ifdef P34D_TRACE
    p->ecdl[p->ecpos] = v92d_trace_tx(p, p->ecdl[p->ecpos]);
#endif
    /* The echo: the canceller of ../modem/ec for the first 12 ms, and behind it
     * a model of a slow tail (reading 53): the transmitted signal, 12 ms on,
     * through two first order low-passes (16 and 32 ms), one coefficient
     * each. All of it learns from what is left after both, while the caller
     * is silent. */
    {
        int32_t e, est, u1, u2, tl = p->ecdl[(p->ecpos - p->echo_delay - EC_NTAP) & (V92D_ECDL - 1)];
        e = ec_tick(&p->ec, p->ecdl[(p->ecpos - p->echo_delay) & (V92D_ECDL - 1)], rxs, -1);
        p->et_s[0] += tl - (p->et_s[0] >> 7);
        p->et_s[1] += tl - (p->et_s[1] >> 8);
        u1 = p->et_s[0] >> 5; u2 = p->et_s[1] >> 6;
        est = (int32_t)(((int64_t)p->et_g[0] * u1 + (int64_t)p->et_g[1] * u2 + 32768) >> 16);
        e -= est;
        p->et_pw += ((int64_t)u1 * u1 + (int64_t)u2 * u2 - p->et_pw) >> 10;
        if (p->ec_mu >= 0) {
            ec_adapt(&p->ec, e, p->ec_mu);
            if (p->et_pw > 4096) {
                p->et_g[0] += (int32_t)((((int64_t)e * u1) << 10) / p->et_pw);
                p->et_g[1] += (int32_t)((((int64_t)e * u2) << 10) / p->et_pw);
            }
        }
        x = (int16_t)(e > 32767 ? 32767 : e < -32767 ? -32767 : e);
    }
    p->ecpos = (p->ecpos + 1) & (V92D_ECDL - 1);
#ifdef P34D_TRACE
    v92d_trace(p, rxs, x);      /* live/replay.c: what the canceller left */
#endif
    sig = v90tx_signal(&p->tx);

    if (p->state >= V92D_RETRAIN) return octet;

    /* ---- receiver ---- */
    if (p->state != V92D_WAIT_RU && p->state != V92D_MD && tone_a(p, x)) { retrain(p, 0); return octet; }

    if (p->trn_at && p->now == p->trn_at) {
        p->trn_at = 0;
        v92_trn_init(&p->trn); v92_sgn_init(&p->sgn, 0);
        p->trn_on = 1; p->four = 0;
        seq_reset(p, p->state == V92D_TRN1U ? W_JA : W_CPT);
    }
    if (p->trn_on == 1) {
        p->q_trn = (uint32_t)((int64_t)p->q_trn + ((int64_t)qvar(p, rxs) - (int64_t)p->q_trn) / 1024);
        r = v92_trn_tick(&p->trn, x);
        if (r < 0) { fail(p, p->state == V92D_TRN1U ? V92D_ERR_JA : V92D_ERR_CPT); return octet; }
        if (r == 1 && p->trn.dec_m >= 0) {
            int32_t m = p->trn.dec_m;
            if (!p->four) {
                int sbit = p->trn.dec_level < 0;
                int bit = v92_sgn_rx_bit(&p->sgn, sbit);
#ifdef P34D_TRACE
                v92d_trace_sym(p, bit);
#endif
                seq_bit(p, bit, m, 1);
                if (p->got_e1u && !p->four) {       /* TRN2u follows (8.7.6) */
                    p->four = 1;
                    v92_trn_levels(&p->trn, 4);
                    v92_ml_init(&p->ml, p->jp.pt8_train, sbit);
                    seq_reset(p, W_PH4);
                }
            } else if (m >= p->m_trn2u) {
                uint8_t b[3];
                v92_ml_rx(&p->ml, p->trn.dec_level, b);
#ifdef P34D_TRACE
                v92d_trace_sym(p, b[0] | (b[1] << 1));
#endif
                seq_bit(p, b[0], m, 0);
                if (p->state < V92D_RETRAIN) seq_bit(p, b[1], m, 1);
            }
            if (p->state >= V92D_RETRAIN) return octet;
        }
    } else if (p->trn_on == 2) {
        v92_trn_store(&p->trn, x);
    }
    if (p->rx_on) {
        if (p->b1_left > 0) { p->cd_acc += (int32_t)x * x; p->cd_n++; }
        while (p->rx_on && p->data_k < p->trn.k) {
#ifdef P34D_TRACE
            v92d_trace_data(p, v92_trn_sample(&p->trn, p->data_k));
#endif
            v92_dec_put(&p->dec, v92_trn_sample(&p->trn, p->data_k++), 0);
            if (p->b1_sym >= 0) b1u_lead(p, v92_trn_sample(&p->trn, p->data_k - 1));
        }
        if (p->state >= V92D_RETRAIN) return octet;
    }

    /* ---- procedures ---- */
    switch (p->state) {
    case V92D_WAIT_RU:
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (r & V92_P6_REV) {
            if (p->cfg.md_len > 0 && !p->revs) { p->revs = 1; enter(p, V92D_MD); break; }     /* 9.5.1.1.1 */
            p->trn_at = p->now - V92_P6_REV_LAG + 24 - TRN_LEAD;
            enter(p, V92D_TRN1U);
        } else if (p->t > MS(T_RU_MS) + p->cfg.md_len * 276) {
            fail(p, V92D_ERR_NO_RU);
        }
        break;

    case V92D_MD:
        /* Ru-bar ends 24 - LAG symbols from the reversal; then MD; listen again a little early */
        if (p->t >= 20 + p->cfg.md_len * 276 - 48) { v92_p6_init(&p->p6); p->state = V92D_WAIT_RU; }
        break;

    case V92D_TRN1U:
        if (p->got_ja) {
            p->peak1 = v92_trn_peak(&p->trn);
            p->trn_on = 0;
            if (v90tx_load_dil(&p->tx, &p->ja.dil) != 0 || v90tx_set_jd(&p->tx, &p->jd) != 0) { fail(p, V92D_ERR_JA); break; }
            (void)v90tx_select(&p->tx, V90TX_SD);       /* 9.5.1.1.3; Sd-bar and TRN1d follow by themselves */
            enter(p, V92D_SD);
        } else if (p->t > MS(T_JA_MS) + p->cfg.rtd) {
            fail(p, V92D_ERR_JA);
        }
        break;

    case V92D_SD:
        if (sig == V90TX_TRN1D) {
            int32_t n = (int32_t)v90tx_symbols(&p->tx), from = p->cfg.rtd + MS(EC_GUARD_MS), len;
            if (n == 1) p->t_trn1d = p->now;
            if (p->cfg.echo_delay < 0) {
                /* first find out how late the echo is (see echo_search) */
                int32_t snap = from + V92D_EC_MAXLAG + V92D_EC_WIN;
                if (n > snap - V92D_EC_WIN && n <= snap) {
                    if (p->ecw_n < V92D_EC_WIN) p->ecwin[p->ecw_n++] = rxs;
                    if (n == snap) { p->ecs_pos = p->ecpos; p->ecs_lag = 0; p->ecs_best = -1; }
                } else if (n > snap && p->ecs_lag <= V92D_EC_MAXLAG) {
                    echo_search(p);
                }
                from = snap + V92D_EC_MAXLAG / EC_SEARCH_STEP + 2;
            }
            len = TRN1D_SYMS + from;
            /* the caller is silent now: learn the echo of TRN1d */
            p->ec_mu = n < from ? -1 : n < from + EC_FAST ? 0 : 3;
            if (n >= len - NOISE_SYMS) {
                p->nz_acc += (int32_t)x * x;
                p->nq_acc += qvar(p, rxs);
                p->ne_acc += (int32_t)rxs * rxs;
                p->nz_n++;
            }
            if (n >= len) {
                p->ec_mu = -1;
                p->noise = (uint32_t)(p->nz_acc / p->nz_n);
                p->q_quiet = (uint32_t)(p->nq_acc / p->nz_n);
                p->echo_rms = isqrt64((uint64_t)(p->ne_acc / p->nz_n));
                (void)v90tx_select(&p->tx, V90TX_JD);                   /* 9.5.1.1.4 */
                v92_p6_init(&p->p6); p->revs = 0;
                enter(p, V92D_JD);
            }
        }
        break;

    case V92D_JD:
        /* 9.5.1.1.6 to .8: Su, its reversal, then Jp with the phase */
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (r & V92_P6_REV) {
            int n;
            /* The peak of the channel response sat peak1 / 256 samples from a
             * sampling instant during the first TRN1u; the caller has since
             * slipped half a symbol (9.5.2.1.7). Ask for the further slip
             * that puts the peak on a sampling instant. */
            p->jp.frac = (uint16_t)(((-p->peak1 - 128) & 255) << 8);
            p->jp.pt8_train = 0; p->jp.pt8_reneg = 0;
            n = v92_jp_build(&p->jp, p->sbits);
            v90_pack_bits(p->sbits, n, p->xjp);
            (void)v90tx_set_xseq(&p->tx, V90TX_XSEQ_JP, p->xjp, n);
            (void)v90tx_select(&p->tx, V90TX_JP);                       /* after the current Jd */
            enter(p, V92D_JP);
        } else if (p->now - p->t_trn1d > MS(T_SU_MS) + p->cfg.rtd) {
            retrain(p, 1);                                              /* 9.5.1.2.2 */
        }
        break;

    case V92D_JP:
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if ((r & V92_P6_REV) && sig == V90TX_JP) {
            /* 9.5.1.1.9: Jp' after the current Jp; the DIL or SCR follow by themselves */
            (void)v90tx_select(&p->tx, V90TX_JP_PRIME);
            p->trn_at = p->now - V92_P6_REV_LAG + 24 - TRN_LEAD;
            p->got_cpt = p->got_e1u = p->ri_sent = 0;
            enter(p, V92D_TRAIN);
        } else if (p->t > MS(T_SU_MS) + p->cfg.rtd) {
            retrain(p, 1);
        }
        break;

    case V92D_TRAIN:
        /* 9.5.1.1.13: no DIL, Ri when trained */
        if (p->ja.dil.n == 0 && !p->ri_sent && sig == V90TX_SCR && p->trn_on == 1 && p->trn.neq >= TRAINED_SYMS) {
            (void)v90tx_select(&p->tx, V90TX_RI);
            p->ri_sent = 1;
        }
        if (p->got_e1u) {
            if (p->ja.dil.n > 0) (void)v90tx_select(&p->tx, V90TX_RI_BAR);          /* 9.5.1.1.12 */
            p->got_suvu = p->got_cpu = p->got_ack = p->got_e2u = 0;
            p->cpd_sent = p->cpd_repeat = 0; p->t_cpd = 0; p->dsn_state = 0;
            enter(p, V92D_PH4);
        } else if (p->t > MS(T_TRAIN_MS) + p->cfg.rtd) {
            fail(p, V92D_ERR_CPT);
        }
        break;

    case V92D_PH4: {
        int ack = p->got_cpu;
        v90tx_sig_t suvd = ack ? V90TX_SUVD_ACK : V90TX_SUVD, cpd = ack ? V90TX_CPD_ACK : V90TX_CPD;
        int is_suvd = sig == V90TX_SUVD || sig == V90TX_SUVD_ACK, is_cpd = sig == V90TX_CPD || sig == V90TX_CPD_ACK;

        /* the design, on the estimate TRN2u has refined */
        if (p->dsn_state == 0 && p->trn_on == 1 && p->four &&
            p->trn.dec_m - p->m_trn2u >= (p->in_reneg ? RENEG_REFINE : REFINE_SYMS)) design_start(p);
        if (p->dsn_state == 1 && (p->cfg.dsn_pace <= 1 || p->now % p->cfg.dsn_pace == 0)) {
            r = v92_dsn_step(&p->dsn);
            if (r < 0) { fail(p, V92D_ERR_DESIGN); break; }
            if (r == 1) {
                p->cpd = p->dsn.cpd;
                p->cpd.e2u_extend = 0;
#ifdef V92D_PROBE
                probe_cpd(&p->cpd);
#endif
                if (p->clearing) p->cpd.drn = 0;
                p->dsn_state = 2;
                build_seqs(p);
                if (p->state != V92D_PH4) break;
            }
        }
        if (p->dsn_state == 2) {
            if (p->clearing) {
                /* 9.11: CPd with drn = 0, then silence */
                if (sig == V90TX_TRN2D) sel(p, V90TX_CPD);
                else if (is_cpd && v90tx_units(&p->tx) >= CLEAR_CPDS) fail(p, V92D_ERR_HANGUP);
            } else if (sig == V90TX_TRN2D) {
                /* 9.6.1.1.1: TRN2d for at least 2040 T, then SUVd */
                if (v90tx_units(&p->tx) >= (uint32_t)(p->in_reneg ? RENEG_TRN2D : TRN2D_FRAMES)) sel(p, suvd);
            } else if ((sig == V90TX_SUVD_ACK || sig == V90TX_CPD_ACK) && v90tx_units(&p->tx) >= 1
                       && (p->got_ack || p->got_e2u) && v90tx_pending(&p->tx) == V90TX_NONE) {
                /* 9.6.1.1.4: Ed, then B1d and data by themselves */
                v90tx_set_getbit(&p->tx, p->cfg.getbit, p->cfg.user);
                (void)v90tx_select(&p->tx, V90TX_ED);
                enter(p, V92D_ED);
            } else if (is_suvd) {
                if (p->cpd_sent == 1) { p->cpd_sent = 2; p->t_cpd = p->now; }       /* the single CPd is out */
                if (p->got_suvu && !p->cpd_sent) sel(p, cpd);                       /* 9.6.1.1.2 */
                else if (p->cpd_sent == 2 && !p->got_ack && !p->cpd_repeat
                         && p->now - p->t_cpd > MS(100) + p->cfg.rtd + 1200) { p->cpd_repeat = 1; sel(p, cpd); }   /* 9.6.1.1.3 */
                else if (sig != suvd) sel(p, suvd);                                 /* the acknowledge bit */
            } else if (is_cpd) {
                if (!p->cpd_sent) p->cpd_sent = 1;
                if (!p->cpd_repeat || p->got_ack) sel(p, suvd);                     /* a single CPd, then SUVd again */
                else if (sig != cpd) sel(p, cpd);
            }
        }
        if (p->state == V92D_PH4 && p->t > MS(p->in_reneg ? T_RENEG_MS + pace_ms(p) : T_PHASE4_MS) + 6 * p->cfg.rtd) {
            if (p->in_reneg) retrain(p, 1); else fail(p, V92D_ERR_CPU);
        }
        break;
    }

    case V92D_ED:
        if (sig == V90TX_DATA) p->tx_ready = 1;
        if (p->tx_ready && p->rx_ready) {
            if (p->in_reneg) { p->renegs++; p->in_reneg = 0; }
            v92_p6_init(&p->p6);
            enter(p, V92D_DATA);
        } else if (p->t > MS(p->in_reneg ? T_RENEG_MS + pace_ms(p) : T_PHASE4_MS) + 6 * p->cfg.rtd) {
            if (p->in_reneg) retrain(p, 1); else fail(p, V92D_ERR_B1);
        }
        break;

    case V92D_DATA:
        /* 9.8.1.2.1: Ru in data mode */
        if (v92_p6_tick(&p->p6, x, RU_FLOOR) & V92_P6_ON) { reneg_begin(p, 0); break; }
        if (carrier_lost(p, x)) { fail(p, V92D_ERR_CARRIER); break; }
        /* is the line still good enough for the rate? */
        if ((p->t & 8191) == 0) {
            uint32_t nb = p->dec.bad_frames - p->bad_seen;
            p->bad_seen = p->dec.bad_frames;
            p->q_bad = nb >= 4 ? p->q_bad + 1 : 0;
            if (p->q_bad >= 2) {
                p->rate_drops++;
                if (p->up_rate - 4000 * p->rate_drops < 24000) retrain(p, 1);
                else reneg_begin(p, 1);
            }
        }
        break;

    case V92D_RENEG:
        /* until Ru-bar the caller sends data or Ru: a signal that dies away
         * can look like the start of Ru to the detector */
        if (!p->revs && carrier_lost(p, x)) { fail(p, V92D_ERR_CARRIER); break; }
        r = v92_p6_tick(&p->p6, x, RU_FLOOR);
        if (r) p->rx_ready = 0;                 /* Ru: no more data (the last 13 ms in the decoder are dropped) */
        if ((r & V92_P6_REV) && !p->revs) {
            p->revs = 1;
            reneg_resume(p);
            if (!p->reneg_ours) (void)v90tx_select(&p->tx, V90TX_RD);   /* 9.8.1.2.2 */
        }
        /* Rd for 384 T, then Rd-bar; TRN2d follows by itself */
        if (sig == V90TX_RD && v90tx_units(&p->tx) >= RD_UNITS && v90tx_pending(&p->tx) == V90TX_NONE)
            (void)v90tx_select(&p->tx, V90TX_RD_BAR);
        if (p->revs && (sig == V90TX_RD_BAR || sig == V90TX_TRN2D)) enter(p, V92D_PH4);
        else if (p->t > MS(T_RENEG_MS)) retrain(p, 1);
        break;

    default:
        break;
    }
    return octet;
}

int v92d_renegotiate(v92d_t *p)
{
    if (p->state != V92D_DATA) return -1;
    reneg_begin(p, 1);
    return 0;
}

int v92d_cleardown(v92d_t *p)
{
    if (p->state != V92D_DATA) return -1;
    p->clearing = 1;
    reneg_begin(p, 1);
    return 0;
}

int v92d_retrain(v92d_t *p)
{
    if (p->state != V92D_DATA && !p->in_reneg) return -1;
    retrain(p, 1);
    return 0;
}
