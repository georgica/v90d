/* Interop: our V.22bis ANSWERER against spandsp's independent V.22bis CALLER,
 * through the channel simulator, at both rates, with data in both directions.
 * Host only (`make test-interop`). */
#include <string.h>
#include "test.h"
#include "peer_spandsp.h"
#include "../modem/v22bis.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

#define FS 8000
#define NB 8000

typedef struct { uint32_t l; uint8_t tx[NB]; int ntx; uint8_t rx[NB + 4000]; int nrx; int sending; } side;
static int gb(void *u)
{
    side *s = u; int b;
    if (!s->sending) return 1;                       /* idle marks until both ends are up */
    s->l = s->l * 1664525u + 1013904223u; b = (s->l >> 24) & 1;
    if (s->ntx < NB) s->tx[s->ntx++] = (uint8_t)b;
    return b;
}
static void pb(void *u, int b) { side *s = u; if (s->sending && s->nrx < NB + 4000) s->rx[s->nrx++] = (uint8_t)b; }

static int errors(const side *from, const side *to, int *compared)
{
    int off, best = 1 << 30;
    *compared = 0;
    for (off = 0; off < 1500; off++) {
        int j, err = 0, cnt = 0;
        for (j = 0; j < from->ntx && j + off < to->nrx; j++, cnt++)
            if (to->rx[j + off] != from->tx[j]) err++;
        if (cnt >= 3000 && err < best) { best = err; *compared = cnt; }
    }
    return best;
}

typedef struct { int rate_us, rate_sp, sp_trained, up_at; int e_sp_us, c_sp_us, e_us_sp, c_us_sp; } outcome;

static void run(int our2400, int sp_rate, double noise, double echo, outcome *o)
{
    static const double one[1] = {1.0};
    static side us, sp;
    channel_cfg cfg; channel_t ch;
    v22bis_t a;
    int16_t atx = 0, ctx = 0;
    int n;

    memset(&us, 0, sizeof us); memset(&sp, 0, sizeof sp); us.l = 5; sp.l = 6;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = noise; cfg.us_noise_dbfs = noise;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = echo; cfg.echo_delay = 19; cfg.full_scale = 32124.0; cfg.seed = 3;
    channel_init(&ch, &cfg);
    v22m_init(&a, V22_ANSWERER, our2400, 3000, gb, pb, &us);
    sp_v22_start(sp_rate, 1, gb, pb, &sp);
    memset(o, 0, sizeof *o); o->up_at = -1;

    for (n = 0; n < 12 * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = v22m_tick(&a, (int16_t)ulaw_to_linear(srv));
        sp_v22_tick((int16_t)lround(client_rx), &ctx);
        atx = t;
        if (o->up_at < 0 && a.tx_ready && a.rx_ready && sp_v22_trained()) o->up_at = n;
        /* start the test data half a second after both ends report data mode */
        if (o->up_at >= 0 && n == o->up_at + FS / 2) { us.sending = 1; sp.sending = 1; }
    }
    o->rate_us = a.bitrate; o->rate_sp = sp_v22_rate(); o->sp_trained = sp_v22_trained();
    o->e_sp_us = errors(&sp, &us, &o->c_sp_us);
    o->e_us_sp = errors(&us, &sp, &o->c_us_sp);
    if (o->up_at < 0)
        printf("  not up: our state %d rdy %d%d sig %d mse %d rate %d | spandsp trained %d rate %d\n",
               a.state, a.tx_ready, a.rx_ready, a.rx.sig, (int)a.rx.mse, a.bitrate, sp_v22_trained(), sp_v22_rate());
    sp_v22_stop();
}

/* ---- longer calls with time-stamped bits: start-up in noise, and retrain ---- */
#define LB 120000
typedef struct { uint32_t l; uint8_t tx[LB]; int32_t tx_at[LB]; int ntx; uint8_t rx[LB]; int32_t rx_at[LB]; int nrx; int sending; } lside;
static int l_now;
static int lgb(void *u)
{
    lside *s = u; int b;
    if (!s->sending) return 1;
    s->l = s->l * 1664525u + 1013904223u; b = (s->l >> 24) & 1;
    if (s->ntx < LB) { s->tx[s->ntx] = (uint8_t)b; s->tx_at[s->ntx++] = l_now; }
    return b;
}
static void lpb(void *u, int b) { lside *s = u; if (s->nrx < LB) { s->rx[s->nrx] = (uint8_t)b; s->rx_at[s->nrx++] = l_now; } }

/* errors among the bits `from` sent at or after t_tx, looked for in what `to`
 * received at or after t_rx; -1 if under 1000 bits could be compared */
