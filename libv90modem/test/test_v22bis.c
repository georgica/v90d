/* End-to-end V.22bis: our answerer (digital side) against our caller through
 * the channel simulator: handshake at both rates, then data both ways. */
#include <string.h>
#include "test.h"
#include "../modem/v22bis.h"
#include "../sim/channel.h"
#include "../sim/g711.h"
#include "../sim/resamp.h"
#include "../sim/filt.h"

#define FS 8000
#define NB 6000

typedef struct { uint32_t l; uint8_t tx[NB]; int ntx; uint8_t rx[NB + 2000]; int nrx; } side;
static int gb(void *u)
{
    side *s = u; int b;
    s->l = s->l * 1664525u + 1013904223u; b = (s->l >> 24) & 1;
    if (s->ntx < NB) s->tx[s->ntx++] = (uint8_t)b;
    return b;
}
static void pb(void *u, int b) { side *s = u; if (s->nrx < NB + 2000) s->rx[s->nrx++] = (uint8_t)b; }

/* errors between what `from` sent and what `to` received (best alignment) */
static int errors(const side *from, const side *to, int *compared)
{
    int off, best = 1 << 30, best_off = 0;
    *compared = 0;
    for (off = 0; off < 1200; off++) {
        int j, err = 0, cnt = 0;
        for (j = 0; j < from->ntx && j + off < to->nrx; j++, cnt++)
            if (to->rx[j + off] != from->tx[j]) err++;
        if (cnt >= 3000 && err < best) { best = err; *compared = cnt; best_off = off; }
    }
    if (best > 0 && best < 1000) { int j; printf("  error positions:"); for (j = 0; j < from->ntx && j + best_off < to->nrx; j++) if (to->rx[j + best_off] != from->tx[j]) printf(" %d", j); printf("\n"); }
    return best;
}

typedef struct { int rate_a, rate_c, ready_at, s1_end, a_2400_at; int err_ca, err_ac, cmp_ca, cmp_ac; } outcome;

static void run(int ans2400, int call2400, double noise, double echo, double ppm, int seconds, outcome *o)
{
    static const double one[1] = {1.0};
    static side sa, sc;
    channel_cfg cfg; channel_t ch;
    v22bis_t a, c;
    sresamp_t rs_dn, rs_up;
    int16_t atx = 0, ctx = 0;
    int16_t fifo[64]; int fh = 0, ft = 4;        /* upstream elastic buffer, 4 samples prefilled */
    double c_in[4], a_in_d[4];
    int n, k;
    v22_txmode prev_c = V22_TX_SILENCE, prev_a = V22_TX_SILENCE;

    memset(fifo, 0, sizeof fifo);
    memset(&sa, 0, sizeof sa); memset(&sc, 0, sizeof sc); sa.l = 11; sc.l = 22;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = noise; cfg.us_noise_dbfs = noise;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = echo; cfg.echo_delay = 23; cfg.full_scale = 32124.0; cfg.seed = 7;
    channel_init(&ch, &cfg);
    /* the caller's codec clock differs from the network's by ppm */
    sresamp_init(&rs_dn, ppm_ratio(ppm)); sresamp_init(&rs_up, ppm_ratio(-ppm));
    v22m_init(&a, V22_ANSWERER, ans2400, 3000, gb, pb, &sa);
    v22m_init(&c, V22_CALLER, call2400, 3000, gb, pb, &sc);
    memset(o, 0, sizeof *o); o->ready_at = o->s1_end = o->a_2400_at = -1;

    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int nd, nu;
        if (fh != ft) { ctx = fifo[fh]; fh = (fh + 1) & 63; }   /* else: underrun, repeat */
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        atx = v22m_tick(&a, (int16_t)ulaw_to_linear(srv));
        nd = sresamp_push(&rs_dn, client_rx, c_in, 4);
        for (k = 0; k < nd; k++) {
            int16_t v = v22m_tick(&c, (int16_t)lround(c_in[k]));
            nu = sresamp_push(&rs_up, (double)v, a_in_d, 4);
            { int q; for (q = 0; q < nu; q++) { fifo[ft] = (int16_t)lround(a_in_d[q]); ft = (ft + 1) & 63; } }
        }
        if (c.tx.mode != prev_c) { if (prev_c == V22_TX_S1) o->s1_end = n; prev_c = c.tx.mode; }
        if (a.tx.mode != prev_a) { if (a.tx.mode == V22_TX_SB1_2400) o->a_2400_at = n; prev_a = a.tx.mode; }
        if (o->ready_at < 0 && a.tx_ready && a.rx_ready && c.tx_ready && c.rx_ready) o->ready_at = n;
    }
    if (o->ready_at < 0)
        printf("  v22bis not ready: A state %d rdy %d%d mse %d sig %d | C state %d rdy %d%d mse %d sig %d\n",
               a.state, a.tx_ready, a.rx_ready, (int)a.rx.mse, a.rx.sig, c.state, c.tx_ready, c.rx_ready, (int)c.rx.mse, c.rx.sig);
    o->rate_a = a.bitrate; o->rate_c = c.bitrate;
    o->err_ca = errors(&sc, &sa, &o->cmp_ca);
    o->err_ac = errors(&sa, &sc, &o->cmp_ac);
}

/* ---------------------------------------------------------------------------
 * Second harness: long calls with exact bit accounting, an event log of both
 * modems, and things happening in mid-call (retrain requests, a change of
 * the caller-to-answerer path, a dropout). Used by the start-up-in-noise and
 * the retrain (V.22bis 6.4) tests.
 * ------------------------------------------------------------------------ */
