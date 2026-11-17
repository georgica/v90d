/* A V.92 call with PCM upstream in simulation: the digital modem
 * (v92/v92_p34d, production code) against the simulated analogue modem
 * (sim/v92a) through sim/v92chan: line shapes, noise, mu-law and A-law
 * codec, digital pads, hybrid echo. Phase 3 to data mode in every scenario;
 * the last ones start with Phase 2 (modem/p2d against sim/p2a), also with a
 * V.90 caller, which must go on through the V.90 Phases 3 and 4 as before. */
#include <string.h>
#include "test.h"
#include "../v92/v92_p34d.h"
#include "../sim/v92a.h"
#include "../sim/v92chan.h"
#include "../modem/p2d.h"
#include "../modem/p34d.h"
#include "../sim/p2a.h"
#include "../sim/p34a.h"
#include "../sim/noise.h"
#include "../v90/v90_pcm.h"

#define MAXD 2048

typedef struct { uint32_t s; long n, bad; } prbs_t;
typedef struct { prbs_t src, sink; } io_t;
static int prbs_next(prbs_t *p) { p->s = p->s * 1103515245u + 12345u; return (int)((p->s >> 16) & 1); }
static int  src_bit(void *u) { prbs_t *p = &((io_t *)u)->src; p->n++; return prbs_next(p); }
static void sink_bit(void *u, int bit) { prbs_t *p = &((io_t *)u)->sink; p->n++; if (prbs_next(p) != (bit & 1)) p->bad++; }

typedef struct {
    const char *name;
    int alaw, shape, dil_n;
    double level, noise, echo, up_pad, phase0, gain_err, echo_tail, wander;
    int echo_delay, d_down, d_up;
    int seconds;
    int max_up;
    uint32_t up_rates;
    int a_reneg_ms, d_reneg_ms, a_clear_ms, d_clear_ms, tone_a_ms, d_retrain_ms;
    int uinfo, ltot, lmax;      /* from Phase 2, 0 = defaults */
    int mute_ms;                /* the caller's signal stops for good */
    int echo_search;            /* the echo delay is not given to the digital modem */
    int pace;                   /* samples per step of the CPd design (0: one) */
    int suvd_wait, cpd_wait;    /* the caller's patience in Phase 4, symbols (0: any) */
    int first_cpd_plain;        /* the caller answers a first CPd with the acknowledge bit by silence */
    int b1u_lead, z2_dead, conv_start, ja_no_ones, ntrn1, jd_wait;      /* the caller's ways, see ../sim/v92a.h */
} scen_t;

typedef struct {
    int state, error, d_data_at, a_tx, a_rx, up_rate, down_rate, renegs, d_end_at, req_rc;
    long up_bits, up_bad, down_bits, down_bad;
    long up_bits2, up_bad2;     /* upstream after the last (re)start */
    int peak;                   /* where the channel peak ended up, 1/256 sample */
    double power;
    long ticks;
} out_t;

static v92d_t d;
static v92a_t a;

