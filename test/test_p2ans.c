/* V.34 Phase 2, answer modem (modem/p2ans) against the call-side test peer
 * (sim/p2c) over a delayed, attenuated, noisy, echoing, band-limited path:
 * INFO contents, the timings 11.2.1.2 gives, the symbol rate decision in
 * INFO1a, the recovery procedures of 11.2.2.2 and the retrain entry. */
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/p2ans.h"
#include "../sim/p2c.h"
#include "../sim/noise.h"

#define MAXD 2048
#define NLP 41
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    int d_ca, d_ac;         /* one-way delays, samples: caller -> answer, answer -> caller */
    double g_ca, g_ac;      /* path gains */
    double echo_a, echo_c;  /* each side's own transmit leaking into its receiver */
    double noise;           /* RMS at each receiver */
    double lp_hz;           /* low-pass on the caller -> answer path, 0 = none */
    int mute_ca_from, mute_ca_to;   /* caller -> answer path dead in this window (samples) */
    int mute_ac_from, mute_ac_to;
    int io_tx, io_rx;       /* our own sample path: delay to the line terminals and back */
    int retrain;            /* both ends start in the retrain entry */
    int seconds;
} scen_t;

typedef struct {
    p2ans_t a; p2c_t c;
    int n, a_done_at, c_done_at;
    int l1_samples, l2_samples;
    double l1_pw, l2_pw;
    int c_restarts;         /* times the harness played the caller's side of a retrain */
} run_t;

static double lp[NLP];
static void lp_design(double fc)
{
    int k; double s = 0;
    for (k = 0; k < NLP; k++) {
        double t = k - (NLP - 1) / 2.0, w = 0.54 - 0.46 * cos(2 * M_PI * k / (NLP - 1));
        double h = fabs(t) < 1e-9 ? 2 * fc / 8000.0 : sin(2 * M_PI * fc / 8000.0 * t) / (M_PI * t);
        lp[k] = h * w; s += lp[k];
    }
    for (k = 0; k < NLP; k++) lp[k] /= s;
}

static void run(const scen_t *s, run_t *r, const p2ans_cfg *ac, const p2c_cfg *cc)
{
    static int16_t lca[MAXD], lac[MAXD], ltx[MAXD]; static double lrx[MAXD];
    rng_t rng;
    int16_t atx = 0, ctx = 0;
    int i, k, c_fail_at = -1, secs = s->seconds ? s->seconds : 8;

    memset(lca, 0, sizeof lca); memset(lac, 0, sizeof lac);
    memset(ltx, 0, sizeof ltx); memset(lrx, 0, sizeof lrx);
    if (s->lp_hz > 0) lp_design(s->lp_hz);
    memset(r, 0, sizeof *r);
    if (s->retrain) {
        info0_t i0 = cc->info0c;
        p2ans_init_retrain(&r->a, ac, &i0);
        p2c_init_retrain(&r->c, cc);
    } else {
        p2ans_init(&r->a, ac);
        p2c_init(&r->c, cc);
    }
    rng_seed(&rng, 2024);
    r->a_done_at = r->c_done_at = -1;
    for (i = 0; i < 8000 * secs; i++) {
        double xa, xc, line_a;
        /* our transmit sample reaches the line terminals io_tx samples late */
        ltx[i % MAXD] = atx;
        line_a = ltx[(i - s->io_tx + MAXD) % MAXD];
        lca[i % MAXD] = ctx; lac[i % MAXD] = (int16_t)line_a;
        if (s->lp_hz > 0) {
            xa = 0;
            for (k = 0; k < NLP; k++) xa += lp[k] * lca[(i - s->d_ca - k + 2 * MAXD) % MAXD];
            xa *= s->g_ca;
        } else {
            xa = s->g_ca * lca[(i - s->d_ca + MAXD) % MAXD];
        }
        xc = s->g_ac * lac[(i - s->d_ac + MAXD) % MAXD];
        if (i >= s->mute_ca_from && i < s->mute_ca_to) xa = 0;
        if (i >= s->mute_ac_from && i < s->mute_ac_to) xc = 0;
        xa += s->echo_a * line_a + s->noise * rng_gauss(&rng);
        xc += s->echo_c * ctx + s->noise * rng_gauss(&rng);
        /* and what the line terminals receive reaches us io_rx samples late */
        lrx[i % MAXD] = xa;
        if (r->a.state == P2N_L1) { r->l1_samples++; r->l1_pw += (double)atx * atx; }
        if (r->a.state == P2N_L2) { r->l2_samples++; r->l2_pw += (double)atx * atx; }
        atx = p2ans_tick(&r->a, (int16_t)lrx[(i - s->io_rx + MAXD) % MAXD]);
        ctx = p2c_tick(&r->c, (int16_t)xc);
        /* the caller's side of a retrain (11.5.1.2), which the peer leaves to
         * the test: it gave up, so 70 ms of silence and Tone B */
        if (r->c.state == P2C_FAILED && c_fail_at < 0 && r->c_restarts < 3) c_fail_at = i;
        if (c_fail_at >= 0 && i == c_fail_at + 560) { p2c_init_retrain(&r->c, cc); r->c_restarts++; c_fail_at = -1; }
        if (r->a.state == P2N_DONE && r->a_done_at < 0) r->a_done_at = i;
        if (r->c.state == P2C_DONE && r->c_done_at < 0) r->c_done_at = i;
        if (r->a.state == P2N_FAILED) break;
        if (r->a.state == P2N_DONE && r->c.state == P2C_DONE) break;
    }
    r->n = i;
}