static int lerrors(const lside *from, const lside *to, int t_tx, int t_rx, int *compared)
{
    int i0 = 0, j0 = 0, d, best = -1, bd = 0, j;
    while (i0 < from->ntx && from->tx_at[i0] < t_tx) i0++;
    while (j0 < to->nrx && to->rx_at[j0] < t_rx) j0++;
    *compared = 0;
    for (d = 0; d < 4000; d++) {
        int err = 0;
        if (i0 + 1000 > from->ntx || j0 + d + 1000 > to->nrx) break;
        for (j = 0; j < 1000; j++) if (to->rx[j0 + d + j] != from->tx[i0 + j]) err++;
        if (best < 0 || err < best) { best = err; bd = d; }
    }
    if (best < 0) return -1;
    best = 0;
    for (j = 0; i0 + j < from->ntx && j0 + bd + j < to->nrx; j++) if (to->rx[j0 + bd + j] != from->tx[i0 + j]) best++;
    *compared = j;
    return best;
}

static lside lus, lsp;
static v22bis_t lm;
typedef struct {
    int up_at, rate_us, rate_sp;
    int rt_at;                   /* when the retrain was asked for */
    int us_s1_on, us_s1_off, us_2400, us_txrdy, us_rxnrdy, us_rxrdy;   /* our side of it (-1: did not happen) */
    int sp_rc, sp_ok_before, sp_ok_after, sp_prog, sp_fail;
    int zeros_clamped;           /* bits other than 1 delivered by us while clamped */
} lout;

/* retrain_by: 0 none, 1 spandsp asks, 2 we ask; 4 s after both are up */
static void lrun(double snr_db, uint32_t seed, int seconds, int retrain_by, lout *o)
{
    static const double one[1] = {1.0};
    channel_cfg cfg; channel_t ch;
    int16_t atx = 0, ctx = 0;
    int n; v22_txmode pm; int ptx = 0, prx = 0;

    memset(&lus, 0, sizeof lus); memset(&lsp, 0, sizeof lsp); lus.l = 5; lsp.l = 6;
    memset(&cfg, 0, sizeof cfg);
    /* SNR: signal RMS over noise RMS in 0..4 kHz; our level is 3000, spandsp's -14 dBm0 is about the same */
    cfg.ds_noise_dbfs = cfg.us_noise_dbfs = snr_db >= 90.0 ? -60.0 : 20.0 * log10(3000.0 / (32124.0 / sqrt(2.0))) - snr_db;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.15; cfg.echo_delay = 19; cfg.full_scale = 32124.0; cfg.seed = seed;
    channel_init(&ch, &cfg);
    l_now = 0;
    v22m_init(&lm, V22_ANSWERER, 1, 3000, lgb, lpb, &lus);
    sp_v22_start(2400, 1, lgb, lpb, &lsp);
    memset(o, 0, sizeof *o);
    o->up_at = o->rt_at = o->us_s1_on = o->us_s1_off = o->us_2400 = o->us_txrdy = o->us_rxnrdy = o->us_rxrdy = -1;
    pm = lm.tx.mode;

    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t; int nrx0;
        l_now = n;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        nrx0 = lus.nrx;
        t = v22m_tick(&lm, (int16_t)ulaw_to_linear(srv));
        if (o->us_rxnrdy >= 0 && o->us_rxrdy < 0 && prx == 0) { int j; for (j = nrx0; j < lus.nrx; j++) if (!lus.rx[j]) o->zeros_clamped++; }
        sp_v22_tick((int16_t)lround(client_rx), &ctx);
        atx = t;
        if (o->up_at < 0 && lm.tx_ready && lm.rx_ready && sp_v22_trained()) { o->up_at = n; ptx = prx = 1; }
        if (o->up_at >= 0 && n == o->up_at + FS / 2) { lus.sending = 1; lsp.sending = 1; }
        if (o->up_at >= 0 && retrain_by && n == o->up_at + 4 * FS) {
            o->rt_at = n; o->sp_ok_before = sp_v22_count(0);
            if (retrain_by == 1) o->sp_rc = sp_v22_retrain(2400); else v22m_retrain(&lm);
        }
        if (o->rt_at >= 0) {
            if (lm.tx.mode != pm) {
                if (lm.tx.mode == V22_TX_S1 && o->us_s1_on < 0) o->us_s1_on = n;
                if (pm == V22_TX_S1 && o->us_s1_off < 0) o->us_s1_off = n;
                if (lm.tx.mode == V22_TX_SB1_2400 && o->us_s1_off >= 0 && o->us_2400 < 0) o->us_2400 = n;
            }
            if (lm.tx_ready && !ptx && o->us_txrdy < 0) o->us_txrdy = n;
            if (!lm.rx_ready && prx && o->us_rxnrdy < 0) o->us_rxnrdy = n;
            if (lm.rx_ready && !prx && o->us_rxrdy < 0) o->us_rxrdy = n;
        }
        pm = lm.tx.mode; ptx = lm.tx_ready; prx = lm.rx_ready;
    }
    o->rate_us = lm.bitrate; o->rate_sp = sp_v22_rate();
    o->sp_ok_after = sp_v22_count(0); o->sp_prog = sp_v22_count(1); o->sp_fail = sp_v22_count(2);
    sp_v22_stop();
}