static void run(const scen_t *s, out_t *o)
{
    static v92chan_t ch;
    static uint8_t dl_o[MAXD];
    static io_t dio, aio;
    v92d_cfg dc; v92a_cfg ac; v92chan_cfg cc;
    uint8_t oct = v90_pcm_octet(s->alaw, 0, 1);
    int16_t rx = 0;
    int i, k, a_starts = 0, d_rx = 0;
    long sink0_n = 0, sink0_bad = 0;

    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac); memset(&cc, 0, sizeof cc);
    for (k = 0; k < MAXD; k++) dl_o[k] = oct;
    memset(&dio, 0, sizeof dio); memset(&aio, 0, sizeof aio);
    dio.src.s = 11; aio.sink.s = 11; aio.src.s = 23; dio.sink.s = 23;

    cc.alaw = s->alaw; cc.shape = s->shape; cc.level = s->level; cc.noise = s->noise;
    cc.echo_gain = s->echo; cc.echo_delay = s->echo_delay; cc.echo_tail = s->echo_tail; cc.wander = s->wander; cc.wander_hz = 25.0; cc.wander_from = 40000; cc.up_pad_db = s->up_pad; cc.up_delay = s->d_up; cc.seed = 5;
    v92chan_init(&ch, &cc);

    dc.alaw = s->alaw; dc.uinfo = s->uinfo ? s->uinfo : 91; dc.md_len = 0;
    dc.ltot = s->ltot ? s->ltot : 256; dc.lmax = s->lmax ? s->lmax : 192;
    dc.rtd = s->d_down + s->d_up; dc.echo_delay = s->echo_search ? -1 : s->echo_delay; dc.dsn_pace = s->pace;
    dc.max_up_rate = s->max_up;
    dc.getbit = src_bit; dc.putbit = sink_bit; dc.user = &dio;
    CHECK_EQ_I(v92d_init(&d, &dc), 0);

    ac.alaw = s->alaw; ac.uinfo = dc.uinfo; ac.sil0 = 560; ac.ntrn1 = 2040; ac.ntrn1b = 2040; ac.jd_wait = 100;
    ac.dil_n = s->dil_n; ac.dil_syms = 1500; ac.trn2u_min = s->suvd_wait ? 1000000 : 12000;
    ac.suvd_wait = s->suvd_wait; ac.cpd_wait = s->cpd_wait; ac.first_cpd_plain = s->first_cpd_plain;
    ac.up_rates = s->up_rates ? s->up_rates : 0x7FFFF;
    ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41; ac.sr_d = 0; ac.ld_d = 0;
    ac.gain_err = s->gain_err;
    ac.b1u_lead = s->b1u_lead; ac.conv_start = s->conv_start; ac.ja_no_ones = s->ja_no_ones;
    if (s->ntrn1) ac.ntrn1 = s->ntrn1;
    if (s->jd_wait) ac.jd_wait = s->jd_wait;
    ac.z2_dead = s->z2_dead;
    ac.getbit = src_bit; ac.putbit = sink_bit; ac.user = &aio;
    v92a_init(&a, &ac);
    a.phase = s->phase0;

    memset(o, 0, sizeof *o); o->d_data_at = -1;
    for (i = 0; i < s->seconds * 8000; i++) {
        double amp, frac;
        int valid, ms = i / 8, ev = (i % 8) == 0;
        if (s->tone_a_ms && ms >= s->tone_a_ms && ms < s->tone_a_ms + 120)      /* the caller's Tone A */
            rx = (int16_t)(3000.0 * sin(2 * 3.14159265358979 * 2400.0 * i / 8000.0));
        oct = v92d_tick(&d, rx);
        dl_o[i % MAXD] = oct;
        valid = v92a_tick(&a, dl_o[(i - s->d_down + MAXD) % MAXD], &amp, &frac);
        if (s->mute_ms && ms >= s->mute_ms) valid = 0;
        rx = v92chan_tick(&ch, oct, valid, amp, frac);
        if (o->d_data_at < 0 && d.state == V92D_DATA) { o->d_data_at = i; o->peak = v92_trn_peak(&d.trn); }
        if (ev && s->a_reneg_ms && ms == s->a_reneg_ms) o->req_rc = v92a_renegotiate(&a, 0, 0);
        if (ev && s->a_clear_ms && ms == s->a_clear_ms) o->req_rc = v92a_renegotiate(&a, 0, 1);
        if (ev && s->d_reneg_ms && ms == s->d_reneg_ms) o->req_rc = v92d_renegotiate(&d);
        if (ev && s->d_clear_ms && ms == s->d_clear_ms) o->req_rc = v92d_cleardown(&d);
        if (ev && s->d_retrain_ms && ms == s->d_retrain_ms) o->req_rc = v92d_retrain(&d);
        /* the upstream data stream restarts with the modems' data pumps */
        if (a.data_starts != a_starts) { a_starts = a.data_starts; aio.src.s = 23; }
        if (d.rx_ready && !d_rx) { dio.sink.s = 23; sink0_n = dio.sink.n; sink0_bad = dio.sink.bad; }
        d_rx = d.rx_ready;
        if (d.state == V92D_RETRAIN || d.state == V92D_FAILED) { o->d_end_at = i; break; }
    }
    o->ticks = i;
    o->up_bits2 = dio.sink.n - sink0_n; o->up_bad2 = dio.sink.bad - sink0_bad;
    o->state = d.state; o->error = d.error; o->a_tx = a.tx_ready; o->a_rx = a.rx_ready;
    o->up_rate = d.up_rate; o->down_rate = d.down_rate; o->renegs = d.renegs;
    o->up_bits = dio.sink.n; o->up_bad = dio.sink.bad; o->down_bits = aio.sink.n; o->down_bad = aio.sink.bad;
    o->power = a.pw_n ? a.pw_acc / (double)a.pw_n : 0;
}