#define LB 200000
typedef struct { uint32_t l; uint8_t tx[LB]; int32_t tx_at[LB]; int ntx; uint8_t rx[LB]; int32_t rx_at[LB]; int nrx; } lside;
static int l_now;
static int lgb(void *u)
{
    lside *s = u; int b;
    s->l = s->l * 1664525u + 1013904223u; b = (s->l >> 24) & 1;
    if (s->ntx < LB) { s->tx[s->ntx] = (uint8_t)b; s->tx_at[s->ntx++] = l_now; }
    return b;
}
static void lpb(void *u, int b) { lside *s = u; if (s->nrx < LB) { s->rx[s->nrx] = (uint8_t)b; s->rx_at[s->nrx++] = l_now; } }

/* SNR here: signal RMS (3000) over noise RMS, both in the whole 0..4 kHz band */
static double noise_for_snr(double snr_db) { return 20.0 * log10(3000.0 / (32124.0 / sqrt(2.0))) - snr_db; }

enum { EV_S1_ON, EV_S1_OFF, EV_2400_ON, EV_TXRDY, EV_TXNRDY, EV_RXRDY, EV_RXNRDY, EV_N };
typedef struct { int at, side, kind, arg; } lev;      /* side 0 = answerer, 1 = caller */
#define LEVMAX 512

typedef struct {
    double snr_db;          /* >= 90: quiet line (-60 dBFS) */
    uint32_t seed;
    int seconds;
    int guard;              /* answerer's guard tone, Hz (0 = none) */
    int c_retrain_at;       /* sample at which the caller / answerer is told to retrain (0 = never) */
    int a_retrain_at;
    int us_step_at;         /* caller-to-answerer path changes here (0 = never) */
    const double *us_step_taps; int us_step_ntaps;
    int us_drop_at, us_drop_len;   /* caller-to-answerer path silent for so long */
    int deaf_caller_at;     /* from here the caller hears nothing at all (0 = never) */
} lcond;

typedef struct {
    lev ev[LEVMAX]; int nev;
    int ready_at, rate_a, rate_c, state_a, state_c;
    int a_retrains, c_retrains;
    int mse_a, mse_c;
} lres;

static lside la, lc;
static lres  lr;
static v22bis_t lma, lmc;

static void levent(int side, int kind, int arg)
{
    if (lr.nev < LEVMAX) { lr.ev[lr.nev].at = l_now; lr.ev[lr.nev].side = side; lr.ev[lr.nev].kind = kind; lr.ev[lr.nev].arg = arg; lr.nev++; }
}
/* first event of this kind on this side at or after `from` (-1: none) */
static int ev_after(int side, int kind, int from)
{
    int i;
    for (i = 0; i < lr.nev; i++) if (lr.ev[i].side == side && lr.ev[i].kind == kind && lr.ev[i].at >= from) return lr.ev[i].at;
    return -1;
}
static int ev_count(int side, int kind, int from)
{
    int i, c = 0;
    for (i = 0; i < lr.nev; i++) if (lr.ev[i].side == side && lr.ev[i].kind == kind && lr.ev[i].at >= from) c++;
    return c;
}

static void lrun(const lcond *q)
{
    static const double one[1] = {1.0};
    channel_cfg cfg; channel_t ch;
    fir_t usf; double usz[16];
    int16_t atx = 0, ctx = 0;
    v22_txmode pm[2]; int ptx[2], prx[2];
    int n, i;

    memset(&la, 0, sizeof la); memset(&lc, 0, sizeof lc); la.l = 11; lc.l = 22;
    memset(&lr, 0, sizeof lr); lr.ready_at = -1;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = cfg.us_noise_dbfs = q->snr_db >= 90.0 ? -60.0 : noise_for_snr(q->snr_db);
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.15; cfg.echo_delay = 23; cfg.full_scale = 32124.0; cfg.seed = q->seed;
    channel_init(&ch, &cfg);
    fir_init(&usf, one, 1, usz);
    l_now = 0;
    v22m_init(&lma, V22_ANSWERER, 1, 3000, lgb, lpb, &la);
    v22m_init(&lmc, V22_CALLER, 1, 3000, lgb, lpb, &lc);
    if (q->guard) v22m_set_guard(&lma, q->guard);
    pm[0] = lma.tx.mode; pm[1] = lmc.tx.mode; ptx[0] = ptx[1] = prx[0] = prx[1] = 0;

    for (n = 0; n < q->seconds * FS; n++) {
        double client_rx, up; uint8_t srv;
        l_now = n;
        if (q->us_step_at && n == q->us_step_at) {
            for (i = 0; i < 16; i++) usz[i] = 0.0;
            fir_init(&usf, q->us_step_taps, q->us_step_ntaps, usz);
        }
        if (q->c_retrain_at && n == q->c_retrain_at) v22m_retrain(&lmc);
        if (q->a_retrain_at && n == q->a_retrain_at) v22m_retrain(&lma);
        up = fir_push(&usf, (double)ctx);
        if (q->us_drop_at && n >= q->us_drop_at && n < q->us_drop_at + q->us_drop_len) up = 0.0;
        channel_tick(&ch, linear_to_ulaw(atx), up, &client_rx, &srv);
        if (q->deaf_caller_at && n >= q->deaf_caller_at) client_rx = 0.0;
        atx = v22m_tick(&lma, (int16_t)ulaw_to_linear(srv));
        ctx = v22m_tick(&lmc, (int16_t)lround(client_rx));
        for (i = 0; i < 2; i++) {
            v22bis_t *m = i ? &lmc : &lma;
            if (m->tx.mode != pm[i]) {
                if (pm[i] == V22_TX_S1) levent(i, EV_S1_OFF, 0);
                if (m->tx.mode == V22_TX_S1) levent(i, EV_S1_ON, 0);
                if (m->tx.mode == V22_TX_SB1_2400) levent(i, EV_2400_ON, 0);
                pm[i] = m->tx.mode;
            }
            if (m->tx_ready != ptx[i]) { levent(i, m->tx_ready ? EV_TXRDY : EV_TXNRDY, 0); ptx[i] = m->tx_ready; }
            if (m->rx_ready != prx[i]) { levent(i, m->rx_ready ? EV_RXRDY : EV_RXNRDY, 0); prx[i] = m->rx_ready; }
        }
        if (lr.ready_at < 0 && lma.tx_ready && lma.rx_ready && lmc.tx_ready && lmc.rx_ready) lr.ready_at = n;
    }
    lr.rate_a = lma.bitrate; lr.rate_c = lmc.bitrate; lr.state_a = lma.state; lr.state_c = lmc.state;
    lr.a_retrains = lma.retrains; lr.c_retrains = lmc.retrains;
    lr.mse_a = (int)lma.rx.mse; lr.mse_c = (int)lmc.rx.mse;
}

