/* V.92 in Phase 2 (V.92 8.4.1, 9.3): the capability bits of INFO0d and
 * INFO0a, INFO1d of Table 17, INFO1a of Table 18, and what the digital modem
 * makes of the caller's INFO1a: V.92 with PCM upstream, V.90, or V.34.
 *
 * The codec of modem/info.c is compared bit by bit with the one of
 * v92/v92_info.c, which was written separately from the same tables. */
#include <string.h>
#include "test.h"
#include "../modem/p2d.h"
#include "../sim/p2a.h"
#include "../sim/noise.h"
#include "../v92/v92_info.h"

#define MAXD 2048

static unsigned field(const uint8_t *b, int lo, int hi)
{
    unsigned v = 0; int k;
    for (k = lo; k <= hi; k++) v |= (unsigned)b[k] << (k - lo);
    return v;
}

typedef struct { p2d_t d; p2a_t a; int n; } run_t;

/* d_v92: 0 = a V.90 digital modem, 1 = V.92 offered with PCM upstream,
 * 2 = V.92 offered, bit 70 of INFO1d clear */
static void run(run_t *r, int d_v92, const p2a_cfg *ac, int retrain)
{
    static int16_t lad[MAXD], lda[MAXD];
    p2d_cfg dc; rng_t rng;
    int16_t dtx = 0, atx = 0;
    int i;
    memset(lad, 0, sizeof lad); memset(lda, 0, sizeof lda);
    memset(&dc, 0, sizeof dc);
    dc.tx_dbm0 = -12; dc.max_tx_code = 23; dc.up_sr_mask = (1 << 3) | (1 << 4); dc.up_hi_carrier = 1 << 4;
    if (retrain) {
        p2d_init_retrain(&r->d, &dc);
        if (d_v92) p2d_retrain_v92(&r->d, d_v92 == 1);
        p2a_init_retrain(&r->a, ac);
    } else {
        p2d_init(&r->d, &dc);
        if (d_v92) p2d_offer_v92(&r->d, d_v92 == 1);
        p2a_init(&r->a, ac);
    }
    rng_seed(&rng, 7);
    for (i = 0; i < 8000 * 8; i++) {
        double xd, xa;
        lad[i % MAXD] = atx; lda[i % MAXD] = dtx;
        xd = 0.5 * lad[(i - 20 + MAXD) % MAXD] + 0.1 * dtx + 2.0 * rng_gauss(&rng);
        xa = 0.5 * lda[(i - 24 + MAXD) % MAXD] + 0.1 * atx + 2.0 * rng_gauss(&rng);
        dtx = p2d_tick(&r->d, (int16_t)xd);
        atx = p2a_tick(&r->a, (int16_t)xa);
        if ((r->d.state == P2D_DONE || r->d.state == P2D_FAILED) &&
            (r->a.state == P2A_DONE || r->a.state == P2A_FAILED)) break;
    }
    r->n = i;
}

static void caller(p2a_cfg *ac, int v92, int pcm_up, int v90)
{
    memset(ac, 0, sizeof *ac);
    ac->level = 4000; ac->v90 = v90; ac->up_sr = -1; ac->uinfo = 91;
    ac->info0a.lo3000 = ac->info0a.hi3000 = 1; ac->info0a.lo3200 = ac->info0a.hi3200 = 1;
    ac->info0a.pwr_reduce = 1; ac->info0a.max_sr_diff = 5; ac->info0a.c1664 = 1;
    ac->info0a.v92 = (uint8_t)v92;
    ac->pcm_up = pcm_up; ac->sections = 2; ac->ltot_code = 1; ac->lmax_code = 3; ac->md_len = 0;
}