static void report(const scen_t *s, const out_t *o)
{
    printf("  %-34s %s shape %d noise %-3.0f: state %d err %d, data after %d ms, up %d bit/s (LC %d, K %d, 4G %u, %d taps, delay %d), "
           "down %d, up %ld bits %ld bad, down %ld bits %ld bad, tx power %.2f, peak at %+.2f T\n",
           s->name, s->alaw ? "A-law " : "mu-law", s->shape, s->noise, o->state, o->error, o->d_data_at / 8, o->up_rate,
           d.cpd.map.lc[0], d.cpd.map.K, d.cpd.filt.gain4, d.cpd.filt.lz2 + d.cpd.filt.lp1, d.dsn.dly,
           o->down_rate, o->up_bits, o->up_bad, o->down_bits, o->down_bad, o->power, (double)(int8_t)(o->peak & 255) / 256.0);
    if (getenv("V92_VERBOSE"))
        printf("      noise floor %.1f (quantiser %.1f), echo %u, training residual %.1f (quantiser %.1f), rho %.2f, ISI %.1f dB, "
               "x rms max %u, points %d, decoder noise %.1f, bad frames %u, design steps %d\n",
               sqrt((double)d.noise), sqrt((double)d.q_quiet), d.dsn.cfg.echo_rms, sqrt((double)d.dsn.cfg.res), sqrt((double)d.dsn.cfg.q_trn),
               (double)d.dsn.rho_q8 / 256.0, 10 * log10((double)d.dsn.isi_q28 / 268435456.0 + 1e-12), d.dsn.xrms_max, d.dsn.ncand,
               sqrt((double)v92_dec_quality(&d.dec)), d.dec.bad_frames, d.dsn.steps);
}

/* the start-up went by the book and data flows both ways without errors */
static void check_good(const scen_t *s, const out_t *o)
{
    CHECK_EQ_I(o->state, V92D_DATA); CHECK_EQ_I(o->error, 0);
    CHECK(o->a_tx); CHECK(o->a_rx);
    /* what the analogue modem saw of our signals */
    CHECK(a.trn1d_n >= 2040 - 48); CHECK_EQ_I(a.trn1d_bad, 0); CHECK_EQ_I(a.sign_bad, 0);
    CHECK(a.jd_ok >= 1); CHECK(a.jp_ok >= 1); CHECK(a.jpp_seen);
    if (s->dil_n) { CHECK(a.dil_n_ok > 50); CHECK_EQ_I(a.dil_bad, 0); }
    else { CHECK(a.scr_n > 2000); CHECK_EQ_I(a.scr_bad, 0); }
    CHECK(a.ri_n >= 48); CHECK_EQ_I(a.ri_bad, 0); CHECK(a.ribar_seen);
    CHECK(a.trn2d_frames >= 340); CHECK_EQ_I(a.trn2d_bad, 0);
    CHECK(a.suvd_n >= 2); CHECK(a.cpd_n >= 1); CHECK(a.suvd_ack || a.cpd_ack);
    CHECK_EQ_I(a.seq_bad, 0); CHECK_EQ_I(a.dec_bad, 0);
    CHECK_EQ_I(a.b1d_bad, 0); CHECK_EQ_I(d.b1_bad, 0);
    /* the phase asked for in Jp put the peak of the channel on a sampling instant */
    CHECK(abs((int8_t)(o->peak & 255)) < 56);
    /* rates */
    CHECK_EQ_I(o->down_rate, (32 + 6) * 8000 / 6);
    CHECK(o->up_rate >= 24000 && o->up_rate <= 48000); CHECK_EQ_I(o->up_rate, a.up_rate);
    if (s->max_up) CHECK(o->up_rate <= s->max_up);
    CHECK(o->power <= 1.1);
    /* data both ways */
    CHECK(o->down_bits > 3 * 8000 * 38 / 6); CHECK_EQ_I(o->down_bad, 0);
    CHECK(o->up_bits > 3 * 24000); CHECK_EQ_I(o->up_bad, 0);
    CHECK_EQ_I(d.dec.bad_frames, 0);
}