/* Bit errors from `from` to `to`, for the bits `from` sent at or after sample
 * t_tx, looked for among the bits `to` received at or after sample t_rx
 * (and before t_end, if t_end > 0). The alignment is searched over the first
 * 3000 received bits; with 0 errors found this also proves nothing sent after
 * t_tx was lost. Returns -1 if fewer than 1000 bits could be compared. */
static int l_tx_end;      /* if > 0: only bits sent before this sample (reset by each call) */
static int l_rx_next;     /* out: index of the first received bit after those compared */
static int lerrors(const lside *from, const lside *to, int t_tx, int t_rx, int t_end, int *compared)
{
    int i0 = 0, i1 = from->ntx, j0 = 0, j1, d, best = -1, bd = 0, j, lim;
    if (l_tx_end > 0) { i1 = 0; while (i1 < from->ntx && from->tx_at[i1] < l_tx_end) i1++; }
    l_tx_end = 0; l_rx_next = -1;
    while (i0 < from->ntx && from->tx_at[i0] < t_tx) i0++;
    while (j0 < to->nrx && to->rx_at[j0] < t_rx) j0++;
    j1 = to->nrx;
    if (t_end > 0) { j1 = j0; while (j1 < to->nrx && to->rx_at[j1] < t_end) j1++; }
    *compared = 0;
    for (d = 0; d < 3000; d++) {
        int err = 0;
        lim = 1000;
        if (i0 + lim > i1 || j0 + d + lim > j1) break;
        for (j = 0; j < lim; j++) if (to->rx[j0 + d + j] != from->tx[i0 + j]) err++;
        if (best < 0 || err < best) { best = err; bd = d; }
    }
    if (best < 0) return -1;
    best = 0;
    for (j = 0; i0 + j < i1 && j0 + bd + j < j1; j++) if (to->rx[j0 + bd + j] != from->tx[i0 + j]) best++;
    *compared = j; l_rx_next = j0 + bd + j;
    return best;
}

/* Start-up in steady noise: SNR sweep, three noise seeds each. Where 2400
 * bit/s is usable the call must come up at 2400; below that it must still
 * come up, at 1200 (the answerer does not return S1 on a line it measured as
 * too noisy), and in neither case into a link that is no use. */
static void test_startup_noise(void)
{
    static const double snr[] = { 26.0, 20.0, 16.0, 14.0, 12.0, 10.0, 8.0, 6.0, 4.0 };
    int k, seed;
    printf("  start-up in steady noise (level 3000 both ways, echo 0.15, mu-law; 3 noise seeds, 16 s calls):\n");
    for (k = 0; k < (int)(sizeof snr / sizeof snr[0]); k++) {
        int up = 0, e_ca = 0, n_ca = 0, e_ac = 0, n_ac = 0, r2400 = 0, r1200 = 0, late = 0, rt = 0;
        double q[3];
        for (seed = 1; seed <= 3; seed++) {
            lcond c0; int e, c;
            memset(&c0, 0, sizeof c0); c0.snr_db = snr[k]; c0.seed = (uint32_t)seed; c0.seconds = 16;
            lrun(&c0);
            q[seed - 1] = lma.s1_snr > 0 ? 10.0 * log10((double)lma.s1_snr) : -99.0;
            rt += lr.a_retrains + lr.c_retrains;
            if (lr.ready_at < 0) continue;
            up++;
            if (lr.ready_at > 3 * FS) late++;
            if (lr.rate_a == 2400 && lr.rate_c == 2400) r2400++;
            if (lr.rate_a == 1200 && lr.rate_c == 1200) r1200++;
            e = lerrors(&lc, &la, lr.ready_at + FS / 2, lr.ready_at + FS / 2, 0, &c); if (e >= 0) { e_ca += e; n_ca += c; }
            e = lerrors(&la, &lc, lr.ready_at + FS / 2, lr.ready_at + FS / 2, 0, &c); if (e >= 0) { e_ac += e; n_ac += c; }
        }
        printf("    SNR %4.1f dB: up %d/3 (%d at 2400, %d at 1200), retrains %d; errors c->a %d/%d (%.1e), a->c %d/%d (%.1e); S1 in-band SNR seen %.1f %.1f %.1f dB\n",
               snr[k], up, r2400, r1200, rt, e_ca, n_ca, n_ca ? (double)e_ca / n_ca : 1.0, e_ac, n_ac, n_ac ? (double)e_ac / n_ac : 1.0,
               q[0], q[1], q[2]);
        CHECK_EQ_I(late, 0);
        CHECK_EQ_I(up, 3); CHECK_EQ_I(r2400 + r1200, 3);
        if (snr[k] >= 12.0) CHECK_EQ_I(rt, 0);         /* a line that works is never retrained */
        if (snr[k] >= 14.0) CHECK_EQ_I(r2400, 3);     /* 2400 bit/s is good here: it must be 2400 */
        if (snr[k] <= 8.0) CHECK_EQ_I(r1200, 3);      /* and here it is no use */
        /* in between either will do; whatever was chosen has to carry data */
        if (snr[k] >= 6.0) {
            double lim = snr[k] >= 12.0 ? 1e-3 : 1e-2;
            CHECK(n_ca > 40000 && e_ca < lim * n_ca);
            CHECK(n_ac > 40000 && e_ac < lim * n_ac);
        }
    }
}

