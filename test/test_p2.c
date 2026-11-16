/* Phase 2 end to end: the digital modem against the analogue test peer over
 * a delayed, attenuated, noisy, echoing 4-wire/2-wire path. */
#include <string.h>
#include "test.h"
#include "../modem/p2d.h"
#include "../sim/p2a.h"
#include "../sim/noise.h"

#define MAXD 2048

typedef struct {
    int d_ad, d_da;         /* one-way delays, samples */
    double g_ad, g_da;      /* path gains */
    double echo_d, echo_a;  /* each side's own transmit leaking into its receiver */
    double noise;           /* RMS at each receiver */
    int mute_ad_until;      /* analogue -> digital path dead until this sample */
    int mute_da_until;
    int v90, up_sr;
} scen_t;

typedef struct {
    p2d_t d; p2a_t a;
    int n;                  /* samples run */
    int a_done_at, d_done_at;
} run_t;

static void run(const scen_t *s, run_t *r, const p2d_cfg *dc)
{
    static int16_t lad[MAXD], lda[MAXD];
    p2a_cfg ac; rng_t rng;
    int16_t dtx = 0, atx = 0;
    int i;

    memset(lad, 0, sizeof lad); memset(lda, 0, sizeof lda);
    memset(&ac, 0, sizeof ac);
    ac.level = 4000; ac.v90 = s->v90; ac.up_sr = s->up_sr; ac.uinfo = 91;
    ac.info0a.sr3429 = 1; ac.info0a.lo3000 = ac.info0a.hi3000 = 1;
    ac.info0a.lo3200 = ac.info0a.hi3200 = 1; ac.info0a.allow3429 = 1;
    ac.info0a.pwr_reduce = 1; ac.info0a.max_sr_diff = 5; ac.info0a.c1664 = 1;
    p2d_init(&r->d, dc);
    p2a_init(&r->a, &ac);
    rng_seed(&rng, 2024);
    r->a_done_at = r->d_done_at = -1;
    for (i = 0; i < 8000 * 8; i++) {
        double xd, xa;
        lad[i % MAXD] = atx; lda[i % MAXD] = dtx;
        xd = s->g_ad * lad[(i - s->d_ad + MAXD) % MAXD];
        xa = s->g_da * lda[(i - s->d_da + MAXD) % MAXD];
        if (i < s->mute_ad_until) xd = 0;
        if (i < s->mute_da_until) xa = 0;
        xd += s->echo_d * dtx + s->noise * rng_gauss(&rng);
        xa += s->echo_a * atx + s->noise * rng_gauss(&rng);
        dtx = p2d_tick(&r->d, (int16_t)xd);
        atx = p2a_tick(&r->a, (int16_t)xa);
        if (r->d.state == P2D_DONE && r->d_done_at < 0) r->d_done_at = i;
        if (r->a.state == P2A_DONE && r->a_done_at < 0) r->a_done_at = i;
        if ((r->d.state == P2D_DONE || r->d.state == P2D_FAILED) &&
            (r->a.state == P2A_DONE || r->a.state == P2A_FAILED)) break;
    }
    r->n = i;
}

