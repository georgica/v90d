/* Whole V.34 calls: modem/v8ans, then modem/v34ans (as modem/answer.c is to
 * run them), against the calling test peer (sim/v34peer: V.8 CM without PCM,
 * Phase 2, Phases 3 and 4, data), over test/line34.h: G.711 mu-law / A-law,
 * loop filters, near and far echo, noise, clock and frequency offset.
 * Rates reached, bit errors both ways, the timings 11.2 and 11.3 give at the
 * hand-overs, retrain, rate renegotiation and cleardown from either side. */
#include "test.h"
#include "dxline.h"
#include "line34.h"
#include "../modem/v8ans.h"
#include "../modem/v8msg.h"
#include "../modem/v34ans.h"
#include "../sim/v34peer.h"

/* ---- what modem/answer.c has to do, in small ---- */
typedef struct {
    v8ans_t v8; v34ans_t m; v34ans_cfg cfg;
    int phase;                  /* 0 V.8, 1 V.34, 2 something else was selected */
    long n, v8_done_at;
} ans_t;

static void ans_init(ans_t *a, const v34ans_cfg *cfg)
{
    v8_menu_t local;
    memset(a, 0, sizeof *a);
    a->cfg = *cfg;
    memset(&local, 0, sizeof local);
    local.call_function = V8_CALLF_DATA;
    local.modes = V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21;
    local.n_mod_octets = 3;
    local.has_pcm = 1; local.pcm = V8_PCM_DIGITAL;      /* V.90 offered too; no V8_PCM_NO_V34_DATA */
    v8ans_init(&a->v8, 8000, (int16_t)(cfg->level * 181 / 128), &local);
}

static int16_t ans_tick(void *u, int16_t rx)
{
    ans_t *a = (ans_t *)u;
    int16_t tx = 0;
    a->n++;
    if (a->phase == 0) {
        if (v8ans_tick(&a->v8, rx, &tx) == V8A_DONE) {
            a->v8_done_at = a->n;
            if (a->v8.result == V8A_RES_OK && a->v8.selected == 1) { v34ans_init(&a->m, &a->cfg); a->phase = 1; }
            else a->phase = 2;
        }
        return tx;
    }
    return a->phase == 1 ? v34ans_tick(&a->m, rx) : 0;
}