/* ---- retrain (6.4) ---- */
static int count_tx(const lside *s, int t0, int t1)
{ int i, c = 0; for (i = 0; i < s->ntx; i++) if (s->tx_at[i] >= t0 && s->tx_at[i] < t1) c++; return c; }
static int count_rx(const lside *s, int t0, int t1, int *zeros)
{
    int i, c = 0; *zeros = 0;
    for (i = 0; i < s->nrx; i++) if (s->rx_at[i] >= t0 && s->rx_at[i] < t1) { c++; if (!s->rx[i]) (*zeros)++; }
    return c;
}

/* Timings of one retrain that began (first S1 on the line) at or after `from`:
 * `ini` sent the retrain signal first, the other modem answered it. */
static void check_retrain_timing(int ini, int from, const char *what)
{
    int rsp = !ini;
    int i_on = ev_after(ini, EV_S1_ON, from), i_off = ev_after(ini, EV_S1_OFF, i_on);
    int r_on = ev_after(rsp, EV_S1_ON, from), r_off = ev_after(rsp, EV_S1_OFF, r_on);
    int i_24 = ev_after(ini, EV_2400_ON, i_off), r_24 = ev_after(rsp, EV_2400_ON, r_off);
    int i_tx = ev_after(ini, EV_TXRDY, i_24), r_tx = ev_after(rsp, EV_TXRDY, r_24);
    int i_rx = ev_after(ini, EV_RXRDY, i_on), r_rx = ev_after(rsp, EV_RXRDY, r_on);
    CHECK(i_on >= 0 && i_off > 0 && r_on > 0 && r_off > 0 && i_24 > 0 && r_24 > 0 && i_tx > 0 && r_tx > 0 && i_rx > 0 && r_rx > 0);
    if (i_on < 0 || r_off < 0 || i_tx < 0 || r_tx < 0 || i_rx < 0 || r_rx < 0) return;
    printf("    %s: S1 %.1f ms, answered %.1f ms after it ended by S1 of %.1f ms; 2400 bit/s %.1f / %.1f ms after the far S1 ended,\n"
           "      data again %.1f / %.1f ms later; receivers unclamped at %.0f / %.0f ms; whole retrain %.0f ms\n",
           what, (i_off - i_on) / 8.0, (r_on - i_off) / 8.0, (r_off - r_on) / 8.0,
           (i_24 - r_off) / 8.0, (r_24 - i_off) / 8.0, (i_tx - i_24) / 8.0, (r_tx - r_24) / 8.0,
           (i_rx - i_on) / 8.0, (r_rx - i_on) / 8.0, ((i_tx > r_tx ? i_tx : r_tx) - i_on) / 8.0);
    /* 6.4 a: S1 for 100 +/- 3 ms, from both */
    CHECK_NEAR((i_off - i_on) / 8.0, 100.0, 3.0);
    CHECK_NEAR((r_off - r_on) / 8.0, 100.0, 3.0);
    /* the answer follows the end of the received S1 (our lag in seeing that end, plus the line) */
    CHECK((r_on - i_off) / 8.0 >= 0.0 && (r_on - i_off) / 8.0 < 40.0);
    /* 6.4 b: scrambled ones at 2400 bit/s 600 +/- 10 ms after the end of the far
     * S1 (plus modem and line delay, as in the start-up test above) */
    CHECK_NEAR((i_24 - r_off) / 8.0, 600.0 + 9.0, 12.0);
    CHECK_NEAR((r_24 - i_off) / 8.0, 600.0 + 9.0, 12.0);
    /* 6.4 c: for 200 +/- 10 ms, then data */
    CHECK_NEAR((i_tx - i_24) / 8.0, 200.0, 10.0);
    CHECK_NEAR((r_tx - r_24) / 8.0, 200.0, 10.0);
    /* 6.4 d: each receiver is ready (32 ones seen) before the far end sends data again */
    CHECK(i_rx < r_tx && r_rx < i_tx);
}

/* Data around one retrain, for the direction from -> to (side numbers as in
 * the event log; fs, ts the bit records). t0 = start of the retrain. */
