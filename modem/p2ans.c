/* V.34 Phase 2, answer modem. See p2ans.h and V.34 11.2.1.2. */
#include <string.h>
#include "p2ans.h"

#define MS(x) ((x) * 8)
#define TONE_SEEN 160           /* 20 ms of steady carrier = tone detected */
#define TURN 320                /* the 40 ms turn-round */
#define TAIL 80                 /* 10 ms of tone after a reversal */
#define PROBE_BLOCKS 23         /* 460 ms of L2 analysed (not to exceed 500 ms) */
#define TX_FLUSH 60             /* samples from the start of the last bit to its end on the line */
#define TX_DECAY 80             /* the shaped carrier-off ramp */
#define P2N_FLOOR_DB10 360      /* signal-dependent noise assumed: 36 dB below the signal */

/* V.34 Tables 1 and 2 */
static const int16_t SR[6] = { 2400, 2743, 2800, 3000, 3200, 3429 };
static const int8_t CAR[6][2][2] = {     /* [index][low / high] = { d, e } */
    { {2, 3}, {3, 4} }, { {3, 5}, {2, 3} }, { {3, 5}, {2, 3} },
    { {3, 5}, {2, 3} }, { {4, 7}, {3, 5} }, { {4, 7}, {4, 7} } };
/* highest data rate of Table 8 per symbol rate, in units of 2400 bit/s */
static const int8_t TOP[6] = { 9, 10, 10, 12, 13, 14 };

int v34_symbol_rate(int idx) { return idx >= 0 && idx <= 5 ? SR[idx] : 0; }

void v34_carrier(int idx, int hi, int *num, int *den)
{
    if (idx < 0 || idx > 5) idx = 0;
    *num = CAR[idx][hi ? 1 : 0][0]; *den = CAR[idx][hi ? 1 : 0][1];
}

static void go(p2ans_t *p, p2ans_state s) { p->state = s; p->t = 0; }

static void fail(p2ans_t *p, int why)
{
    p->error = why; dpsktx_off(&p->tx); p->probing = 0; go(p, P2N_FAILED);
}

static void send_info0a(p2ans_t *p, int ack)
{
    info0_t i;
    uint8_t bits[INFO_MAX_BITS];
    int n;
    memset(&i, 0, sizeof i);
    /* Table 14: no 2743 / 2800 / 3429; both carriers at 3000 and 3200; the
     * transmitter can reduce its power; one symbol rate for both directions;
     * up to 1664 points; internal transmit clock */
    i.lo3000 = i.hi3000 = i.lo3200 = i.hi3200 = 1;
    i.allow3429 = 0;
    i.pwr_reduce = 1;
    i.max_sr_diff = 0;
    i.c1664 = 1;
    i.ack = (uint8_t)ack;
    n = info0a_build(&i, bits);
    dpsktx_send(&p->tx, bits, n);
    p->tone_since = -1;
    p->n_info0a++;
}

static void common_init(p2ans_t *p, const p2ans_cfg *cfg)
{
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    if (!p->cfg.sr_mask) p->cfg.sr_mask = P2N_SR_2400 | P2N_SR_3000 | P2N_SR_3200;
    p->state = P2N_INFO0; p->error = P2N_OK;
    dpsktx_init(&p->tx, DPSK_FC_A, cfg->level, 1);
    dpskrx_init(&p->rx, DPSK_FC_B);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));   /* INFO0c: Table 14, 49 bits */
    probetx_init(&p->ptx, cfg->level);
    proberx_init(&p->prx);
    p->tone_since = -1;
}

void p2ans_init(p2ans_t *p, const p2ans_cfg *cfg)
{
    common_init(p, cfg);
    send_info0a(p, 0);
}

void p2ans_init_retrain(p2ans_t *p, const p2ans_cfg *cfg, const info0_t *info0c)
{
    common_init(p, cfg);
    p->info0c = *info0c;
    p->got0c = 1;
    dpsktx_on(&p->tx);
}

/* a retrain we begin ourselves (11.5.2.1): silence, then as the retrain entry */
static void self_retrain(p2ans_t *p, int why)
{
    if (p->retrains >= P2N_MAX_RETRAINS) { fail(p, why); return; }
    p->retrains++;
    dpsktx_off(&p->tx);
    p->probing = 0;
    go(p, P2N_RT_SILENCE);
}

/* back to 11.2.1.2.3: Tone A, and the reversal once Tone B is there */
static void restart_tones(p2ans_t *p)
{
    p->restarts++;
    p->probing = 0;
    dpsktx_on(&p->tx);
    dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
    p->tone_since = -1;
    go(p, P2N_INFO0);
}