static int16_t peer_tick(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

static ans_t A; static v34peer_t C; static line34_t L; static bend_t UA, UC;
static long gap_last, gap_first;    /* INFO1a's last loud sample, S's first */

static void call_init(const line34_cfg *lc, v34ans_cfg *ac, v34peer_cfg *pc)
{
    bend_init(&UA, &A.m.dx, 0xACE1, 0xBEEF1); bend_init(&UC, &C.dx, 0xBEEF1, 0xACE1);
    ac->getbit = bend_get; ac->putbit = bend_put; ac->user = &UA;
    pc->dx.getbit = bend_get; pc->dx.putbit = bend_put; pc->dx.user = &UC;
    ans_init(&A, ac); v34peer_init(&C, pc); line34_init(&L, lc);
    gap_last = gap_first = -1;
}

static void call_run(long samples)
{
    long i;
    for (i = 0; i < samples; i++) {
        int16_t atx = line34_step(&L, ans_tick, &A, peer_tick, &C);
        int loud = atx > A.cfg.level / 3 || atx < -A.cfg.level / 3;
        /* a retrain starts the engines afresh: the data sequences start afresh with them */
        if (A.m.dx.tx_starts == 0) UA.seen_tx = 0;
        if (A.m.dx.rx_starts == 0) UA.seen_rx = 0;
        if (C.dx.tx_starts == 0) UC.seen_tx = 0;
        if (C.dx.rx_starts == 0) UC.seen_rx = 0;
        if (A.phase == 1 && A.m.state == V34A_PHASE2 && A.m.p2.state >= P2N_INFO1A && A.m.retrains == 0 && loud) gap_last = L.n;
        if (A.phase == 1 && A.m.state == V34A_TRAINING && gap_first < 0 && loud) gap_first = L.n;
    }
}

static int call_to_data(int max_ms)
{
    long end = L.n + 8L * max_ms;
    while (L.n < end) {
        call_run(8);
        if (A.phase == 1 && A.m.state == V34A_DATA && C.state == VP_DATA) return 1;
        if (A.phase == 2 || (A.phase == 1 && A.m.state == V34A_ENDED) || C.state == VP_ENDED) return 0;
    }
    return 0;
}

static void sinks_reset(void) { UA.sink.n = UA.sink.errs = UC.sink.n = UC.sink.errs = 0; }

static void report(const char *what)
{
    printf("  %-30s %d sym/s  A->C %5d bit/s %7ld bits %3ld err   C->A %5d bit/s %7ld bits %3ld err  (data at %.1f s)\n",
           what, A.m.dx.cfg.sym_rate, A.m.dx.tx_rate, UC.sink.n, UC.sink.errs, A.m.dx.rx_rate, UA.sink.n, UA.sink.errs,
           (double)L.n / 8000.0);
}

/* a call that must reach data mode and carry `secs` seconds of data with at most max_errs per direction */
static void good_call(const char *what, const line34_cfg *lc, v34ans_cfg *ac, v34peer_cfg *pc, int secs, int min_rate, long max_errs)
{
    long t_data;
    call_init(lc, ac, pc);
    CHECK_EQ_I(call_to_data(25000), 1);
    t_data = L.n;
    sinks_reset();
    call_run(8000L * secs);
    CHECK_EQ_I(A.m.state, V34A_DATA); CHECK_EQ_I(C.state, VP_DATA);
    CHECK_EQ_I(v34ans_tx_rate(&A.m), C.dx.rx_rate); CHECK_EQ_I(v34ans_rx_rate(&A.m), C.dx.tx_rate);
    CHECK(v34ans_tx_rate(&A.m) >= min_rate); CHECK(v34ans_rx_rate(&A.m) >= min_rate);
    CHECK(UA.sink.n > (long)(secs - 1) * A.m.dx.rx_rate); CHECK(UC.sink.n > (long)(secs - 1) * A.m.dx.tx_rate);
    CHECK(UA.sink.errs <= max_errs); CHECK(UC.sink.errs <= max_errs);
    L.n = t_data; report(what); L.n = t_data + 8000L * secs;
}

static const double LOOP_MILD[3] = { 0.1, 0.8, 0.1 };              /* -4 dB at 3.4 kHz */
static const double LOOP_LONG[3] = { 0.25, 0.6, 0.25 };            /* -5 dB at 1.9 kHz, -13 dB at 3 kHz, -17 dB at 3.4 kHz */
static const double LOOP_HARD[5] = { 0.06, 0.22, 0.44, 0.22, 0.06 }; /* -9 dB at 1.9 kHz, -19 dB at 3.4 kHz */

int main(void)
{
    line34_cfg lc, base;
    v34ans_cfg ac; v34peer_cfg pc;
    int k;

    memset(&base, 0, sizeof base);
    base.law = 1; base.d_ca = 24; base.d_ac = 20; base.gain = 0.5; base.echo_a = 0.3; base.echo_c = 0.3;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.law = 1;

    /* ---- V.8: what a V.34-only caller and a V.90 caller look like to the same menu ---- */
    {
        v34peer_default(&pc, 3000);
        call_init(&base, &ac, &pc);
        while (L.n < 8000L * 12 && A.phase == 0) call_run(8);
        CHECK_EQ_I(A.phase, 1); CHECK_EQ_I(A.v8.result, V8A_RES_OK);
        CHECK_EQ_I(A.v8.selected, 1);                       /* V.34, bit 1 of the modulation modes */
        CHECK_EQ_I(A.v8.cm.has_pcm, 0); CHECK_EQ_I(A.v8.jm.has_pcm, 0);
        CHECK(A.v8.jm.modes & V8_MOD_V34);
        {
            v8_menu_t cm, local, jm;
            memset(&cm, 0, sizeof cm); memset(&local, 0, sizeof local);
            cm.call_function = local.call_function = V8_CALLF_DATA;
            cm.modes = V8_MOD_V34; cm.n_mod_octets = 3; cm.has_pcm = 1; cm.pcm = V8_PCM_ANALOG;
            local.modes = V8_MOD_V34 | V8_MOD_V22; local.n_mod_octets = 3; local.has_pcm = 1; local.pcm = V8_PCM_DIGITAL;
            v8_build_jm(&cm, &local, &jm);
            CHECK_EQ_I(v8_selected(&jm), V8_SEL_V90);       /* a V.90 caller: PCM in CM and JM */
            cm.has_pcm = 0; cm.pcm = 0;
            v8_build_jm(&cm, &local, &jm);
            CHECK_EQ_I(v8_selected(&jm), 1);                /* V.34 only */
            local.pcm = V8_PCM_DIGITAL | V8_PCM_NO_V34_DATA;    /* today's answer.c: V.34 withdrawn */
            v8_build_jm(&cm, &local, &jm);
            CHECK_EQ_I(v8_selected(&jm), 0);
        }
    }

    /* ---- the codecs, with the timings of the hand-overs ---- */
    for (k = 0; k < 3; k++) {
        static const char *name[3] = { "no codec (linear)", "G.711 mu-law", "G.711 A-law" };
        lc = base; lc.law = k; ac.law = k;
        v34peer_default(&pc, 3000);
        good_call(name[k], &lc, &ac, &pc, 5, k ? 24000 : 28800, 0);
        /* 11.2.1.2.1: INFO0a 75 +- 5 ms after JM ended: v8ans counts 75 ms, p2ans starts with the next sample */
        CHECK_EQ_I(A.m.p2.n_info0a >= 1, 1);
        /* 11.3.1.2.1: S 70 +- 5 ms after INFO1a, measured on what we sent */
        CHECK_NEAR((gap_first - gap_last) / 8.0, 70.0, 5.0);
        if (k == 1) printf("  silence between INFO1a and S: %.1f ms; round trip measured %d samples (true 44)\n",
                           (gap_first - gap_last) / 8.0, (int)A.m.p2.rtd);
        CHECK_NEAR(A.m.p2.rtd, 44, 10);
        CHECK_EQ_I(A.m.retrains, 0); CHECK_EQ_I(A.m.connects, 1);
        CHECK_EQ_I(A.m.dx.cfg.sym_rate, 3200);
    }
    ac.law = 1;

    /* ---- loops, echo, noise ---- */
    v34peer_default(&pc, 3000);
    lc = base; lc.loop = LOOP_MILD; lc.nloop = 3;
    good_call("mild loop", &lc, &ac, &pc, 5, 21600, 0);
    lc = base; lc.loop = LOOP_LONG; lc.nloop = 3;
    good_call("long loop", &lc, &ac, &pc, 5, 9600, 0);
    lc = base; lc.loop = LOOP_HARD; lc.nloop = 5;
    good_call("loop past anything real", &lc, &ac, &pc, 5, 4800, 2);
    lc = base; lc.echo_a = 0.8; lc.io_delay = 96;
    ac.io_delay = -1;
    good_call("strong echo, own delay unknown", &lc, &ac, &pc, 5, 21600, 0);
    CHECK(A.m.dx.ecd.delay >= 80 && A.m.dx.ecd.delay <= 97);
    lc = base; lc.d_ca = 200; lc.d_ac = 240; lc.far_a = 0.1; lc.io_delay = 96;
    good_call("far echo 55 ms late, same", &lc, &ac, &pc, 5, 21600, 0);
    CHECK_EQ_I(A.m.dx.far_on, 1);
    ac.io_delay = 0;
    for (k = 0; k < 3; k++) {
        static const int snr[3] = { 34, 28, 22 }, need[3] = { 19200, 14400, 9600 };
        char name[40];
        lc = base; lc.noise = 1500.0 / pow(10, snr[k] / 20.0);
        sprintf(name, "noise %d dB down", snr[k]);
        good_call(name, &lc, &ac, &pc, 8, need[k], 2);
    }

    /* ---- clock and frequency offset ---- */
    lc = base; lc.ppm = 100;
    good_call("caller's clock +100 ppm", &lc, &ac, &pc, 8, 24000, 0);
    lc = base; lc.ppm = -100;
    good_call("caller's clock -100 ppm", &lc, &ac, &pc, 8, 24000, 0);
    lc = base; lc.foff_ca = 1.5; lc.foff_ac = -1.5;
    good_call("frequency offset +-1.5 Hz", &lc, &ac, &pc, 8, 24000, 0);
    CHECK_NEAR(A.m.p2.info1a.freq_off * 0.02, 1.5, 0.3);

    /* ---- what the caller asks: INFO1c and MP ---- */
    {
        v34peer_default(&pc, 3000);
        pc.p2.preemph[4] = 6; pc.p2.min_pwr_red = 2; pc.p2.hi_carrier_mask = 0;
        pc.dx.ask_16pt = 1; pc.dx.ask_trellis = 2; pc.dx.ask_nonlinear = 1; pc.dx.ask_shaping = 1;
        pc.dx.ask_h[0][0] = 3277; pc.dx.ask_h[0][1] = -1638; pc.dx.ask_h[1][0] = -819;
        good_call("caller asks for everything", &base, &ac, &pc, 5, 24000, 0);
        CHECK_EQ_I(A.m.dx.cfg.tx_preemph, 6); CHECK_EQ_I(A.m.dx.cfg.tx_pwr_red, 2);
        CHECK_EQ_I(A.m.dx.cfg.tx_car_num * 7, A.m.dx.cfg.tx_car_den * 4);      /* the low carrier, 4/7 */
        CHECK_EQ_I(A.m.dx.far_16pt, 1); CHECK_EQ_I(A.m.dx.tpar.cfg.trellis, 2);
        CHECK_EQ_I(A.m.dx.tpar.cfg.nonlinear, 1); CHECK_EQ_I(A.m.dx.tpar.cfg.expanded, 1);
        CHECK_EQ_I(A.m.dx.tpar.cfg.h[0][0], 3277);
        /* symbol rates: the caller cannot do 3200; we allow 2400 only */
        v34peer_default(&pc, 3000); pc.p2.max_rate[4] = 0;
        good_call("caller without 3200", &base, &ac, &pc, 4, 19200, 0);
        CHECK_EQ_I(A.m.dx.cfg.sym_rate, 3000);
        v34peer_default(&pc, 3000); ac.sr_mask = P2N_SR_2400;
        good_call("2400 symbols/s only", &base, &ac, &pc, 4, 14400, 0);
        CHECK_EQ_I(A.m.dx.cfg.sym_rate, 2400);
        ac.sr_mask = 0;
    }

    /* ---- retrain (11.5), from either side, and when Phase 3 loses the caller ---- */
    for (k = 0; k < 3; k++) {
        static const char *name[3] = { "after our retrain", "after the caller's retrain", "caller lost in Phase 3" };
        long t0;
        v34peer_default(&pc, 3000);
        lc = base;
        call_init(&lc, &ac, &pc);
        if (k == 2) {
            /* the caller's signal is gone for 1.5 s from the start of our Phase 3 */
            while (L.n < 8000L * 25 && !(A.phase == 1 && A.m.state == V34A_TRAINING)) call_run(8);
            L.c.mute_ca_from = (int)L.n; L.c.mute_ca_to = (int)L.n + 12000;
            t0 = L.n;
        } else {
            CHECK_EQ_I(call_to_data(25000), 1);
            call_run(8000);
            t0 = L.n;
            if (k == 0) CHECK_EQ_I(v34ans_retrain(&A.m), 0); else CHECK_EQ_I(v34dx_retrain(&C.dx), 0);
            call_run(2400);
            CHECK(A.m.state != V34A_DATA);
        }
        CHECK_EQ_I(call_to_data(25000), 1);
        CHECK(A.m.retrains >= 1); CHECK(C.retrains >= 1);
        CHECK_EQ_I(A.m.retrains_ours, k == 1 ? 0 : A.m.retrains);
        CHECK_EQ_I(A.m.connects, k == 2 ? 1 : 2);
        printf("  retrain: data again %.1f s later;", (L.n - t0) / 8000.0);
        sinks_reset(); call_run(8000L * 4);
        CHECK_EQ_I(A.m.state, V34A_DATA);
        CHECK(UA.sink.n > 3L * A.m.dx.rx_rate); CHECK(UC.sink.n > 3L * A.m.dx.tx_rate);
        CHECK_EQ_I(UA.sink.errs, 0); CHECK_EQ_I(UC.sink.errs, 0);
        CHECK(A.m.dx.rx_rate >= 24000 && A.m.dx.tx_rate >= 24000);
        report(name[k]);
    }

    /* ---- rate renegotiation and cleardown through the top level ---- */
    {
        v34peer_default(&pc, 3000);
        call_init(&base, &ac, &pc);
        CHECK_EQ_I(v34ans_renegotiate(&A.m, 0), -1); CHECK_EQ_I(v34ans_cleardown(&A.m), -1);
        CHECK_EQ_I(call_to_data(25000), 1); call_run(8000);
        CHECK_EQ_I(v34ans_renegotiate(&A.m, 16800), 0);
        call_run(80); CHECK_EQ_I(A.m.state, V34A_RENEG); CHECK_EQ_I(v34ans_rx_rate(&A.m), 0);
        CHECK_EQ_I(call_to_data(3000), 1);
        CHECK_EQ_I(v34ans_rx_rate(&A.m), 16800); CHECK(v34ans_tx_rate(&A.m) >= 24000);
        CHECK_EQ_I(v34dx_renegotiate(&C.dx, 0), 0);
        call_run(800); CHECK_EQ_I(call_to_data(3000), 1);
        CHECK_EQ_I(A.m.dx.n_reneg, 2); CHECK_EQ_I(A.m.retrains, 0);
        sinks_reset(); call_run(8000L * 3);
        CHECK_EQ_I(UA.sink.errs, 0); CHECK_EQ_I(UC.sink.errs, 0);
        report("after two renegotiations");
        CHECK_EQ_I(v34ans_cleardown(&A.m), 0);
        call_run(8000);
        CHECK_EQ_I(A.m.state, V34A_ENDED); CHECK_EQ_I(A.m.error, V34A_ERR_HANGUP);
        CHECK_EQ_I(C.state, VP_ENDED); CHECK_EQ_I(C.dx.error, DX_ERR_CLEARDOWN);
        CHECK_EQ_I(ans_tick(&A, 0), 0);

        call_init(&base, &ac, &pc);
        CHECK_EQ_I(call_to_data(25000), 1); call_run(8000);
        CHECK_EQ_I(v34dx_cleardown(&C.dx), 0);
        call_run(8000);
        CHECK_EQ_I(A.m.state, V34A_ENDED); CHECK_EQ_I(A.m.error, V34A_ERR_CLEARDOWN);
    }

    /* ---- failures ---- */
    {
        /* the caller never starts Phase 2: INFO0c does not come */
        lc = base;
        v34peer_default(&pc, 3000);
        call_init(&lc, &ac, &pc);
        while (L.n < 8000L * 12 && A.phase == 0) call_run(8);
        L.c.mute_ca_from = 0; L.c.mute_ca_to = 0x7fffffff;
        call_run(8000L * 14);
        CHECK_EQ_I(A.m.state, V34A_ENDED); CHECK_EQ_I(A.m.error, V34A_ERR_PHASE2);
        CHECK_EQ_I(A.m.p2.error, P2N_ERR_INFO0);
        /* the caller goes away for good during training: retrains, then the end */
        call_init(&lc, &ac, &pc);
        while (L.n < 8000L * 25 && !(A.phase == 1 && A.m.state == V34A_TRAINING)) call_run(8);
        L.c.mute_ca_from = 0; L.c.mute_ca_to = 0x7fffffff;
        call_run(8000L * 40);
        CHECK_EQ_I(A.m.state, V34A_ENDED);
        CHECK(A.m.error == V34A_ERR_PHASE2 || A.m.error == V34A_ERR_RETRAINS);
    }
    T_REPORT("v34call");
}