static void check_retrain_data(int from, int to, int t_up, int t0, const char *what)
{
    const lside *fs = from ? &lc : &la, *ts = to ? &lc : &la;
    int tx_off = ev_after(from, EV_TXNRDY, t0), tx_on = ev_after(from, EV_TXRDY, tx_off);
    int rx_off = ev_after(to, EV_RXNRDY, t0), rx_on = ev_after(to, EV_RXRDY, rx_off);
    int e_before, c_before, e_after, c_after, during, zeros, held, sent_before, idle = 0, idle_z = 0, j;
    CHECK(tx_off > 0 && tx_on > 0 && rx_off > 0 && rx_on > 0);
    if (tx_off < 0 || tx_on < 0 || rx_off < 0 || rx_on < 0) return;
    /* before: everything received up to the clamp is what was sent */
    l_tx_end = tx_off;
    e_before = lerrors(fs, ts, t_up + FS / 2, t_up + FS / 2, rx_off + 1, &c_before);
    /* and what came after the sender's last data bit is its idle line: ones */
    for (j = l_rx_next; j >= 0 && j < ts->nrx && ts->rx_at[j] <= rx_off; j++) { idle++; if (!ts->rx[j]) idle_z++; }
    /* during: circuit 106 OFF (no bit taken from the DTE), circuit 104 at binary 1 at 2400 bit/s */
    held = count_tx(fs, tx_off, tx_on);
    during = count_rx(ts, rx_off + 1, rx_on + 1, &zeros);     /* (the event is logged after that sample's bits) */
    /* after: from the first bit taken once 106 is ON again, nothing lost, nothing wrong */
    e_after = lerrors(fs, ts, tx_on, rx_on, 0, &c_after);
    /* bits taken from the DTE before the retrain that the far end's clamp swallowed */
    sent_before = count_tx(fs, t_up + FS / 2, tx_off);
    printf("    %s: before %d errors in %d bits, then %d idle bits (%d not 1); clamp held %.0f ms: %d bits, %d of them not 1;\n"
           "      DTE bits taken meanwhile %d; after %d errors in %d bits; lost at the cut %d bits\n",
           what, e_before, c_before, idle, idle_z, (rx_on - rx_off) / 8.0, during, zeros, held, e_after, c_after, sent_before - c_before);
    CHECK(c_before > 5000); CHECK_EQ_I(e_before, 0);
    CHECK_EQ_I(idle_z, 0);
    CHECK_EQ_I(held, 0);
    CHECK_EQ_I(zeros, 0);
    CHECK_NEAR((double)during, 2.4 * (rx_on - rx_off) / 8.0, 12.0);   /* the 2400 bit/s clock runs on */
    CHECK(c_after > 20000); CHECK_EQ_I(e_after, 0);
    CHECK(sent_before - c_before >= 0 && sent_before - c_before < 200);    /* the far end's reaction time and the line */
}

static void test_retrain(void)
{
    lcond q; int who, t0;

    printf("  retrain (6.4), quiet line, requested 8 s into the call:\n");
    for (who = 0; who < 3; who++) {
        static const char *name[3] = { "by the caller", "by us (answerer)", "by both at once" };
        int ini = who == 0 ? 1 : 0;
        memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 30;
        if (who != 1) q.c_retrain_at = 8 * FS;
        if (who != 0) q.a_retrain_at = 8 * FS;
        lrun(&q);
        CHECK(lr.ready_at > 0 && lr.ready_at < 3 * FS);
        CHECK_EQ_I(lr.a_retrains, 1); CHECK_EQ_I(lr.c_retrains, 1);
        CHECK_EQ_I(lr.state_a, V22S_DATA); CHECK_EQ_I(lr.state_c, V22S_DATA);
        CHECK_EQ_I(lma.rt_lost + lmc.rt_lost, 0);
        t0 = ev_after(ini, EV_S1_ON, 8 * FS - 8);
        CHECK(t0 >= 8 * FS - 8 && t0 < 8 * FS + 40);
        /* one S1 from each, no more */
        CHECK_EQ_I(ev_count(0, EV_S1_ON, 7 * FS), 1); CHECK_EQ_I(ev_count(1, EV_S1_ON, 7 * FS), 1);
        if (who < 2) check_retrain_timing(ini, 8 * FS - 8, name[who]);
        else {
            /* both sent S1 at the same time: each still counts 600 ms from the end of the other's */
            int a_off = ev_after(0, EV_S1_OFF, t0), c_off = ev_after(1, EV_S1_OFF, t0);
            int a_24 = ev_after(0, EV_2400_ON, t0), c_24 = ev_after(1, EV_2400_ON, t0);
            printf("    %s: 2400 bit/s %.1f / %.1f ms after the far S1 ended\n", name[who], (a_24 - c_off) / 8.0, (c_24 - a_off) / 8.0);
            CHECK_NEAR((a_24 - c_off) / 8.0, 609.0, 12.0); CHECK_NEAR((c_24 - a_off) / 8.0, 609.0, 12.0);
        }
        check_retrain_data(1, 0, lr.ready_at, t0, "caller -> answerer");
        check_retrain_data(0, 1, lr.ready_at, t0, "answerer -> caller");
        /* the carrier never went away: circuits 107 / 109 stay ON (6.4) */
        CHECK(lma.rx.carrier && lmc.rx.carrier);
    }
}

/* Retrain started by us because the receiver lost equalisation: the path from
 * the caller to us changes in mid-call. */
/* The same on a noisy line (14 dB over 4 kHz, where 2400 bit/s makes about one
 * bit error in 10000), three noise seeds, both directions of request: the
 * retrain completes at the first attempt and no S1 is delivered as data. */