static void base(scen_t *s, const char *name, int alaw, int shape, double noise, int dil_n)
{
    memset(s, 0, sizeof *s);
    s->name = name; s->alaw = alaw; s->shape = shape; s->noise = noise; s->dil_n = dil_n;
    s->level = 4000; s->echo = 0.05; s->echo_delay = 3; s->d_down = 20; s->d_up = 24; s->seconds = 9;
    s->phase0 = 0.3;
}

/* ---- Phase 2 in front ---- */

typedef struct { p2d_t d; p2a_t a; } p2_t;

static void phase2(p2_t *r, int caller_v92)
{
    static int16_t lad[MAXD], lda[MAXD];
    p2d_cfg dc; p2a_cfg ac; rng_t rng;
    int16_t dtx = 0, atx = 0;
    int i;
    memset(lad, 0, sizeof lad); memset(lda, 0, sizeof lda);
    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac);
    dc.tx_dbm0 = -12; dc.max_tx_code = 23; dc.up_sr_mask = (1 << 3) | (1 << 4); dc.up_hi_carrier = 1 << 4;
    p2d_init(&r->d, &dc);
    p2d_offer_v92(&r->d, 1);                /* a V.92 digital modem, PCM upstream offered */
    ac.level = 4000; ac.v90 = 1; ac.up_sr = -1; ac.uinfo = 91;
    ac.info0a.lo3000 = ac.info0a.hi3000 = 1; ac.info0a.lo3200 = ac.info0a.hi3200 = 1;
    ac.info0a.pwr_reduce = 1; ac.info0a.max_sr_diff = 5; ac.info0a.c1664 = 1;
    ac.info0a.v92 = (uint8_t)caller_v92;
    ac.pcm_up = caller_v92; ac.sections = 2; ac.ltot_code = 1; ac.lmax_code = 1; ac.md_len = 0;
    p2a_init(&r->a, &ac);
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
}

/* a V.90 caller after Phase 2: V.90 Phases 3 and 4 (as test_p34.c) */
static void v90_call(const p2_t *r)
{
    static p34d_t pd; static p34a_t pa;
    static uint8_t dl_o[MAXD]; static double dl_u[MAXD]; static int16_t dl_e[MAXD];
    static io_t dio, aio;
    p34d_cfg dc; p34a_cfg ac;
    int sr = r->d.info1a.up_sr, hi = sr == 4, i, k;
    uint8_t oct = v90_pcm_octet(0, 0, 1);
    memset(&dc, 0, sizeof dc); memset(&ac, 0, sizeof ac);
    memset(dl_u, 0, sizeof dl_u); memset(dl_e, 0, sizeof dl_e);
    for (k = 0; k < MAXD; k++) dl_o[k] = oct;
    memset(&dio, 0, sizeof dio); memset(&aio, 0, sizeof aio);
    dio.src.s = 11; aio.sink.s = 11; aio.src.s = 23; dio.sink.s = 23;
    dc.uinfo = r->d.info1a.uinfo; dc.md_len = r->d.info1a.md_len; dc.rtd = 44; dc.echo_delay = 0;
    if (sr == 3) { dc.sym_rate = 3000; dc.car_num = hi ? 2 : 3; dc.car_den = hi ? 3 : 5; }
    else { dc.sym_rate = 3200; dc.car_num = hi ? 3 : 4; dc.car_den = hi ? 5 : 7; }
    dc.getbit = src_bit; dc.putbit = sink_bit; dc.user = &dio;
    CHECK_EQ_I(p34d_init(&pd, &dc), 0);
    ac.uinfo = dc.uinfo; ac.sym_rate = dc.sym_rate; ac.fc = (double)dc.sym_rate * dc.car_num / dc.car_den;
    ac.gain = 2000; ac.sil0 = 224; ac.ntrn = 600; ac.jd_wait = 100; ac.dil_syms = 1500;
    ac.Kt = 12; ac.Mt = 4; ac.Kd = 32; ac.Md = 41; ac.up_rates = 0x1FFF;
    ac.getbit = src_bit; ac.putbit = sink_bit; ac.user = &aio;
    p34a_init(&pa, &ac);
    for (i = 0; i < 8 * 8000; i++) {
        double rx = dl_u[(i - 24 + MAXD) % MAXD] + 0.05 * dl_e[(i - 9 + MAXD) % MAXD];
        oct = p34d_tick(&pd, (int16_t)floor(rx + 0.5));
        dl_o[i % MAXD] = oct; dl_e[i % MAXD] = (int16_t)v90_octet_linear(0, oct);
        dl_u[i % MAXD] = p34a_tick(&pa, dl_o[(i - 20 + MAXD) % MAXD]);
        if (pd.state == P34_RETRAIN || pd.state == P34_FAILED) break;
    }
    printf("  V.90 caller after a V.92 Phase 2: state %d err %d, down %d up %d bit/s, up %ld bits %ld bad, down %ld bits %ld bad\n",
           pd.state, pd.error, pd.down_rate, pd.up_rate, dio.sink.n, dio.sink.bad, aio.sink.n, aio.sink.bad);
    CHECK_EQ_I(pd.state, P34_DATA); CHECK(pa.tx_ready && pa.rx_ready);
    CHECK_EQ_I(pd.down_rate, 38 * 8000 / 6); CHECK(pd.up_rate >= 4800);
    CHECK(aio.sink.n > 20000); CHECK_EQ_I(aio.sink.bad, 0);
    CHECK(dio.sink.n > 5000); CHECK_EQ_I(dio.sink.bad, 0);
}