int main(void)
{
    static run_t r;
    p2d_cfg dc;
    scen_t s;
    int m;

    CHECK_NEAR(p2_level(-6), 15770 * 0.5012, 60);
    CHECK_NEAR(p2_level(-12), 15770 * 0.2512, 40);

    memset(&dc, 0, sizeof dc);
    dc.tx_dbm0 = -12; dc.max_tx_code = 23; dc.up_sr_mask = (1 << 3) | (1 << 4);
    dc.up_hi_carrier = 1 << 4;

    /* ---- clean short line ---- */
    memset(&s, 0, sizeof s);
    s.d_ad = 20; s.d_da = 24; s.g_ad = 0.5; s.g_da = 0.5; s.noise = 2.0; s.v90 = 1; s.up_sr = -1;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_DONE);
    CHECK_EQ_I(r.a.state, P2A_DONE);
    CHECK_EQ_I(r.d.error, 0);
    CHECK(r.n < 8000 * 4);
    CHECK_EQ_I(r.d.n_info0d, 1); CHECK_EQ_I(r.a.n_info0a, 1);
    /* capabilities crossed over intact */
    CHECK_EQ_I(r.d.info0a.sr3429, 1); CHECK_EQ_I(r.d.info0a.max_sr_diff, 5);
    CHECK_EQ_I(r.d.info0a.c1664, 1); CHECK_EQ_I(r.d.info0a.sr2743, 0);
    CHECK_EQ_I(r.a.info0d.tx_power, 6);         /* -12 dBm0 */
    CHECK_EQ_I(r.a.info0d.max_tx_power, 23);
    CHECK_EQ_I(r.a.info0d.alaw, 0);
    CHECK_EQ_I(r.a.info0d.lo3200, 1);
    /* round-trip delay seen from both ends, within 1 ms (the other end's 40 ms turn-round) */
    CHECK_NEAR(r.d.rtd, 44, 8);
    CHECK_NEAR(r.a.rtd, 44, 8);
    /* INFO1d: only the offered symbol rates are usable; a clean line projects a high rate */
    CHECK_EQ_I(r.a.info1d.sr[0].max_rate, 0); CHECK_EQ_I(r.a.info1d.sr[1].max_rate, 0);
    CHECK_EQ_I(r.a.info1d.sr[2].max_rate, 0); CHECK_EQ_I(r.a.info1d.sr[5].max_rate, 0);
    CHECK(r.a.info1d.sr[3].max_rate >= 10); CHECK(r.a.info1d.sr[4].max_rate >= 10);
    CHECK(r.a.info1d.sr[4].max_rate <= 14);
    CHECK_EQ_I(r.a.info1d.sr[4].hi_carrier, 1); CHECK_EQ_I(r.a.info1d.sr[3].hi_carrier, 0);
    CHECK_EQ_I(r.a.info1d.md_len, 0);
    CHECK_NEAR(r.a.info1d.freq_off, 0, 3);
    /* INFO1a back: V.90, the peer's choice of upstream rate and of the 2-point train */
    CHECK_EQ_I(r.d.info1a.v90, 1);
    CHECK_EQ_I(r.d.info1a.uinfo, 91);
    CHECK_EQ_I(r.d.info1a.up_sr, 4);
    /* probing saw the line: L2 tones at about peer level - 6 dB per tone / sqrt(21), high SNR */
    for (m = 2; m <= 22; m++) {
        if (!probe_has_tone(m)) continue;
        CHECK_NEAR(r.d.rx_level[m], 4000 * 0.5 / sqrt(21.0), 4000 * 0.5 / sqrt(21.0) * 0.1);
        CHECK(r.d.snr[m] > 300);
    }

    /* ---- long, lossy, noisy line with echo both sides ---- */
    memset(&s, 0, sizeof s);
    s.d_ad = 400; s.d_da = 440; s.g_ad = 0.06; s.g_da = 0.08; s.noise = 6.0;
    s.echo_d = 0.2; s.echo_a = 0.2; s.v90 = 1; s.up_sr = 3;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_DONE);
    CHECK_EQ_I(r.a.state, P2A_DONE);
    CHECK_NEAR(r.d.rtd, 840, 8);
    CHECK_NEAR(r.a.rtd, 840, 8);
    CHECK_EQ_I(r.d.info1a.up_sr, 3);
    /* per-tone SNR: tone RMS 240/sqrt(21) = 52, noise in a 50 Hz bin 6*sqrt(1/80) = 0.67 -> 37.8 dB */
    for (m = 2; m <= 22; m++) if (probe_has_tone(m)) CHECK_NEAR(r.d.snr[m] / 10.0, 37.8, 4.0);
    /* a noisier line projects a lower rate */
    {
        int clean = r.a.info1d.sr[4].max_rate;
        s.noise = 40.0;
        run(&s, &r, &dc);
        CHECK_EQ_I(r.d.state, P2D_DONE);
        CHECK(r.a.info1d.sr[4].max_rate < clean);
        CHECK(r.a.info1d.sr[4].max_rate >= 2);
    }

    /* ---- the analogue modem asks for V.34 instead ---- */
    memset(&s, 0, sizeof s);
    s.d_ad = 20; s.d_da = 24; s.g_ad = 0.5; s.g_da = 0.5; s.noise = 2.0; s.v90 = 0; s.up_sr = 4;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_DONE);
    CHECK_EQ_I(r.d.info1a.v90, 0);

    /* ---- recovery: INFO0a lost (9.2.1.2.1) ---- */
    s.v90 = 1; s.mute_ad_until = 900;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_DONE);
    CHECK_EQ_I(r.a.state, P2A_DONE);
    CHECK_EQ_I(r.d.info0a.sr3429, 1);
    CHECK_NEAR(r.d.rtd, 44, 8);
    CHECK(r.d.n_info0d >= 2);       /* the recovery branch really ran */
    CHECK(r.a.n_info0a >= 2);
    /* ---- recovery: INFO0d lost (the peer asks again) ---- */
    s.mute_ad_until = 0; s.mute_da_until = 900;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_DONE);
    CHECK_EQ_I(r.a.state, P2A_DONE);
    CHECK_EQ_I(r.a.info0d.tx_power, 6);
    CHECK(r.a.n_info0a >= 2);
    CHECK(r.d.n_info0d >= 2);

    /* ---- dead line: gives up with a reason ---- */
    s.mute_da_until = 0; s.g_ad = 0.0;
    run(&s, &r, &dc);
    CHECK_EQ_I(r.d.state, P2D_FAILED);
    CHECK_EQ_I(r.d.error, P2D_ERR_INFO0);

    T_REPORT("p2");
}