int main(void)
{
    uint8_t b[INFO_MAX_BITS], c[V92_INFO_MAX_BITS];
    static run_t r;
    p2a_cfg ac;
    int n;

    /* ---- INFO0d, Table 15: bit 26 short Phase 2, bit 27 V.92 ---- */
    {
        info0_t a, g; v92_info0_t w;
        memset(&a, 0, sizeof a);
        a.lo3000 = a.hi3200 = 1; a.max_sr_diff = 5; a.tx_power = 6; a.max_tx_power = 23; a.alaw = 1;
        a.v92 = 1;
        n = info0d_build(&a, b);
        CHECK_EQ_I(n, INFO0D_BITS);
        CHECK_EQ_I(b[26], 0); CHECK_EQ_I(b[27], 1);
        memset(&g, 0xff, sizeof g);
        info0d_parse(b + 12, &g);
        CHECK(memcmp(&a, &g, sizeof a) == 0);
        CHECK(info_check(b + 12, INFO_PAYLOAD(INFO0D_BITS)));
        /* the other codec agrees on every bit */
        memset(&w, 0, sizeof w);
        w.lo3000 = w.hi3200 = 1; w.max_sr_diff = 5; w.tx_power = 6; w.max_tx_power = 23; w.alaw = 1; w.v92 = 1;
        CHECK_EQ_I(v92_info0d_build(&w, c), INFO0D_BITS);
        CHECK(memcmp(b, c, INFO0D_BITS) == 0);
        a.v92 = 0; a.short_p2 = 1;
        info0d_build(&a, b);
        CHECK_EQ_I(b[26], 1); CHECK_EQ_I(b[27], 0);
        /* a V.90 INFO0d is what it was: both bits zero */
        a.short_p2 = 0;
        info0d_build(&a, b);
        CHECK_EQ_I(field(b, 26, 27), 0);
    }

    /* ---- INFO0a, Table 16: bit 26 V.92, bit 27 short Phase 2 ---- */
    {
        info0_t a, g; v92_info0_t w;
        memset(&a, 0, sizeof a);
        a.sr2800 = 1; a.lo3200 = 1; a.max_sr_diff = 2; a.c1664 = 1; a.v92 = 1; a.ack = 1;
        n = info0a_build(&a, b);
        CHECK_EQ_I(n, INFO0A_BITS);
        CHECK_EQ_I(b[26], 1); CHECK_EQ_I(b[27], 0); CHECK_EQ_I(b[28], 1);
        memset(&g, 0xff, sizeof g);
        info0a_parse(b + 12, &g);
        CHECK_EQ_I(g.v92, 1); CHECK_EQ_I(g.short_p2, 0); CHECK_EQ_I(g.ack, 1); CHECK_EQ_I(g.sr2800, 1);
        memset(&w, 0, sizeof w);
        w.sr2800 = 1; w.lo3200 = 1; w.max_sr_diff = 2; w.c1664 = 1; w.v92 = 1; w.ack = 1;
        CHECK_EQ_I(v92_info0a_build(&w, c), INFO0A_BITS);
        CHECK(memcmp(b, c, INFO0A_BITS) == 0);
    }

    /* ---- INFO1d, Table 17: bit 70, and 71:78 for 3429 ---- */
    {
        info1d_t a, g; v92_info1d_t w; int k;
        memset(&a, 0, sizeof a);
        a.min_pwr_red = 1; a.add_pwr_red = 2; a.md_len = 5; a.freq_off = -9;
        for (k = 0; k < 6; k++) { a.sr[k].hi_carrier = (uint8_t)(k & 1); a.sr[k].preemph = (uint8_t)(k + 1); a.sr[k].max_rate = (uint8_t)(12 - k); }
        n = info1d_build_v92(&a, 1, b);
        CHECK_EQ_I(n, INFO1D_BITS);
        CHECK_EQ_I(b[70], 1); CHECK_EQ_I(field(b, 71, 74), 6); CHECK_EQ_I(field(b, 75, 78), 7);
        CHECK(info_check(b + 12, INFO_PAYLOAD(INFO1D_BITS)));
        CHECK_EQ_I(field(b, 105, 108), 15);
        memset(&w, 0, sizeof w);
        w.min_pwr_red = 1; w.add_pwr_red = 2; w.md_len = 5; w.freq_off = -9; w.pcm_up = 1;
        for (k = 0; k < 6; k++) { w.sr[k].hi_carrier = (uint8_t)(k & 1); w.sr[k].preemph = (uint8_t)(k + 1); w.sr[k].max_rate = (uint8_t)(12 - k); }
        CHECK_EQ_I(v92_info1d_build(&w, c), INFO1D_BITS);
        CHECK(memcmp(b, c, INFO1D_BITS) == 0);
        n = info1d_build_v92(&a, 0, b);
        CHECK_EQ_I(b[70], 0);
        CHECK(info_check(b + 12, INFO_PAYLOAD(INFO1D_BITS)));
        info1d_parse(b + 12, &g);
        CHECK_EQ_I(g.sr[5].hi_carrier, 0); CHECK_EQ_I(g.sr[5].max_rate, 7); CHECK_EQ_I(g.sr[4].hi_carrier, 0);
        CHECK_EQ_I(g.sr[3].hi_carrier, 1); CHECK_EQ_I(g.freq_off, -9);
    }

    /* ---- INFO1a, Table 18 ---- */
    {
        info1a_t a, g; v92_info1a_t w;
        memset(&a, 0, sizeof a);
        a.v90 = 1; a.pcm = 1; a.sections = 3; a.ltot_code = 2; a.lmax_code = 1; a.md_len = 4; a.uinfo = 80; a.up_sr = 6;
        n = info1a_build(&a, b);
        CHECK_EQ_I(n, INFO1A_BITS);
        CHECK_EQ_I(field(b, 12, 13), 3); CHECK_EQ_I(field(b, 14, 15), 2); CHECK_EQ_I(field(b, 16, 17), 1);
        CHECK_EQ_I(field(b, 18, 24), 4); CHECK_EQ_I(field(b, 25, 31), 80); CHECK_EQ_I(field(b, 32, 33), 0);
        CHECK_EQ_I(field(b, 34, 36), 6); CHECK_EQ_I(field(b, 37, 39), 6); CHECK_EQ_I(field(b, 40, 49), 0x3ff);
        CHECK(info_check(b + 12, INFO_PAYLOAD(INFO1A_BITS)));
        memset(&g, 0xff, sizeof g);
        info1a_parse(b + 12, &g);
        CHECK_EQ_I(info1a_kind(&g), INFO1A_PCM);
        CHECK_EQ_I(g.sections, 3); CHECK_EQ_I(g.ltot_code, 2); CHECK_EQ_I(g.lmax_code, 1);
        CHECK_EQ_I(g.md_len, 4); CHECK_EQ_I(g.uinfo, 80); CHECK_EQ_I(g.up_sr, 6);
        memset(&w, 0, sizeof w);
        w.kind = V92_INFO1A_PCM; w.sections = 3; w.ltot_code = 2; w.lmax_code = 1; w.md_len = 4; w.uinfo = 80;
        CHECK_EQ_I(v92_info1a_build(&w, c), INFO1A_BITS);
        CHECK(memcmp(b, c, INFO1A_BITS) == 0);
        /* the V.90 and V.34 forms are told apart as before */
        memset(&a, 0, sizeof a);
        a.v90 = 1; a.md_len = 9; a.uinfo = 93; a.up_sr = 4; a.freq_off = 211;
        info1a_build(&a, b); memset(&g, 0xff, sizeof g); info1a_parse(b + 12, &g);
        CHECK_EQ_I(info1a_kind(&g), INFO1A_V90); CHECK_EQ_I(g.pcm, 0); CHECK_EQ_I(g.up_sr, 4);
        CHECK_EQ_I(g.sections, 0); CHECK_EQ_I(g.freq_off, 211);
        memset(&a, 0, sizeof a);
        a.v90 = 0; a.up_sr = 5; a.down_sr = 3; a.max_rate = 11;
        info1a_build(&a, b); memset(&g, 0xff, sizeof g); info1a_parse(b + 12, &g);
        CHECK_EQ_I(info1a_kind(&g), INFO1A_V34); CHECK_EQ_I(g.pcm, 0);
    }

    /* ---- the procedure (9.3): digital modem against the test caller ---- */

    /* V.92 at both ends, PCM upstream selected */
    caller(&ac, 1, 1, 1);
    run(&r, 1, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.a.state, P2A_DONE);
    CHECK_EQ_I(r.d.mode, P2D_MODE_V92);
    CHECK_EQ_I(r.a.info0d.v92, 1); CHECK_EQ_I(r.d.info0a.v92, 1);
    CHECK_EQ_I(r.a.info1d.sr[5].hi_carrier, 1);         /* bit 70 as the caller saw it */
    CHECK_EQ_I(r.d.info1a.pcm, 1); CHECK_EQ_I(r.d.info1a.uinfo, 91);
    CHECK_EQ_I(r.d.info1a.sections, 2); CHECK_EQ_I(r.d.info1a.ltot_code, 1); CHECK_EQ_I(r.d.info1a.lmax_code, 3);
    CHECK_NEAR(r.d.rtd, 44, 6);
    printf("  V.92 / V.92, PCM upstream: mode %d after %.2f s, rtd %d\n", r.d.mode, r.n / 8000.0, (int)r.d.rtd);

    /* V.92 at both ends, the caller prefers V.34 upstream: V.90 Phase 3 */
    caller(&ac, 1, 0, 1);
    run(&r, 1, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V90);
    CHECK_EQ_I(r.a.info1d.sr[5].hi_carrier, 1);         /* still the Table 17 form */
    CHECK(r.d.info1a.up_sr == 3 || r.d.info1a.up_sr == 4);

    /* V.92 at both ends, the caller selects V.34 */
    caller(&ac, 1, 0, 0);
    run(&r, 1, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V34);

    /* we say the channel does not support PCM upstream: the caller falls back */
    caller(&ac, 1, 1, 1);
    run(&r, 2, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V90);
    CHECK_EQ_I(r.a.info0d.v92, 1); CHECK_EQ_I(r.a.info1d.sr[5].hi_carrier, 0);

    /* a V.90 caller at a V.92 digital modem: INFO1d of V.90, V.90 Phase 3 */
    caller(&ac, 0, 0, 1);
    run(&r, 1, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V90);
    CHECK_EQ_I(r.d.info0a.v92, 0);
    CHECK_EQ_I(r.a.info1d.sr[5].hi_carrier, 0);         /* bit 70 is the 3429 carrier bit: not offered */
    /* a V.34 caller at a V.92 digital modem */
    caller(&ac, 0, 0, 0);
    run(&r, 1, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V34);

    /* a V.92 caller at a V.90 digital modem: no V.92 bit, V.90 as before */
    caller(&ac, 1, 1, 1);
    run(&r, 0, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V90);
    CHECK_EQ_I(r.a.info0d.v92, 0); CHECK_EQ_I(r.d.info1a.pcm, 0);

    /* a caller that sends Table 18 where it "shall not" (bit 70 clear; no V.92 here) */
    caller(&ac, 1, 1, 1); ac.pcm_force = 1;
    run(&r, 2, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_FAILED); CHECK_EQ_I(r.d.error, P2D_ERR_PCM_UP);
    run(&r, 0, &ac, 0);
    CHECK_EQ_I(r.d.state, P2D_FAILED); CHECK_EQ_I(r.d.error, P2D_ERR_PCM_UP);

    /* the Phase 2 of a retrain (no INFO0): V.92 again when the owner says so */
    caller(&ac, 1, 1, 1);
    run(&r, 1, &ac, 1);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V92); CHECK_EQ_I(r.d.n_info0d, 0);
    run(&r, 0, &ac, 1);
    CHECK_EQ_I(r.d.state, P2D_DONE); CHECK_EQ_I(r.d.mode, P2D_MODE_V90);

    T_REPORT("p2_v92");
}