static void test_noise(void)
{
    static const double snr[] = { 26.0, 22.0, 20.0, 18.0, 16.0, 14.0, 12.0, 10.0, 8.0, 6.0 };
    int k, seed;
    printf("  start-up in steady noise, spandsp calling (2 noise seeds, 20 s calls):\n");
    for (k = 0; k < (int)(sizeof snr / sizeof snr[0]); k++) {
        int up = 0, r24 = 0, r12 = 0, e1 = 0, n1 = 0, e2 = 0, n2 = 0, rt = 0;
        double q[2];
        for (seed = 1; seed <= 2; seed++) {
            lout o; int e, c;
            lrun(snr[k], (uint32_t)seed, 20, 0, &o);
            q[seed - 1] = lm.s1_snr > 0 ? 10.0 * log10((double)lm.s1_snr) : -99.0;
            rt += lm.retrains;
            if (o.up_at < 0) continue;
            up++;
            if (o.rate_us == 2400 && o.rate_sp == 2400) r24++;
            if (o.rate_us == 1200 && o.rate_sp == 1200) r12++;
            e = lerrors(&lsp, &lus, o.up_at + FS, o.up_at + FS, &c); if (e >= 0) { e1 += e; n1 += c; }
            e = lerrors(&lus, &lsp, o.up_at + FS, o.up_at + FS, &c); if (e >= 0) { e2 += e; n2 += c; }
        }
        printf("    SNR %4.1f dB: up %d/2 (%d at 2400, %d at 1200), our retrains %d; errors spandsp->us %d/%d (%.1e), us->spandsp %d/%d (%.1e); S1 in-band SNR seen %.1f %.1f dB\n",
               snr[k], up, r24, r12, rt, e1, n1, n1 ? (double)e1 / n1 : 1.0, e2, n2, n2 ? (double)e2 / n2 : 1.0, q[0], q[1]);
        /* (spandsp sends about 2 dB louder than our 3000, so its direction is that much better off) */
        CHECK_EQ_I(up, 2); CHECK_EQ_I(r24 + r12, 2);              /* always up, both ends at the same rate */
        if (snr[k] >= 12.0) {
            CHECK_EQ_I(r24, 2); CHECK_EQ_I(rt, 0);
            CHECK(n1 > 50000 && e1 * 1000.0 < n1); CHECK(n2 > 50000 && e2 * 1000.0 < n2);
        }
        if (snr[k] <= 6.0) {
            CHECK_EQ_I(r12, 2);
            CHECK(n1 > 30000 && e1 * 1000.0 < n1); CHECK(n2 > 30000 && e2 * 1000.0 < n2);
        }
    }
}

/* Retrain with spandsp. What can be shown: it sends S1 when asked to retrain
 * and we recognise it; it answers our S1 with its own; our side then runs
 * 6.4 to the letter and delivers nothing but binary 1 while clamped.
 * What cannot: a completed retrain. After the S1 exchange this spandsp sends
 * scrambled ones at 1200 bit/s and stays there (watched for 12 s; it reports
 * 2400 bit/s all the while and its receiver takes our 2400 bit/s signal), so
 * the 32 ones at 2400 bit/s of 6.4 d never come. We repeat the retrain
 * signal as 6.4 says, then give the call up. */