static void caller_cfg(p2c_cfg *cc)
{
    int k;
    memset(cc, 0, sizeof *cc);
    cc->level = 4000;
    cc->info0c.sr3429 = 1; cc->info0c.lo3000 = cc->info0c.hi3000 = 1;
    cc->info0c.lo3200 = cc->info0c.hi3200 = 1; cc->info0c.allow3429 = 1;
    cc->info0c.pwr_reduce = 1; cc->info0c.max_sr_diff = 5; cc->info0c.c1664 = 1;
    cc->hi_carrier_mask = 1 << 4;                   /* high carrier at 3200 only */
    for (k = 0; k < 6; k++) { cc->preemph[k] = k + 1; cc->max_rate[k] = -1; }
    cc->min_pwr_red = 2; cc->add_pwr_red = 3; cc->md_len = 0;
}

int main(void)
{
    static run_t r;
    p2ans_cfg ac; p2c_cfg cc;
    scen_t s;
    int m, num, den, clean_rate;

    /* Tables 1 and 2 */
    CHECK_EQ_I(v34_symbol_rate(0), 2400); CHECK_EQ_I(v34_symbol_rate(3), 3000);
    CHECK_EQ_I(v34_symbol_rate(4), 3200); CHECK_EQ_I(v34_symbol_rate(6), 0);
    v34_carrier(0, 0, &num, &den); CHECK_EQ_I(2400 * num / den, 1600);
    v34_carrier(0, 1, &num, &den); CHECK_EQ_I(2400 * num / den, 1800);
    v34_carrier(3, 0, &num, &den); CHECK_EQ_I(3000 * num / den, 1800);
    v34_carrier(3, 1, &num, &den); CHECK_EQ_I(3000 * num / den, 2000);
    v34_carrier(4, 0, &num, &den); CHECK_EQ_I(3200 * num / den, 1828);
    v34_carrier(4, 1, &num, &den); CHECK_EQ_I(3200 * num / den, 1920);

    memset(&ac, 0, sizeof ac);
    ac.level = 3000;
    caller_cfg(&cc);

    /* ---- clean short line ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
    run(&s, &r, &ac, &cc);
    printf("  p2ans clean: done at %.2f s (peer %.2f s), rtd %d / %d, 3200 SNR %.1f dB, projected %d\n",
           r.a_done_at / 8000.0, r.c_done_at / 8000.0, (int)r.a.rtd, (int)r.c.rtd,
           r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.error, 0);
    CHECK(r.n < 8000 * 4);
    CHECK_EQ_I(r.a.n_info0a, 1); CHECK_EQ_I(r.c.n_info0c, 1);
    CHECK_EQ_I(r.a.retrains, 0); CHECK_EQ_I(r.a.restarts, 0);
    /* INFO0a as the caller received it: what this modem can do (Table 14) */
    CHECK_EQ_I(r.c.info0a.sr2743, 0); CHECK_EQ_I(r.c.info0a.sr2800, 0); CHECK_EQ_I(r.c.info0a.sr3429, 0);
    CHECK_EQ_I(r.c.info0a.lo3000, 1); CHECK_EQ_I(r.c.info0a.hi3000, 1);
    CHECK_EQ_I(r.c.info0a.lo3200, 1); CHECK_EQ_I(r.c.info0a.hi3200, 1);
    CHECK_EQ_I(r.c.info0a.allow3429, 0); CHECK_EQ_I(r.c.info0a.pwr_reduce, 1);
    CHECK_EQ_I(r.c.info0a.max_sr_diff, 0); CHECK_EQ_I(r.c.info0a.cme, 0); CHECK_EQ_I(r.c.info0a.c1664, 1);
    /* INFO0c as we received it */
    CHECK_EQ_I(r.a.info0c.sr3429, 1); CHECK_EQ_I(r.a.info0c.max_sr_diff, 5);
    CHECK_EQ_I(r.a.info0c.c1664, 1); CHECK_EQ_I(r.a.info0c.sr2743, 0);
    /* round-trip delay at both ends within 1 ms: ours checks the caller's
     * 40 ms turn-round, the caller's checks OURS (11.2.1.2.5: 40 +- 1 ms) */
    CHECK_NEAR(r.a.rtd, 44, 8);
    CHECK_NEAR(r.c.rtd, 44, 8);
    /* 11.2.1.2.6: Tone A for 50 ms, then the reversal (as the caller saw it) */
    CHECK_NEAR(r.c.t_rev3 - r.c.t_tone_a2, 400, 56);
    /* L1 for 160 ms at 6 dB above L2; L2 no longer than 550 ms + a round trip */
    CHECK_EQ_I(r.l1_samples, 1280);
    CHECK(r.l2_samples > 800 && r.l2_samples <= 4400 + 44);
    CHECK_NEAR(10 * log10((r.l1_pw / r.l1_samples) / (r.l2_pw / r.l2_samples)), 6.02, 0.3);
    CHECK_NEAR(sqrt(r.l2_pw / r.l2_samples), 3000, 150);       /* L2 at the nominal power */
    /* INFO1a as the caller received it (Table 16): V.34, 3200 both ways */
    CHECK_EQ_I(r.c.info1a.v90, 0);
    CHECK_EQ_I(r.c.info1a.up_sr, 4); CHECK_EQ_I(r.c.info1a.down_sr, 4);
    CHECK_EQ_I(r.c.info1a.md_len, 0);
    CHECK_EQ_I(r.c.info1a.min_pwr_red, 0); CHECK_EQ_I(r.c.info1a.add_pwr_red, 0);
    CHECK_EQ_I(r.c.info1a.preemph, 0);
    CHECK(r.c.info1a.max_rate >= 12 && r.c.info1a.max_rate <= 13);  /* 3200 symbols/s: 31200 at most */
    CHECK_NEAR(r.c.info1a.freq_off, 0, 3);
    CHECK_EQ_I(r.c.info1a.hi_carrier, r.a.rx_hi_carrier);
    clean_rate = r.c.info1a.max_rate;
    /* the outcome on our side: what INFO1c asked of our transmitter at 3200 */
    CHECK_EQ_I(r.a.sym_idx, 4); CHECK_EQ_I(r.a.sym_rate, 3200);
    CHECK_EQ_I(r.a.tx_hi_carrier, 1); CHECK_EQ_I(r.a.tx_preemph, 5); CHECK_EQ_I(r.a.tx_pwr_red, 2);
    CHECK_EQ_I(r.a.md_len, 0);
    /* probing saw the line: L2 tones at about peer level / sqrt(21), high SNR */
    for (m = 2; m <= 22; m++) {
        if (!probe_has_tone(m)) continue;
        CHECK_NEAR(r.a.rx_level[m], 4000 * 0.5 / sqrt(21.0), 4000 * 0.5 / sqrt(21.0) * 0.1);
        CHECK(r.a.snr[m] > 300);
    }
    /* signal 2000 RMS, noise 2 RMS over 4 kHz: 60 dB, 61 dB in 3200 Hz */
    CHECK_NEAR(r.a.rx_snr_db10 / 10.0, 37.0, 1.5);     /* the floor assumed for what follows the signal */

    /* ---- the symbol rate decision ---- */
    /* the caller cannot use 3200 from us: 3000, and its parameters for 3000 */
    cc.max_rate[4] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 3000); CHECK_EQ_I(r.c.info1a.up_sr, 3); CHECK_EQ_I(r.c.info1a.down_sr, 3);
    CHECK_EQ_I(r.a.tx_hi_carrier, 0); CHECK_EQ_I(r.a.tx_preemph, 4);
    CHECK(r.c.info1a.max_rate >= 11 && r.c.info1a.max_rate <= 12);  /* 28800 at most at 3000 */
    /* only 2400 left */
    cc.max_rate[3] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 2400); CHECK_EQ_I(r.c.info1a.up_sr, 0); CHECK_EQ_I(r.c.info1a.down_sr, 0);
    CHECK_EQ_I(r.a.tx_preemph, 1);
    CHECK_EQ_I(r.c.info1a.max_rate, 9);                             /* 21600 at most at 2400 */
    /* nothing we implement: Phase 2 fails with the reason */
    cc.max_rate[0] = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_FAILED); CHECK_EQ_I(r.a.error, P2N_ERR_NO_RATE);
    caller_cfg(&cc);
    /* our own configuration can exclude rates */
    ac.sr_mask = P2N_SR_3000;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3000);
    ac.sr_mask = 0;
    /* a ceiling on the projected rate */
    ac.max_rate = 19200;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.info1a.max_rate, 8);
    ac.max_rate = 0;
    /* the caller cannot do 1664 points: nothing above 28800 is projected */
    cc.info0c.c1664 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.c.info1a.max_rate, 12);
    cc.info0c.c1664 = 1;
    /* a caller that cannot transmit the high carrier at 3200 is not asked to */
    cc.info0c.hi3200 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.a.rx_hi_carrier, 0);
    cc.info0c.hi3200 = 1; cc.info0c.lo3200 = 0;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.a.sym_rate, 3200); CHECK_EQ_I(r.a.rx_hi_carrier, 1);
    caller_cfg(&cc);

    /* a line that ends at 2.9 kHz: the probing must send us down to 2400.
     * The caller projects the same throughput at every rate, so it is our
     * measurement that decides. */
    cc.max_rate[0] = cc.max_rate[3] = cc.max_rate[4] = 9;
    s.lp_hz = 2900; s.noise = 4.0;
    run(&s, &r, &ac, &cc);
    printf("  p2ans 2.9 kHz line: %d symbols/s, carrier %s, SNR %.1f dB, projected %d\n", r.a.sym_rate,
           r.a.rx_hi_carrier ? "high" : "low", r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 2400);
    CHECK_EQ_I(r.a.rx_hi_carrier, 0);       /* 400 .. 2800 Hz fits, 600 .. 3000 Hz does not */
    CHECK(r.c.info1a.max_rate >= 7);
    /* and one that ends at 3.2 kHz: 3000 symbols/s on the low carrier (300 .. 3300 Hz) */
    s.lp_hz = 3200;
    run(&s, &r, &ac, &cc);
    printf("  p2ans 3.2 kHz line: %d symbols/s, carrier %s, SNR %.1f dB, projected %d\n", r.a.sym_rate,
           r.a.rx_hi_carrier ? "high" : "low", r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE);
    CHECK_EQ_I(r.a.sym_rate, 3000); CHECK_EQ_I(r.a.rx_hi_carrier, 0);
    caller_cfg(&cc);

    /* ---- long, lossy, noisy line with echo both sides ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 400; s.d_ac = 440; s.g_ca = 0.06; s.g_ac = 0.08; s.noise = 6.0;
    s.echo_a = 0.2; s.echo_c = 0.2;
    run(&s, &r, &ac, &cc);
    printf("  p2ans long line: rtd %d / %d, SNR %.1f dB, projected %d\n", (int)r.a.rtd, (int)r.c.rtd,
           r.a.rx_snr_db10 / 10.0, r.a.info1a.max_rate * 2400);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK_NEAR(r.c.rtd, 840, 8);
    CHECK(r.l2_samples <= 4400 + 840);
    /* signal 240 RMS, noise 6 RMS over 4 kHz: 32 dB, 33 dB in 3200 Hz */
    CHECK_NEAR(r.a.rx_snr_db10 / 10.0, 33.0, 3.0);
    CHECK(r.c.info1a.max_rate < clean_rate);
    CHECK(r.c.info1a.max_rate >= 7 && r.c.info1a.max_rate <= 10);
    /* our own sample path is slow (a driver that buffers): told so, the
     * modem still turns round in 40 ms at the line and measures the line */
    s.io_tx = 90; s.io_rx = 70; ac.io_delay = 160;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK_NEAR(r.c.rtd, 840, 8);
    s.io_tx = s.io_rx = 0; ac.io_delay = 0;

    /* ---- recovery ---- */
    memset(&s, 0, sizeof s);
    s.d_ca = 20; s.d_ac = 24; s.g_ca = 0.5; s.g_ac = 0.5; s.noise = 2.0;
    /* INFO0c lost (11.2.2.2.1): Tone B is there, INFO0c is not */
    s.mute_ca_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.info0c.sr3429, 1);
    CHECK_NEAR(r.a.rtd, 44, 8);
    CHECK(r.a.n_info0a >= 2); CHECK(r.c.n_info0c >= 2);
    /* INFO0a lost: the caller asks again */
    s.mute_ca_to = 0; s.mute_ac_to = 900;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.c.info0a.c1664, 1);
    CHECK(r.a.n_info0a >= 2); CHECK(r.c.n_info0c >= 2);
    s.mute_ac_to = 0;
    /* our first Tone A reversal lost (11.2.2.2.2): no Tone B reversal comes
     * back within 2000 ms, so again from 11.2.1.2.3 */
    {
        scen_t s2 = s; run_t probe;
        /* find when the first reversal is on the line, then repeat the call with
         * the path dead around it */
        run(&s2, &probe, &ac, &cc);
        s2.mute_ac_from = probe.c.t_rev1 - 200; s2.mute_ac_to = probe.c.t_rev1 + 400;
        run(&s2, &r, &ac, &cc);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK(r.a.restarts >= 1);
        CHECK_NEAR(r.a.rtd, 44, 8); CHECK_NEAR(r.c.rtd, 44, 8);
        /* INFO1c lost (11.2.2.2.4): 2000 ms + 2 round trips after Tone B we
         * begin a retrain (11.5.2.1): 70 ms of silence, Tone A, and Phase 2
         * again from the reversals */
        s2 = s;
        s2.mute_ca_from = probe.c_done_at - 3200; s2.mute_ca_to = probe.c_done_at + 800;
        s2.seconds = 12;
        run(&s2, &r, &ac, &cc);
        printf("  p2ans INFO1c lost: done at %.2f s after %d retrain(s)\n", r.a_done_at / 8000.0, r.a.retrains);
        CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
        CHECK_EQ_I(r.a.retrains, 1);
        CHECK_EQ_I(r.a.sym_rate, 3200);
        CHECK_NEAR(r.a.rtd, 44, 8);
    }

    /* ---- the retrain entry (11.5.2): no INFO0, straight to the tones ---- */
    s.retrain = 1;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_DONE); CHECK_EQ_I(r.c.state, P2C_DONE);
    CHECK_EQ_I(r.a.n_info0a, 0); CHECK_EQ_I(r.c.n_info0c, 0);
    CHECK_NEAR(r.a.rtd, 44, 8); CHECK_NEAR(r.c.rtd, 44, 8);
    CHECK_EQ_I(r.a.sym_rate, 3200);
    CHECK_EQ_I(r.a.info0c.c1664, 1);        /* remembered from the first Phase 2 */
    s.retrain = 0;

    /* ---- dead line: gives up with a reason ---- */
    s.g_ca = 0.0; s.seconds = 30;
    run(&s, &r, &ac, &cc);
    CHECK_EQ_I(r.a.state, P2N_FAILED);
    CHECK_EQ_I(r.a.error, P2N_ERR_INFO0);
    CHECK(r.n < 8000 * 12);

    T_REPORT("p2ans");
}