static void test_retrain_noisy(void)
{
    int seed, who, done = 0, bad_before = 0, n_before = 0, zeros = 0, e_after = 0, n_after = 0, extra = 0;
    for (seed = 1; seed <= 3; seed++) for (who = 0; who < 2; who++) {
        lcond q; int t0, rsp, ini, rx_off, rx_on, tx_on, e, c, z;
        memset(&q, 0, sizeof q); q.snr_db = 14.0; q.seed = (uint32_t)seed; q.seconds = 20;
        if (who) q.c_retrain_at = 8 * FS; else q.a_retrain_at = 8 * FS;
        ini = who ? 1 : 0; rsp = !ini;
        lrun(&q);
        if (lr.a_retrains == 1 && lr.c_retrains == 1 && lr.state_a == V22S_DATA && lr.state_c == V22S_DATA) done++;
        extra += ev_count(0, EV_S1_ON, 7 * FS) + ev_count(1, EV_S1_ON, 7 * FS) - 2;
        t0 = ev_after(ini, EV_S1_ON, 8 * FS - 8);
        /* towards the modem that was asked: what it delivered before its clamp is the data sent */
        rx_off = ev_after(rsp, EV_RXNRDY, t0); rx_on = ev_after(rsp, EV_RXRDY, rx_off); tx_on = ev_after(ini, EV_TXRDY, t0);
        if (rx_off < 0 || rx_on < 0 || tx_on < 0) continue;
        l_tx_end = t0;
        e = lerrors(ini ? &lc : &la, rsp ? &lc : &la, lr.ready_at + FS / 2, lr.ready_at + FS / 2, rx_off + 1, &c);
        if (e >= 0) { bad_before += e; n_before += c; }
        count_rx(rsp ? &lc : &la, rx_off + 1, rx_on + 1, &z); zeros += z;
        e = lerrors(ini ? &lc : &la, rsp ? &lc : &la, tx_on, rx_on, 0, &c);
        if (e >= 0) { e_after += e; n_after += c; }
    }
    printf("  retrain at 14 dB SNR, 6 calls: completed at the first attempt %d; towards the modem asked: before %d errors in %d bits,\n"
           "    clamped bits not 1: %d, after %d errors in %d bits\n", done, bad_before, n_before, zeros, e_after, n_after);
    CHECK_EQ_I(done, 6); CHECK_EQ_I(extra, 0);
    CHECK(n_before > 60000 && bad_before * 2000.0 < n_before);     /* line errors only: an S1 let through is some 50 at once */
    CHECK_EQ_I(zeros, 0);
    CHECK(n_after > 100000 && e_after * 2000.0 < n_after);
}

static void test_retrain_loss(void)
{
    /* a change the receiver adapts to by itself, and one it does not come
     * back from (shown by building with -DLOSS_MSE=100000000: the receiver
     * then stays at an error of about 3400 and every second bit is wrong) */
    static const double mild[] = { 0.3, 1.0, -0.5, 0.3 };
    static const double harsh[] = { 0.5, -1.0, 0.6, -0.3, 0.2 };
    lcond q; int e, c, t0, tx_on, rx_on;

    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 30;
    q.us_step_at = 8 * FS; q.us_step_taps = mild; q.us_step_ntaps = 4;
    lrun(&q);
    e = lerrors(&lc, &la, 12 * FS, 12 * FS, 0, &c);
    printf("  path change the equaliser follows: retrains %d, %d errors in %d bits from 4 s after it\n", lr.a_retrains + lr.c_retrains, e, c);
    CHECK_EQ_I(lr.a_retrains + lr.c_retrains, 0); CHECK_EQ_I(lma.rt_lost, 0);
    CHECK(c > 30000); CHECK_EQ_I(e, 0);

    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 30;
    q.us_step_at = 8 * FS; q.us_step_taps = harsh; q.us_step_ntaps = 5;
    lrun(&q);
    t0 = ev_after(0, EV_S1_ON, 8 * FS);
    printf("  path change that loses equalisation: we sent the retrain signal %.2f s after it; retrains: answerer %d (for loss: %d), caller %d\n",
           (t0 - 8 * FS) / 8000.0, lr.a_retrains, lma.rt_lost, lr.c_retrains);
    CHECK_EQ_I(lma.rt_lost, 1); CHECK_EQ_I(lmc.rt_lost, 0);
    CHECK_EQ_I(lr.a_retrains, 1); CHECK_EQ_I(lr.c_retrains, 1);
    CHECK_EQ_I(lr.state_a, V22S_DATA); CHECK_EQ_I(lr.state_c, V22S_DATA);
    CHECK(t0 > 8 * FS + FS && t0 < 8 * FS + 4 * FS);        /* not at once, and not never */
    if (t0 < 0) return;
    check_retrain_timing(0, 8 * FS, "by us, for loss");
    /* the good direction: clean before, clamped during, clean after */
    check_retrain_data(0, 1, lr.ready_at, t0, "answerer -> caller");
    /* the direction that broke: after the retrain it carries data again, on the new path */
    tx_on = ev_after(1, EV_TXRDY, t0); rx_on = ev_after(0, EV_RXRDY, t0);
    e = lerrors(&lc, &la, tx_on, rx_on, 0, &c);
    {
        int z, n1 = count_rx(&la, ev_after(0, EV_RXNRDY, t0) + 1, rx_on + 1, &z);
        printf("    caller -> answerer: clamp held %d bits, %d of them not 1; after the retrain %d errors in %d bits; receiver error now %d\n",
               n1, z, e, c, lr.mse_a);
        CHECK_EQ_I(z, 0);
    }
    CHECK(c > 20000); CHECK_EQ_I(e, 0);
    CHECK(lr.mse_a < 400);
}

/* The far end never answers our retrain signal: it is repeated (6.4: no S1
 * during it or within 1.2 s), and after RT_MAX_TRIES (ours, not the
 * Recommendation's) the modem reports failure. Our receiver is fine and the
 * far end is still sending data, so that data goes on being delivered. */
static void test_retrain_unanswered(void)
{
    lcond q; int i, n = 0, prev = -1, z, cnt, bad = 0, first;
    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 26;
    q.a_retrain_at = 8 * FS; q.deaf_caller_at = 8 * FS - 400;
    lrun(&q);
    first = ev_after(0, EV_S1_ON, 8 * FS - 8);
    for (i = 0; i < lr.nev; i++) {
        if (lr.ev[i].side != 0 || lr.ev[i].kind != EV_S1_ON || lr.ev[i].at < 8 * FS - 8) continue;
        if (prev >= 0 && abs(lr.ev[i].at - prev - 1300 * 8) > 3 * 8) bad++;
        prev = lr.ev[i].at; n++;
    }
    z = lerrors(&lc, &la, 9 * FS, 9 * FS, 0, &cnt);
    printf("  retrain not answered: %d retrain signals 1300 ms apart, then state %d; data still arriving meanwhile: %d errors in %d bits; DTE bits taken %d\n",
           n, lr.state_a, z, cnt, count_tx(&la, first, 26 * FS));
    CHECK_EQ_I(n, 10); CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(lr.state_a, V22S_FAILED);
    CHECK(cnt > 30000); CHECK_EQ_I(z, 0);
    CHECK_EQ_I(count_tx(&la, first, 26 * FS), 0);
    CHECK_EQ_I(lr.a_retrains, 0);
}

