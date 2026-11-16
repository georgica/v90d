/* V.32bis answering modem: start-up (6.2), data, retrain (7.2).
 *
 *   AC  ->  CA  ->  AC  -> 16 T silence -> S, S-bar, TRN -> R1 ...     (we send)
 *   AA      ->  CC   -> (silent)                              S ..     (the caller)
 *
 * The AC/CA and AA/CC phase reversals time the round trip (MT), which places
 * the far echo canceller. Our first S, S-bar, TRN run with the caller silent:
 * the echo cancellers train on that TRN. R1 follows until the caller's S is
 * heard; then we are silent while the receiver trains on the caller's S,
 * S-bar, TRN and reads R2. A second S, S-bar, TRN (now in double talk)
 * trains the caller's receiver, R3 names the rate, both ends send E and
 * scrambled ones (B1), then data.
 */
#include "v32_priv.h"

#define MS(ms) ((uint32_t)(ms) * 8)
/* samples per symbol = 10/3 */
#define SYM2SAMP(n) ((int32_t)(n) * 10 / 3)

/* samples from a reversed symbol's pulse centre arriving at the line to the
 * tone detector firing: filter bank centre 16, two symbols of averaging, and
 * "three of the four newest T/2 samples" (measured: test_startup) */
#define RX_REV_LAT 19

#define TRN1_MIN   1280          /* 5.2.3: at least 1280 symbols */
#define TRN1_MAX   8192          /* 5.2.3: at most 8192 symbols */
#define TRN2_LEN   1280
#define T_NO_TONE  MS(20000)     /* AC with no AA in reply */
#define T_REVERSAL MS(4000)      /* CA / second AC unanswered */
#define T_TRAIN    MS(10000)     /* no S / R2 / E */

/* mean-square error, 4096 per signal-space unit squared, below which a rate
 * is taken (see README: decisions 2.8 standard deviations from a boundary) */
static const int32_t rate_mse[V32_NRATES] = { 0x7fffffff, 4177, 2088, 1044, 522 };
/* ... and where a rate in use is given up: set by trial a little short of
 * where each rate's errors begin in test_call's noise runs (about 21, 18,
 * 15, 12 and 10 dB of SNR) */
static const int32_t retrain_mse[V32_NRATES] = { 12000, 7500, 4200, 2400, 850 };
#define RT_BAD_MS  2000
#define RT_MAX     3
#define CD_LOSS_MS 400
/* three times |y|^2 of the receive filter output is compared with the
 * tone detectors' -43 dBm0 threshold of v32_rx.c (2512^2): 4.8 dB below it */
#define CD_OFF     ((int64_t)2512 * 2512)

static void go(v32_t *m, v32_state s)
{
    m->seg_len[m->state] = m->seg;
    m->state = s; m->seg = 0; m->t_state = 0;
}

static void fail(v32_t *m, int error)
{
    go(m, V32S_FAILED);
    m->error = error;
    m->connected = 0; m->retraining = 0; m->tx_ready = m->rx_ready = 0;
    v32tx_set_mode(&m->tx, V32_TX_SILENCE);
    m->ec_mu = -1;
}

static void start(v32_t *m)
{
    m->state = V32S_AC1; m->seg = 0; m->t_state = 0;
    v32tx_set_mode(&m->tx, V32_TX_AC);
    v32rx_set_mode(&m->rx, V32_RX_TONE);
    m->rx.tone_run = 0; m->rx.pure_run = 0; m->rx.rev_count = 0;
    m->ec_mu = -1;
    m->ca_len = 0; m->drop_run = 0;
    m->rx_e_seen = 0; m->tx_ready = m->rx_ready = 0;
    m->tone_data_run = 0; m->bad_run = 0; m->lost_run = 0; m->carrier = 0;
}

void v32_init(v32_t *m, const v32_cfg *cfg)
{
    int i;
    m->cfg = *cfg;
    if (!(m->cfg.rates & V32_RALL)) m->cfg.rates = V32_RALL;
    v32tx_init(&m->tx, 1, cfg->level, cfg->getbit, cfg->user);
    v32ec_init(&m->ec);
    v32ec_set_near_delay(&m->ec, cfg->echo_delay);
    v32rx_init(&m->rx, 1, cfg->putbit, cfg->user);
    m->error = V32_ERR_NONE;
    m->nsamp = 0;
    m->t_ca = m->t_ac2 = 0; m->mt = 0;
    m->tone_ref = 0;
    m->ec_best = 0; m->ec_block = 0; m->ec_stall = 0; m->erle_db10 = 0; m->trn1_min = TRN1_MIN;
    m->r2_rates = 0; m->rate = -1; m->v32_only = 0;
    m->connected = 0; m->bitrate = 0; m->retrains = 0; m->retraining = 0;
    m->auto_rt = 0; m->good_run = 0;
    for (i = 0; i <= V32S_FAILED; i++) m->seg_len[i] = 0;
    start(m);
}