static void test_retrain(void)
{
    int by;
    for (by = 1; by <= 2; by++) {
        lout o;
        lrun(99.0, 3, 34, by, &o);
        printf("  retrain asked for by %s (spandsp returned %d): our receiver clamped %.1f ms after the request, %d bits other than 1 delivered since;\n"
               "    our S1 began %.1f ms after the request and lasted %.1f ms; 2400 bit/s %.1f ms after our S1 began, data %.1f ms after that;\n"
               "    never 32 ones at 2400 from spandsp: retrains completed %d, retrain signals sent %d, our state at the end %d (7 = failed);\n"
               "    spandsp reports: training succeeded %d -> %d times, failed %d, rate %d\n",
               by == 1 ? "spandsp" : "us", o.sp_rc, (o.us_rxnrdy - o.rt_at) / 8.0, o.zeros_clamped,
               (o.us_s1_on - o.rt_at) / 8.0, (o.us_s1_off - o.us_s1_on) / 8.0,
               (o.us_2400 - o.us_s1_on) / 8.0, (o.us_txrdy - o.us_2400) / 8.0,
               lm.retrains, lm.rt_tries, lm.state, o.sp_ok_before, o.sp_ok_after, o.sp_fail, o.rate_sp);
        CHECK(o.up_at > 0 && o.rt_at > 0);
        CHECK(o.us_s1_on >= 0 && o.us_s1_off > 0 && o.us_2400 > 0 && o.us_txrdy > 0 && o.us_rxnrdy > 0);
        CHECK_NEAR((o.us_s1_off - o.us_s1_on) / 8.0, 100.0, 3.0);          /* 6.4 a */
        CHECK_NEAR((o.us_txrdy - o.us_2400) / 8.0, 200.0, 10.0);           /* 6.4 c */
        CHECK_EQ_I(o.zeros_clamped, 0);                                    /* circuit 104 at binary 1 */
        if (by == 1) {
            /* its S1 (100 ms) recognised while it lasts; ours follows its end; 2400 bit/s
             * 600 ms after that end, which we place 13 ms before we began our S1 */
            CHECK((o.us_rxnrdy - o.rt_at) / 8.0 > 20.0 && (o.us_rxnrdy - o.rt_at) / 8.0 < 70.0);
            CHECK((o.us_s1_on - o.rt_at) / 8.0 > 100.0 && (o.us_s1_on - o.rt_at) / 8.0 < 150.0);
            CHECK_NEAR((o.us_2400 - o.us_s1_on) / 8.0, 600.0 - 13.0, 10.0);
        } else {
            /* our S1 at once; reaching the timed part at all means spandsp's S1 came back,
             * within the 1.2 s window (it took its end to start our 600 ms) */
            CHECK_EQ_I(o.us_s1_on, o.rt_at);
            CHECK((o.us_2400 - o.us_s1_off) / 8.0 > 600.0 && (o.us_2400 - o.us_s1_off) / 8.0 < 1000.0);
        }
        /* we do not hang in a retrain that cannot finish */
        CHECK(lm.state == V22S_FAILED || lm.state == V22S_DATA);
    }
}

int main(void)
{
    outcome o;

    test_noise();
    test_retrain();

    /* spandsp V.22bis caller at 2400 */
    run(1, 2400, -55.0, 0.15, &o);
    printf("  vs spandsp 2400: up at %.2f s, rates %d/%d, errors sp->us %d/%d, us->sp %d/%d\n",
           o.up_at / 8000.0, o.rate_us, o.rate_sp, o.e_sp_us, o.c_sp_us, o.e_us_sp, o.c_us_sp);
    CHECK(o.up_at > 0);
    CHECK_EQ_I(o.rate_us, 2400); CHECK_EQ_I(o.rate_sp, 2400);
    CHECK(o.c_sp_us >= 3000); CHECK_EQ_I(o.e_sp_us, 0);
    CHECK(o.c_us_sp >= 3000); CHECK_EQ_I(o.e_us_sp, 0);

    /* spandsp as a 1200 bit/s (V.22) caller */
    run(1, 1200, -55.0, 0.15, &o);
    printf("  vs spandsp 1200: up at %.2f s, rates %d/%d, errors sp->us %d/%d, us->sp %d/%d\n",
           o.up_at / 8000.0, o.rate_us, o.rate_sp, o.e_sp_us, o.c_sp_us, o.e_us_sp, o.c_us_sp);
    CHECK(o.up_at > 0);
    CHECK_EQ_I(o.rate_us, 1200); CHECK_EQ_I(o.rate_sp, 1200);
    CHECK(o.c_sp_us >= 3000); CHECK_EQ_I(o.e_sp_us, 0);
    CHECK(o.c_us_sp >= 3000); CHECK_EQ_I(o.e_us_sp, 0);

    /* we are limited to 1200, spandsp offers 2400: both fall back */
    run(0, 2400, -55.0, 0.15, &o);
    CHECK(o.up_at > 0);
    CHECK_EQ_I(o.rate_us, 1200); CHECK_EQ_I(o.rate_sp, 1200);
    CHECK_EQ_I(o.e_sp_us, 0); CHECK_EQ_I(o.e_us_sp, 0);

    /* 2400 on a noisy, echoey line */
    run(1, 2400, -42.0, 0.4, &o);
    printf("  vs spandsp 2400 poor line: errors sp->us %d/%d, us->sp %d/%d\n", o.e_sp_us, o.c_sp_us, o.e_us_sp, o.c_us_sp);
    CHECK(o.up_at > 0);
    CHECK(o.c_sp_us >= 3000 && o.e_sp_us * 1000 < o.c_sp_us);
    CHECK(o.c_us_sp >= 3000 && o.e_us_sp * 1000 < o.c_us_sp);

    T_REPORT("interop_v22bis_spandsp");
}