/* A 150 ms hole in the caller's signal (6.5): whatever the receiver makes of
 * it, the link must be carrying data again afterwards. */
static void test_dropout(void)
{
    lcond q; int e, c, e2, c2;
    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 30;
    q.us_drop_at = 8 * FS; q.us_drop_len = 150 * 8;
    lrun(&q);
    e = lerrors(&lc, &la, 16 * FS, 16 * FS, 0, &c);
    e2 = lerrors(&la, &lc, 16 * FS, 16 * FS, 0, &c2);
    printf("  150 ms dropout: retrains %d (for loss %d); from 8 s after it %d errors in %d bits, other way %d in %d\n",
           lr.a_retrains, lma.rt_lost, e, c, e2, c2);
    CHECK(c > 20000); CHECK_EQ_I(e, 0);
    CHECK(c2 > 20000); CHECK_EQ_I(e2, 0);
    CHECK_EQ_I(lr.state_a, V22S_DATA); CHECK_EQ_I(lr.state_c, V22S_DATA);
}

/* guard tone on: the caller's receiver must not mind it (start-up, data, retrain) */
static void test_guard_call(void)
{
    lcond q; int e, c, e2, c2;
    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 20; q.guard = 1800;
    q.c_retrain_at = 8 * FS;
    lrun(&q);
    e = lerrors(&la, &lc, 10 * FS, 10 * FS, 0, &c);
    e2 = lerrors(&lc, &la, 10 * FS, 10 * FS, 0, &c2);
    printf("  call with our 1800 Hz guard tone: up at %.2f s, %d bit/s, retrains %d; errors to the caller %d/%d, from it %d/%d; caller's receiver error %d\n",
           lr.ready_at / 8000.0, lr.rate_c, lr.c_retrains, e, c, e2, c2, lr.mse_c);
    CHECK(lr.ready_at > 0 && lr.ready_at < 3 * FS);
    CHECK_EQ_I(lr.rate_a, 2400); CHECK_EQ_I(lr.rate_c, 2400);
    CHECK_EQ_I(lr.a_retrains, 1); CHECK_EQ_I(lr.c_retrains, 1);
    CHECK(c > 20000); CHECK_EQ_I(e, 0); CHECK(c2 > 20000); CHECK_EQ_I(e2, 0);
    CHECK(lr.mse_c < 400);
}

/* Start-up where the answerer misses the caller's 200 ms of scrambled ones at
 * 2400 bit/s (a hole in the line just then) and the caller then sends data,
 * not idle marks: the 32 ones of 6.3.1.1.2 e never come. Instead of giving
 * the call up the answerer asks for a retrain (6.4), and that completes. */
static void test_startup_retry(void)
{
    lcond q; int c_24, e, c, e2, c2, t_up;
    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 4;
    lrun(&q);
    c_24 = ev_after(1, EV_2400_ON, 0);                 /* same line, same seed: same timing next time */
    memset(&q, 0, sizeof q); q.snr_db = 99.0; q.seed = 5; q.seconds = 25;
    q.us_drop_at = c_24 - 30 * 8; q.us_drop_len = 260 * 8;
    lrun(&q);
    t_up = lr.ready_at;
    e = lerrors(&lc, &la, t_up + FS, t_up + FS, 0, &c);
    e2 = lerrors(&la, &lc, t_up + FS, t_up + FS, 0, &c2);
    printf("  start-up with the 2400 bit/s ones lost in a 260 ms hole: retrain signals from the answerer %d, up after %.2f s at %d bit/s; then %d/%d and %d/%d errors\n",
           ev_count(0, EV_S1_ON, c_24), t_up / 8000.0, lr.rate_a, e, c, e2, c2);
    CHECK(c_24 > 0);
    CHECK_EQ_I(ev_count(0, EV_S1_ON, c_24), 1);
    CHECK(t_up > 3 * FS && t_up < 6 * FS);
    CHECK_EQ_I(lr.state_a, V22S_DATA); CHECK_EQ_I(lr.state_c, V22S_DATA);
    CHECK(c > 20000); CHECK_EQ_I(e, 0); CHECK(c2 > 20000); CHECK_EQ_I(e2, 0);
}