/* 10^(d/100) for d in 0.1 dB, 0..600: 2^(d * 0.0332193) */
static uint32_t db10_lin(int d)
{
    static const uint16_t frac[9] = { 32768, 35734, 38968, 42495, 46341, 50535, 55109, 60097, 65535 };
    int32_t e = d * 2177;                   /* log2 in Q16: 0.0332193 * 65536 */
    int n = e >> 16, i = (e >> 13) & 7, f = e & 0x1fff;
    uint32_t m = (uint32_t)frac[i] + (uint32_t)(((int32_t)(frac[i + 1] - frac[i]) * f) >> 13);    /* 2^frac, Q15 */
    return n >= 15 ? m << (n - 15) : m >> (15 - n);
}

/* What a linear equaliser makes of the band lo..hi Hz, from the per-tone
 * SNRs: 1 / mean(1 / (1 + SNR)), in 0.1 dB. A tone half a spacing outside
 * the band still counts: it is the nearest witness of the band edge. The
 * probing tones stand 150 Hz apart and the analyser's noise band is 50 Hz,
 * so a signal that fills s Hz with the same power has 21 * 50 / s of the
 * per-tone SNR. */
static int band_snr(const p2ans_t *p, int fc, int s)
{
    int m, n = 0, corr;
    int64_t sum = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        if (!probe_has_tone(m) || 150 * m < fc - s / 2 - 75 || 150 * m > fc + s / 2 + 75) continue;
        sum += ((int64_t)1 << 40) / (1 + db10_lin(p->snr[m]));
        n++;
    }
    if (!n || sum < 1) return 0;
    corr = probe_db10(1050, s);
    return probe_db10((int64_t)n << 40, sum) + corr;
}

/* INFO1c is in: choose, and build INFO1a (Table 16). Returns 0, or -1 when
 * no symbol rate is left. */
static int decide(p2ans_t *p)
{
    int m, k, best = -1, best_score = -1, best_fine = -1, best_hi = 0, best_snr = 0, best_rate = 0;
    int cap = p->info0c.c1664 ? 14 : 12;    /* Table 16 note */

    int64_t ptot = 0;

    for (m = 1; m <= PROBE_NBIN; m++) {
        p->snr[m] = probe_has_tone(m) ? proberx_snr(&p->prx, m) : 0;
        p->rx_level[m] = proberx_level(&p->prx, m);
        if (probe_has_tone(m)) ptot += (int64_t)p->rx_level[m] * p->rx_level[m];
    }
    /* What the analyser cannot see: noise that follows the signal (the
     * quantisation of a G.711 codec, the residue of our own receiver)
     * repeats with the periodic probing signal and counts as tone. Taken as
     * P2N_FLOOR_DB10 below the whole received signal, spread evenly over
     * 4 kHz; in the analyser's 50 Hz that is 80 times less. On a line that
     * tilts, the weak tones are then as poor as they will be in data mode. */
    for (m = 1; m <= PROBE_NBIN; m++) {
        int lim;
        if (!probe_has_tone(m) || ptot <= 0) continue;
        lim = probe_db10((int64_t)p->rx_level[m] * p->rx_level[m] * 80, ptot) + P2N_FLOOR_DB10;
        if (lim < 0) lim = 0;
        if (p->snr[m] > lim) p->snr[m] = lim;
    }
    for (k = 4; k >= 0; k--) {
        int c, hi_ok[2], snr = -1, hi = 0, rate, top, fine, score;
        if (!((p->cfg.sr_mask >> k) & 1) || (k != 0 && k != 3 && k != 4)) continue;
        if (p->info1c.sr[k].max_rate == 0) continue;        /* "the symbol rate cannot be used" */
        /* which carriers the caller can transmit (INFO0c bits 15:18; both at 2400) */
        hi_ok[0] = k == 3 ? p->info0c.lo3000 : k == 4 ? p->info0c.lo3200 : 1;
        hi_ok[1] = k == 3 ? p->info0c.hi3000 : k == 4 ? p->info0c.hi3200 : 1;
        if (!hi_ok[0] && !hi_ok[1]) continue;
        for (c = 0; c < 2; c++) {
            int v;
            if (!hi_ok[c]) continue;
            v = band_snr(p, SR[k] * CAR[k][c][0] / CAR[k][c][1], SR[k]);
            /* the high carrier must be better by 1 dB to be chosen */
            if (snr < 0 || v > snr + 10) { snr = v; hi = c; }
        }
        /* 9 dB for the first bit, 3 dB for each further bit per symbol */
        fine = (int)((int64_t)(snr - 90) * SR[k] * 16 / (30 * 2400));
        rate = fine >> 4;
        top = TOP[k] < cap ? TOP[k] : cap;
        if (p->cfg.max_rate && top > p->cfg.max_rate / 2400) top = p->cfg.max_rate / 2400;
        if (rate > top) rate = top;
        if (rate < 1) rate = 1;
        /* throughput of the two directions together; on a tie the symbol
         * rate with more in hand (what the rate would be without the caps),
         * then the higher symbol rate */
        score = rate + p->info1c.sr[k].max_rate;
        if (score > best_score || (score == best_score && fine > best_fine)) {
            best_score = score; best_fine = fine;
            best = k; best_hi = hi; best_snr = snr; best_rate = rate;
        }
    }
    if (best < 0) return -1;

    p->sym_idx = best; p->sym_rate = SR[best];
    p->rx_hi_carrier = best_hi; p->rx_snr_db10 = best_snr;
    p->tx_hi_carrier = p->info1c.sr[best].hi_carrier;
    p->tx_preemph = p->info1c.sr[best].preemph > 10 ? 0 : p->info1c.sr[best].preemph;
    p->tx_pwr_red = p->info1c.min_pwr_red;
    p->md_len = p->info1c.md_len;

    memset(&p->info1a, 0, sizeof p->info1a);
    p->info1a.v90 = 0;
    p->info1a.min_pwr_red = 0; p->info1a.add_pwr_red = 0;
    p->info1a.md_len = 0;                           /* we send no MD */
    p->info1a.hi_carrier = (uint8_t)best_hi;
    p->info1a.preemph = 0;
    p->info1a.max_rate = (uint8_t)best_rate;
    p->info1a.up_sr = (uint8_t)best;                /* bits 34:36: answer -> call */
    p->info1a.down_sr = (uint8_t)best;              /* bits 37:39: call -> answer */
    p->info1a.freq_off = (int16_t)proberx_freq_off(&p->prx);
    return 0;
}