int v32_retrain(v32_t *m)
{
    if (m->state != V32S_DATA) return -1;
    m->retrains++; m->retraining = 1;
    m->seg_len[m->state] = m->seg;
    start(m);
    return 0;
}

/* 10 log10(a / b) in tenths of a dB, for the record */
static int32_t db10(int64_t a, int64_t b)
{
    int32_t d = 0;
    if (b < 1) b = 1;
    if (a < 1) return 0;
    while (b * 10 <= a && d < 900) { b *= 10; d += 100; }
    while (b * 161 / 128 <= a && d < 990) { b = b * 161 / 128; d += 10; }
    return d;
}

static int pick_rate(const v32_t *m, unsigned common)
{
    int k, rate = -1, low = -1;
    for (k = 0; k < V32_NRATES; k++)
        if (common & (1u << k)) {
            if (low < 0) low = k;
            if (m->rx.mse <= rate_mse[k]) rate = k;
        }
    return rate >= 0 ? rate : low;
}

/* once per transmit symbol, before the symbol is generated: what to send */
static void tx_control(v32_t *m)
{
    v32tx_t *t = &m->tx;
    switch (m->state) {
    case V32S_AC1:
        /* 6.2: AC for an even number of symbols >= 128, and the caller's
         * 1800 Hz tone seen for 64 symbol periods: reverse, start the timer */
        if (m->seg >= 128 && !(m->seg & 1) && m->rx.tone_run >= 128) {
            go(m, V32S_CA);
            v32tx_set_mode(t, V32_TX_CA);
            m->t_ca = m->nsamp + V32_TX_DELAY;
            m->rx.rev_count = 0;
            m->tone_ref = m->rx.tone_q;
            m->ca_len = 0;
        }
        break;
    case V32S_CA:
        if (m->ca_len && (int)m->seg >= m->ca_len) {
            /* CA ended on an A: one more A begins AC */
            go(m, V32S_AC2);
            v32tx_set_mode(t, V32_TX_AC);
            m->t_ac2 = m->nsamp + V32_TX_DELAY;
            m->drop_run = 0;
        }
        break;
    case V32S_SILENCE:
        if (m->seg >= 16) { go(m, V32S_S1); v32tx_set_mode(t, V32_TX_S); }
        break;
    case V32S_S1:
        if (m->seg >= 256) { go(m, V32S_SBAR1); v32tx_set_mode(t, V32_TX_SBAR); }
        break;
    case V32S_SBAR1:
        if (m->seg >= 16) {
            go(m, V32S_TRN1); v32tx_set_mode(t, V32_TX_TRN);
            m->ec_mu = 0;                       /* the caller is silent: full step */
            m->ec_block = 0; m->ec_best = 0; m->ec_stall = 0;
        }
        break;
    case V32S_TRN1:
        /* The echo canceller trains here. Full step for 768 symbols, then
         * smaller ones to settle; past the minimum length TRN goes on only
         * while the residual echo is still falling. */
        if (m->seg + 512 == m->trn1_min) m->ec_mu = 1;
        if (m->seg + 256 == m->trn1_min) m->ec_mu = 2;
        if (m->seg >= m->trn1_min && ((m->seg - m->trn1_min) & 255) == 0) {
            int64_t out = m->ec.pw_out;
            int done = m->seg >= TRN1_MAX || m->ec.pw_in > out * 10000;      /* 40 dB: enough */
            if (!done && m->ec_block && out * 5 > m->ec_best * 4) done = 1;  /* less than 1 dB gained */
            m->ec_best = out; m->ec_block++;
            if (done) {
                m->erle_db10 = db10(m->ec.pw_in, m->ec.pw_out);
                m->ec_mu = -1;
                go(m, V32S_R1);
                t->rword = v32_rate_word(m->cfg.rates);
                v32tx_set_mode(t, V32_TX_RATE);
                m->rx.s_run = 0;
            }
        }
        break;
    case V32S_S2:
        if (m->seg >= 256) { go(m, V32S_SBAR2); v32tx_set_mode(t, V32_TX_SBAR); }
        break;
    case V32S_SBAR2:
        if (m->seg >= 16) { go(m, V32S_TRN2); v32tx_set_mode(t, V32_TX_TRN); }
        break;
    case V32S_TRN2:
        if (m->seg >= TRN2_LEN) {
            /* R3: one rate, within R2, that this receiver can take: its error
             * now includes what is left of our own echo */
            unsigned common = m->cfg.rates & m->r2_rates;
            if (m->v32_only) common &= V32_R4800;
            m->rate = common ? pick_rate(m, common) : -1;
            t->rword = v32_rate_word(m->rate >= 0 ? 1u << m->rate : 0);
            v32tx_set_mode(t, V32_TX_RATE);
            go(m, m->rate >= 0 ? V32S_R3 : V32S_CLEAR);
        }
        break;
    case V32S_R3:
        /* 6.2: on the caller's E, finish the current sequence, then our E */
        if (m->rx_e_seen && t->rbit == 0 && m->seg >= 16) {
            go(m, V32S_E);
            t->rword = (uint16_t)(v32_rate_word(1u << m->rate) | V32_RW_E);
        }
        break;
    case V32S_E:
        if (m->seg >= 8) {
            go(m, V32S_B1);
            t->rate = m->rate;
            v32tx_set_mode(t, V32_TX_ONES);
        }
        break;
    case V32S_B1:
        if (m->seg >= 128) {
            go(m, V32S_DATA);
            v32tx_set_mode(t, V32_TX_DATA);
            m->tx_ready = 1;
        }
        break;
    case V32S_CLEAR:
        /* Note 6: a cleardown R3 is repeated for at least 64 symbols */
        if (m->seg >= 64 && t->rbit == 0) fail(m, V32_ERR_NO_RATE);
        break;
    default:
        break;
    }
    m->seg++;
}