int main(void)
{
    outcome o;

    test_startup_noise();
    test_retrain();
    test_retrain_noisy();
    test_retrain_loss();
    test_retrain_unanswered();
    test_dropout();
    test_startup_retry();
    test_guard_call();

    /* 1. V.22bis both ends: 2400 bit/s */
    run(1, 1, -60.0, 0.15, 0.0, 8, &o);
    CHECK_EQ_I(o.rate_a, 2400); CHECK_EQ_I(o.rate_c, 2400);
    CHECK(o.ready_at > 0);
    CHECK(o.cmp_ca >= 3000 && o.cmp_ac >= 3000);
    CHECK_EQ_I(o.err_ca, 0); CHECK_EQ_I(o.err_ac, 0);
    /* answerer goes to 2400 600 +/- 10 ms after the END of the received S1
     * (6.3.1.1.2 c); allow the modem + channel signal delay on top */
    printf("  v22bis: 2400 switch %.1f ms after caller's S1 ended, data mode at %.2f s\n",
           (o.a_2400_at - o.s1_end) / 8.0, o.ready_at / 8000.0);
    CHECK_NEAR((o.a_2400_at - o.s1_end) / 8.0, 600.0 + 9.0, 12.0);

    /* 2. V.22 caller (1200 only) */
    run(1, 0, -60.0, 0.15, 0.0, 8, &o);
    CHECK_EQ_I(o.rate_a, 1200); CHECK_EQ_I(o.rate_c, 1200);
    CHECK(o.cmp_ca >= 3000); CHECK_EQ_I(o.err_ca, 0); CHECK_EQ_I(o.err_ac, 0);

    /* 3. answerer limited to 1200, V.22bis caller */
    run(0, 1, -60.0, 0.15, 0.0, 8, &o);
    CHECK_EQ_I(o.rate_a, 1200); CHECK_EQ_I(o.rate_c, 1200);
    CHECK(o.cmp_ca >= 3000); CHECK_EQ_I(o.err_ca, 0); CHECK_EQ_I(o.err_ac, 0);

    /* 4. poor line: noise, strong echo, 150 ppm clock offset */
    run(1, 1, -42.0, 0.4, 150.0, 8, &o);
    CHECK_EQ_I(o.rate_a, 2400); CHECK_EQ_I(o.rate_c, 2400);
    printf("  v22bis poor line: %d/%d and %d/%d bit errors\n", o.err_ca, o.cmp_ca, o.err_ac, o.cmp_ac);
    CHECK(o.cmp_ca >= 3000 && o.err_ca * 1000 < o.cmp_ca);
    CHECK(o.cmp_ac >= 3000 && o.err_ac * 1000 < o.cmp_ac);

    /* 5. nobody there: the answerer keeps sending unscrambled ones, no false connect */
    {
        v22bis_t a; static side s; int n, bad = 0;
        memset(&s, 0, sizeof s);
        v22m_init(&a, V22_ANSWERER, 1, 3000, gb, pb, &s);
        for (n = 0; n < 4 * FS; n++) { v22m_tick(&a, 0); if (a.bitrate || a.tx.mode != V22_TX_USB1) bad++; }
        CHECK_EQ_I(bad, 0);
    }

    /* 6. a 1300 Hz calling tone (V.25 CT: 0.6 s on, 1.5 s off) is in our receive
     *    band but is not a V.22 caller: no false connect */
    {
        v22bis_t a; static side s; int n, bad = 0;
        memset(&s, 0, sizeof s);
        v22m_init(&a, V22_ANSWERER, 1, 3000, gb, pb, &s);
        for (n = 0; n < 9 * FS; n++) {
            int on = (n % (FS * 21 / 10)) < FS * 6 / 10;
            int16_t x = on ? (int16_t)lround(3000.0 * sin(2.0 * 3.14159265358979323846 * 1300.0 * n / FS)) : 0;
            v22m_tick(&a, x);
            if (a.bitrate || a.tx.mode != V22_TX_USB1) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }

    /* noise bursts (12 ms of amplitude 6000 every 700 ms, on both receivers)
     * cost data while they last but must not leave a receiver diverged: some
     * seconds after the last one both are back at a small error. Found by
     * the call-level tests, where one burst in a hundred did it for good. */
    {
        static v22bis_t a, c; static side sa2, sc2;
        int16_t atx = 0, ctx = 0, a_e = 0, c_e = 0;
        uint32_t rng = 12345; int n, worst_a = 0, worst_c = 0, hits = 0;
        memset(&sa2, 0, sizeof sa2); memset(&sc2, 0, sizeof sc2); sa2.l = 5; sc2.l = 6;
        v22m_init(&a, V22_ANSWERER, 1, 3000, gb, pb, &sa2);
        v22m_init(&c, V22_CALLER, 1, 3000, gb, pb, &sc2);
        for (n = 0; n < 140 * FS; n++) {
            int32_t ia = ctx + a_e / 5, ic = atx + c_e / 5;
            int bursting = n > 8 * FS && n < 125 * FS && (n % 5600) < 96;
            if (bursting) {
                rng = rng * 1664525u + 1013904223u; ia += (int)((rng >> 16) % 12001u) - 6000;
                rng = rng * 1664525u + 1013904223u; ic += (int)((rng >> 16) % 12001u) - 6000;
            }
            if (ia > 32767) ia = 32767;
            if (ia < -32768) ia = -32768;
            if (ic > 32767) ic = 32767;
            if (ic < -32768) ic = -32768;
            a_e = atx; c_e = ctx;
            atx = v22m_tick(&a, (int16_t)ia);
            ctx = v22m_tick(&c, (int16_t)ic);
            /* 3 s after each burst the receivers should have recovered */
            if (n > 8 * FS && n < 125 * FS && (n % 5600) == 5599 - 0 && 0) hits++;
            if (n > 8 * FS && (n % 5600) == 5500) {
                if ((int)a.rx.mse > worst_a) worst_a = (int)a.rx.mse;
                if ((int)c.rx.mse > worst_c) worst_c = (int)c.rx.mse;
            }
        }
        (void)hits;
        printf("  v22bis bursts: receiver error 0.7 s after a burst, worst: answerer %d, caller %d; at the end %d, %d\n",
               worst_a, worst_c, (int)a.rx.mse, (int)c.rx.mse);
        CHECK(a.rx.carrier && c.rx.carrier);
        CHECK_EQ_I(a.retrains + c.retrains, 0);       /* bursts are not loss of equalisation */
        CHECK_EQ_I(a.state, V22S_DATA); CHECK_EQ_I(c.state, V22S_DATA);
        CHECK(a.rx.mse < 400); CHECK(c.rx.mse < 400);
        CHECK(worst_a < 1000); CHECK(worst_c < 1000);
    }

    T_REPORT("v22bis");
}