/* a tone (bin m) standing out of our own probing echo for two blocks running */
static int tone_under_probe(p2ans_t *p, int16_t x, int m)
{
    proberx_tick(&p->prx, x);
    if (p->prx.n != 0) return p->tone_blocks >= 2;
    {
        int lvl = proberx_last(&p->prx, m), ref = proberx_last(&p->prx, 6);
        if (lvl > 30 && lvl > 4 * ref) p->tone_blocks++; else p->tone_blocks = 0;
    }
    return p->tone_blocks >= 2;
}

int16_t p2ans_tick(p2ans_t *p, int16_t x)
{
    int ev = dpskrx_tick(&p->rx, x);
    int32_t out;

    switch (p->state) {
    case P2N_INFO0:
        if (ev & DPSK_EV_FRAME) {
            int again = p->got0c;
            info0a_parse(p->rx.frame, &p->info0c);
            p->got0c = 1;
            /* a repeated INFO0c without the acknowledge bit: the caller has not got ours */
            p->resend = (again && !p->info0c.ack);
        }
        if (!dpsktx_busy(&p->tx)) {
            if (p->tone_since < 0) p->tone_since = p->now;
            if (p->resend || (!p->got0c && p->rx.steady >= MS(50))) {
                send_info0a(p, p->got0c);           /* 11.2.2.2.1 */
                p->resend = 0;
            } else if (p->got0c && p->rx.steady >= TONE_SEEN && p->now - p->tone_since >= MS(50) + TX_FLUSH) {
                /* 11.2.1.2.3: Tone B is there, Tone A has run for 50 ms */
                dpsktx_reverse(&p->tx);
                p->a_rev_line = p->now + DPSK_TX_DELAY;
                go(p, P2N_WAIT_BREV1);
            }
        }
        if (p->t > MS(10000)) fail(p, p->got0c ? P2N_ERR_REV : P2N_ERR_INFO0);
        break;

    case P2N_WAIT_BREV1:
        if (ev & DPSK_EV_FRAME) {                   /* a repeated INFO0c */
            info0a_parse(p->rx.frame, &p->info0c);
            p->resend = !p->info0c.ack;
            p->tone_since = p->now;
            go(p, P2N_INFO0);
        } else if (ev & DPSK_EV_REVERSAL) {
            /* 11.2.1.2.4, .5: the round trip, and our reversal 40 ms after
             * the caller's was at the line terminals */
            int32_t line = p->now - DPSK_RX_REV_DELAY;
            p->rtd = line - p->a_rev_line - TURN - p->cfg.io_delay;
            if (p->rtd < 0) p->rtd = 0;
            p->act_at = line + TURN - DPSK_TX_DELAY - p->cfg.io_delay;
            go(p, P2N_REPLY);
        } else if (p->t > MS(2000)) {
            if (p->restarts >= 4) fail(p, P2N_ERR_REV);
            else restart_tones(p);                  /* 11.2.2.2.2 */
        }
        break;

    case P2N_REPLY:
        if (p->now == p->act_at || (p->t == 0 && p->now > p->act_at)) {
            dpsktx_reverse(&p->tx);
            p->act_at = p->now;
        }
        if (p->now == p->act_at + TAIL && p->t > 0) dpsktx_off(&p->tx);
        if (p->now == p->act_at + TAIL + DPSK_TX_DELAY && p->t > 0) {
            probetx_init(&p->ptx, p->cfg.level);
            proberx_init(&p->prx);
            p->tone_blocks = 0;
            p->probing = 1;
            go(p, P2N_L1);
        }
        break;

    case P2N_L1:
        (void)tone_under_probe(p, x, 8);
        if (p->t >= MS(160)) { p->probing = 2; p->tone_blocks = 0; go(p, P2N_L2); }
        break;

    case P2N_L2:
        if (tone_under_probe(p, x, 8)) {            /* 11.2.1.2.6 */
            p->probing = 0;
            p->tone_b_at = p->now;
            dpsktx_on(&p->tx);
            go(p, P2N_TONE50);
        } else if (p->t > MS(600) + p->rtd) {
            if (p->restarts >= 4) fail(p, P2N_ERR_REV);
            else restart_tones(p);                  /* 11.2.2.2.3 */
        }
        break;

    case P2N_TONE50:
        if (p->t == MS(50) + DPSK_TX_DELAY) { dpsktx_reverse(&p->tx); go(p, P2N_REV_TAIL); }
        break;

    case P2N_REV_TAIL:
        if (p->t == TAIL) { dpsktx_off(&p->tx); go(p, P2N_WAIT_BREV2); }
        break;

    case P2N_WAIT_BREV2:
        if (ev & DPSK_EV_REVERSAL) {
            /* 11.2.1.2.7, .8: Tone B runs 10 ms on, then L1 for 160 ms, then L2 */
            p->probe_from = p->now - DPSK_RX_REV_DELAY + TAIL + MS(160) + MS(10);
            proberx_init(&p->prx);
            go(p, P2N_PROBE_RX);
        } else if (p->now - p->tone_b_at > MS(2000) + 2 * p->rtd) {
            self_retrain(p, P2N_ERR_INFO1C);        /* 11.2.2.2.4 */
        }
        break;

    case P2N_PROBE_RX:
        if (p->now >= p->probe_from) {
            proberx_tick(&p->prx, x);
            if (p->prx.nblk >= PROBE_BLOCKS) {
                dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO1D_BITS));   /* INFO1c: Table 15, 109 bits */
                dpsktx_on(&p->tx);
                go(p, P2N_WAIT_INFO1C);
            }
        }
        break;

    case P2N_WAIT_INFO1C:
        if (ev & DPSK_EV_FRAME) {                   /* 11.2.1.2.9 */
            uint8_t bits[INFO_MAX_BITS];
            int n;
            info1d_parse(p->rx.frame, &p->info1c);
            if (decide(p) != 0) { fail(p, P2N_ERR_NO_RATE); break; }
            n = info1a_build(&p->info1a, bits);
            dpsktx_send(&p->tx, bits, n);
            go(p, P2N_INFO1A);
        } else if (p->now - p->tone_b_at > MS(2000) + 2 * p->rtd) {
            self_retrain(p, P2N_ERR_INFO1C);        /* 11.2.2.2.4 */
        }
        break;

    case P2N_INFO1A:
        if (dpsktx_busy(&p->tx)) p->t = 0;
        else if (p->t == TX_FLUSH) dpsktx_off(&p->tx);
        else if (p->t == TX_FLUSH + TX_DECAY) go(p, P2N_DONE);
        break;

    case P2N_RT_SILENCE:
        if (p->t >= MS(70)) {
            dpsktx_on(&p->tx);
            dpskrx_expect(&p->rx, INFO_PAYLOAD(INFO0A_BITS));
            p->tone_since = -1;
            p->restarts = 0;
            go(p, P2N_INFO0);
        }
        break;

    case P2N_DONE:
    case P2N_FAILED:
        break;
    }

    out = dpsktx_tick(&p->tx);
    if (p->probing) out += probetx_tick(&p->ptx, p->probing == 1);
    if (out > 32767) out = 32767;
    if (out < -32768) out = -32768;
    p->now++; p->t++;
    return (int16_t)out;
}