/* every sample, after the receiver has run: what was heard */
static void rx_control(v32_t *m)
{
    v32rx_t *r = &m->rx;
    switch (m->state) {
    case V32S_AC1:
        if (m->t_state > T_NO_TONE) fail(m, V32_ERR_NO_TONE);
        break;
    case V32S_CA:
        if (!m->ca_len && r->rev_count) {
            /* The caller's reversal: it left 64 T after ours arrived there.
             * Ours back (CA to AC) must leave 64 +- 2 T after this one came
             * in, and CA must be an even number of symbols. */
            int32_t arr = (int32_t)(r->rev_t - RX_REV_LAT);
            int n;
            m->mt = arr - (int32_t)m->t_ca;
            /* (with I/O buffering between us and the line, the reversal
             * was there earlier and ours gets there later: echo_delay in all) */
            n = ((m->mt - m->cfg.echo_delay + SYM2SAMP(64)) * 3 + 10) / 20 * 2;      /* nearest even symbol count */
            if (n < (int)m->seg + 2) n = (int)((m->seg + 3) & ~1u);
            m->ca_len = n;
        }
        if (m->t_state > T_REVERSAL) fail(m, V32_ERR_NO_TONE);
        break;
    case V32S_AC2:
        /* the caller stops when our second reversal reaches it: its tone drops */
        if (r->rev_hold == 0 && r->tone_q * 4 < m->tone_ref) m->drop_run++; else m->drop_run = 0;
        if (m->drop_run >= SYM2SAMP(4)) {
            /* far echo: back after the round trip, MT less the caller's 64 T.
             * The canceller's reference leads the line signal by V32_TXD
             * symbols and the model spans the pulse: start 28 samples early */
            int32_t rt = m->mt - SYM2SAMP(64);
            v32ec_set_bulk(&m->ec, rt - 28);
            /* and the far echo of TRN is back only a round trip into it:
             * the canceller needs its 1280 symbols from then on */
            m->trn1_min = TRN1_MIN + (rt > 0 ? (uint32_t)rt * 3 / 10 : 0);
            if (m->trn1_min > TRN1_MAX - 512) m->trn1_min = TRN1_MAX - 512;
            go(m, V32S_SILENCE);
            v32tx_set_mode(&m->tx, V32_TX_SILENCE);
        } else if (m->t_state > T_REVERSAL) fail(m, V32_ERR_NO_TONE);
        break;
    case V32S_R1:
        /* 6.2: on the caller's S, cease transmitting */
        if (m->t_state > MS(30) && r->s_run >= 40) {
            go(m, V32S_WAIT);
            v32tx_set_mode(&m->tx, V32_TX_SILENCE);
        } else if (m->t_state > T_TRAIN) fail(m, V32_ERR_TRAIN);
        break;
    case V32S_WAIT:
        /* ... wait MT (our own far echo dies away), then train on S if it is
         * still there, or when it comes back */
        if ((int32_t)m->t_state >= m->mt + V32_TX_DELAY && r->s_run >= 40) {
            go(m, V32S_RXTRAIN);
            v32rx_set_mode(r, V32_RX_S);
        } else if (m->t_state > T_TRAIN) fail(m, V32_ERR_TRAIN);
        break;
    case V32S_RXTRAIN:
        if (r->mode == V32_RX_DD4 && r->rate_seen >= 2) {
            /* R2 */
            m->r2_rates = v32_word_rates(r->rate_word);
            m->v32_only = !(r->rate_word & (1u << 11)) || !(r->rate_word & (1u << 7));   /* B4, B8 */
            go(m, V32S_S2);
            v32tx_set_mode(&m->tx, V32_TX_S);
        } else if (m->t_state > T_TRAIN) fail(m, V32_ERR_TRAIN);
        break;
    case V32S_S2: case V32S_SBAR2: case V32S_TRN2: case V32S_R3:
        if (r->e_word && !m->rx_e_seen) {
            m->rx_e_seen = 1;
            if (r->mode != V32_RX_DATA) { fail(m, V32_ERR_NO_RATE); break; }     /* E naming no rate */
        }
        if (m->state == V32S_R3 && m->t_state > T_TRAIN) fail(m, V32_ERR_TRAIN);
        break;
    case V32S_DATA:
        /* 7.2: an 1800 Hz tone for more than 128 symbol intervals is the
         * caller asking for a retrain */
        if (r->pure_run > 2 * 128 + 16) {
            m->retrains++; m->retraining = 1;
            m->seg_len[m->state] = m->seg;
            start(m);
            break;
        }
        if (!m->connected || m->retraining) break;
        /* line signal gone (below about -48 dBm0; the Recommendation sets
         * no level, 3.2 only ties circuit 109 to the sequences) for
         * CD_LOSS: the call is over */
        m->carrier = r->pw_raw * 3 > CD_OFF;
        if (m->carrier) m->lost_run = 0;
        else if (++m->lost_run > (int32_t)MS(CD_LOSS_MS)) { fail(m, V32_ERR_CARRIER); break; }
        /* reception too poor for this rate for RT_BAD: retrain (7.2), and R3
         * will name a rate the line can carry. Each good minute forgives one;
         * a fourth in a row gives the line up. */
        if (m->cfg.no_auto_retrain) break;
        if (r->mse > retrain_mse[m->rate]) {
            if (++m->bad_run > (int32_t)MS(RT_BAD_MS)) {
                if (m->auto_rt >= RT_MAX) { fail(m, V32_ERR_CARRIER); break; }
                m->auto_rt++; m->good_run = 0;
                v32_retrain(m);
            }
        } else {
            if (m->bad_run > 0) m->bad_run--;
            if (m->auto_rt > 0 && ++m->good_run > (int32_t)MS(60000)) { m->auto_rt--; m->good_run = 0; }
        }
        break;
    default:
        break;
    }
    if (!m->rx_ready && r->mode == V32_RX_DATA && r->clamp == 0) m->rx_ready = 1;
    if (m->tx_ready && m->rx_ready && m->state == V32S_DATA) {
        m->connected = 1; m->retraining = 0;
        m->bitrate = V32_BITRATE(m->rate);
    }
}

int16_t v32_tick(v32_t *m, int16_t rx)
{
    int16_t tx;
    int32_t e;
    if (m->state == V32S_FAILED) return 0;
    m->nsamp++; m->t_state++;
    if (v32tx_symbol_due(&m->tx)) tx_control(m);
    tx = v32tx_tick(&m->tx);
    e = v32ec_tick(&m->ec, m->tx.ref, rx, m->ec_mu);
    v32rx_tick(&m->rx, e);
    rx_control(m);
    return tx;
}