int main(int argc, char **argv)
{
    static p2_t p2;
    scen_t s;
    out_t o, clean_mu, clean_a;
    int only = argc > 1 ? atoi(argv[1]) : 0;
#define WANT(n) (!only || only == (n))

    /* ---- law, line shape, noise ---- */
    if (WANT(1)) {
        base(&s, "1 mu-law, short line, no DIL", 0, 0, 3, 0);
        run(&s, &o); report(&s, &o); check_good(&s, &o); clean_mu = o;
        CHECK(o.up_rate > 33600);
        base(&s, "2 A-law, short line, DIL", 1, 0, 3, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o); clean_a = o;
        CHECK(o.up_rate > 33600);
        base(&s, "3 mu-law, short line, noisy", 0, 0, 40, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate < clean_mu.up_rate);
        base(&s, "4 A-law, short line, noisy", 1, 0, 40, 0);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate < clean_a.up_rate);
    }
    if (WANT(2)) {
        base(&s, "5 mu-law, long line", 0, 1, 3, 40);
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(o.up_rate > 33600);
        base(&s, "6 A-law, long line, noisy", 1, 1, 40, 40);
        s.phase0 = 0.85;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "7 A-law, long line with a bump", 1, 2, 3, 0);
        s.phase0 = 0.6;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "8 mu-law, bump, noisy, weak", 0, 2, 20, 40);
        s.level = 1500;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- pads, echo, delay ---- */
    if (WANT(3)) {
        base(&s, "9 mu-law, 3 dB pad upstream", 0, 0, 3, 0);
        s.up_pad = 3;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "10 A-law, 6 dB pad upstream", 1, 1, 3, 40);
        s.up_pad = 6;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "11 mu-law, strong late echo", 0, 0, 3, 40);
        s.echo = 0.25; s.echo_delay = 60; s.d_down = 150; s.d_up = 170;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "12 A-law, no echo, loud", 1, 0, 3, 0);
        s.echo = 0; s.level = 9000;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- our limits ---- */
    if (WANT(4)) {
        base(&s, "13 ceiling 33333 bit/s", 0, 0, 3, 0);
        s.max_up = 33333;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.up_rate, 33333);
        base(&s, "14 caller allows 24000..32000", 1, 0, 3, 40);
        s.up_rates = 0x7F;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.up_rate, 32000);
    }
    /* ---- rate renegotiation, cleardown, retrain ---- */
    if (WANT(5)) {
        base(&s, "15 caller renegotiates", 0, 0, 3, 0);
        s.a_reneg_ms = 5500; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I(a.renegs, 1);
        CHECK(o.up_bits2 > 2 * 24000); CHECK_EQ_I(o.up_bad2, 0);
        base(&s, "16 we renegotiate", 1, 1, 3, 40);
        s.d_reneg_ms = 5500; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I(a.renegs, 1);
        CHECK(a.rd_units >= 60 && a.rdbar_units == 4); CHECK_EQ_I(a.rd_bad, 0);
        CHECK(o.up_bits2 > 2 * 24000); CHECK_EQ_I(o.up_bad2, 0);
        base(&s, "17 caller clears down", 0, 0, 3, 0);
        s.a_clear_ms = 5500; s.seconds = 9;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CLEARDOWN);
        CHECK_EQ_I(o.up_bad, 0); CHECK_EQ_I(o.down_bad, 0);
        base(&s, "18 we clear down", 0, 0, 3, 0);
        s.d_clear_ms = 5500; s.seconds = 9;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_HANGUP);
        CHECK(a.clear_seen); CHECK_EQ_I(o.up_bad, 0); CHECK_EQ_I(o.down_bad, 0);
        base(&s, "19 the caller's Tone A", 0, 0, 3, 0);
        s.tone_a_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_RETRAIN); CHECK_EQ_I(d.retrain_ours, 0);
        CHECK(o.d_end_at > 5500 * 8 + 400 && o.d_end_at < 5500 * 8 + 1000);
        base(&s, "20 we retrain", 0, 0, 3, 0);
        s.d_retrain_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.req_rc, 0); CHECK_EQ_I(o.state, V92D_RETRAIN); CHECK_EQ_I(d.retrain_ours, 1);
    }
    /* ---- the echo delay measured during TRN1d; the caller's signal lost ---- */
    if (WANT(7)) {
        base(&s, "22 echo 60 samples late, not told", 0, 0, 3, 40);
        s.echo = 0.25; s.echo_delay = 60; s.echo_search = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        /* a little room before the echo path, as p34d leaves */
        CHECK(d.ecd.delay >= 60 - 12 && d.ecd.delay <= 60);
        base(&s, "23 A-law, echo 400 late, not told", 1, 1, 3, 0);
        s.echo_delay = 400; s.echo_search = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(d.ecd.delay >= 400 - 12 && d.ecd.delay <= 400);
        base(&s, "24 the caller falls silent", 0, 0, 3, 0);
        s.mute_ms = 5500;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_FAILED); CHECK_EQ_I(o.error, V92D_ERR_CARRIER);
        CHECK(o.d_end_at > 5500 * 8 + 3000 && o.d_end_at < 5500 * 8 + 6000);
        /* next to nothing was delivered from the silence: the tail in the
         * decoder and at most the 10 ms the carrier watch needs to see it */
        CHECK(o.up_bad < 400);
    }
    /* ---- what the first real V.92 caller showed (live/v92a.raw, readings 53 on) ---- */
    if (WANT(8)) {
        /* an FXS port: our echo as loud as the caller (9 dB under what we
         * send) and 270 samples late, with a slow tail that a 12 ms canceller
         * leaves as a wander of some 150 units */
        base(&s, "25 echo at the caller's level, slow tail", 0, 0, 5, 40);
        s.level = 1400; s.echo = 0.2; s.echo_tail = 0.57; s.echo_delay = 270; s.echo_search = 1;
        s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);

        /* a U.S. Robotics 5686 on the same port: the echo 25 dB under what we
         * send, its start no larger than one tap of the slow tail behind it;
         * the delay must still be found (live/usr2.raw: 300 taken for 270,
         * the canceller beside the echo, the design gave up) */
        base(&s, "25b weak echo under its own slow tail", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 4.0; s.echo_delay = 270; s.echo_search = 1;
        s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        printf("    echo delay found: %d\n", d.ecd.delay);
        CHECK(d.ecd.delay >= 270 - 12 && d.ecd.delay <= 270 + 4);

        /* the same modem's patience: it gives up 3.8 s into TRN2u without
         * SUVd and 3.5 s after its SUVu without CPd, and does not send SUVu
         * by itself. A design at eight samples a step takes longer than
         * the first and must still arrive within the second; CPu is in by
         * then, and a first CPd that says so is answered with silence. */
        base(&s, "25c a caller that waits 3.8 s for SUVd, 3.5 s for CPd", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 1.2; s.echo_delay = 270; s.echo_search = 1;
        s.pace = 8; s.suvd_wait = 30400; s.cpd_wait = 28000; s.first_cpd_plain = 1; s.seconds = 14;
        run(&s, &o); report(&s, &o); check_good(&s, &o);

        /* and what it showed in data mode: a slow wander under the symbols
         * (our own low frequencies, which the tail estimate does not take
         * away once the downstream carries data), a hundred units where
         * the points are 250 apart. Here it begins at 5 s, after the
         * design, as a sine of 25 Hz. */
        base(&s, "25d a slow wander under the data", 0, 0, 5, 40);
        s.level = 2700; s.echo = 0.02; s.echo_tail = 1.2; s.echo_delay = 270; s.echo_search = 1;
        s.wander = 100; s.seconds = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);

        /* a data frame of Ki = 0 before B1u (reading 57) */
        base(&s, "26 a data frame of Ki = 0 before B1u", 0, 1, 3, 40);
        s.b1u_lead = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);
        base(&s, "27 the same, A-law, another line", 1, 0, 3, 0);
        s.b1u_lead = 1;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.b1_leads, 1);

        /* the caller's prefilter has no coefficient 64 (reading 58) */
        base(&s, "28 z2(64) taken as zero", 0, 1, 3, 40);
        s.z2_dead = 64;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK_EQ_I(d.cpd.filt.coef[d.cpd.filt.lp1 + 64], 0);

        /* the convolutional encoder does not start B1u from zero (reading 59) */
        base(&s, "30 trellis encoder not at zero for B1u", 0, 1, 3, 40);
        s.conv_start = 11;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        base(&s, "31 the same after a data frame of Ki = 0, with a rate renegotiation", 0, 0, 3, 40);
        s.conv_start = 6; s.b1u_lead = 1; s.a_reneg_ms = 5000; s.seconds = 12;
        run(&s, &o); report(&s, &o);
        CHECK_EQ_I(o.state, V92D_DATA); CHECK_EQ_I(o.renegs, 1); CHECK_EQ_I((int)o.up_bad2, 0); CHECK(o.up_bits2 > 24000);

        /* all of it, with the limits the CX93001 gives in INFO1a */
        base(&s, "29 as the CX93001 on the FXS port", 0, 0, 5, 40);
        s.level = 1400; s.echo = 0.2; s.echo_tail = 0.57; s.echo_delay = 270; s.echo_search = 1;
        s.b1u_lead = 1; s.z2_dead = 64; s.ltot = 192; s.lmax = 128; s.uinfo = 79;
        s.conv_start = 9; s.ja_no_ones = 1; s.ntrn1 = 16368; s.jd_wait = 30000;
        s.seconds = 17;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
    }
    /* ---- with Phase 2 in front ---- */
    if (WANT(6)) {
        phase2(&p2, 1);
        CHECK_EQ_I(p2.d.state, P2D_DONE); CHECK_EQ_I(p2.a.state, P2A_DONE);
        CHECK_EQ_I(p2.d.mode, P2D_MODE_V92);
        CHECK_EQ_I(info1a_kind(&p2.d.info1a), INFO1A_PCM);
        base(&s, "21 after Phase 2 (V.92 caller)", 0, 1, 3, 40);
        s.uinfo = p2.d.info1a.uinfo;
        s.ltot = 192 + 64 * p2.d.info1a.ltot_code; s.lmax = 128 + 64 * p2.d.info1a.lmax_code;
        run(&s, &o); report(&s, &o); check_good(&s, &o);
        CHECK(d.cpd.filt.lz2 <= s.lmax && d.cpd.filt.lz2 + d.cpd.filt.lp1 <= s.ltot);

        phase2(&p2, 0);                     /* a V.90 caller */
        CHECK_EQ_I(p2.d.state, P2D_DONE); CHECK_EQ_I(p2.a.state, P2A_DONE);
        CHECK_EQ_I(p2.d.mode, P2D_MODE_V90);
        v90_call(&p2);
    }
    T_REPORT("v92call");
}
