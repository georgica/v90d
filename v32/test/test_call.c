/* End-to-end calls: the answering modem (v32/, fixed point) against the
 * calling test peer (sim/v32call, double) over the simulated 2-wire line,
 * from the first AC to data, then a few seconds of pseudo-random data both
 * ways with every bit checked. One line of results per call. */
#include "test.h"
#include "calltest.h"

static call_t k;

#ifdef __arm__
#define SECS 2
#else
#define SECS 8
#endif

#define A_LEVEL 3000            /* -14.4 dBm0 */

typedef struct { int ok, rate; long ab, ae, cb, ce; } res_t;

/* one call. a_rates / c_rates: what each end offers. loss: line loss each
 * way. Returns what happened; prints one line. */
static res_t call(const char *name, v32line_cfg *lc, unsigned a_rates, unsigned c_rates, int secs)
{
    res_t r;
    memset(&r, 0, sizeof r);
    call_init(&k, lc, a_rates, c_rates, A_LEVEL, A_LEVEL);
    r.ok = call_connect(&k, 8000L * 30);
    if (!r.ok) {
        printf("  %-34s NO CONNECTION (answerer state %d error %d, caller state %d)\n", name, k.m.state, k.m.error, k.c.state);
        return r;
    }
    call_run(&k, 8000L * secs);
    r.rate = k.m.bitrate;
    r.ab = k.a_chk.bits; r.ae = k.a_chk.errs; r.cb = k.c_chk.bits; r.ce = k.c_chk.errs;
    printf("  %-34s %5d bit/s  c->a %6ld bits %5ld errors   a->c %6ld bits %5ld errors   echo %2d dB  mse %4d%s\n",
           name, r.rate, r.ab, r.ae, r.cb, r.ce, k.m.erle_db10 / 10, (int)k.m.rx.mse,
           k.m.retrains ? "  (retrained)" : "");
    return r;
}

static const int bitrate[5] = { 4800, 7200, 9600, 12000, 14400 };

/* every rate on one line; the bit counts must be there and the errors at most `maxerr` */
static void all_rates(const char *what, const v32line_cfg *base, long maxerr)
{
    int r;
    for (r = 4; r >= 0; r--) {
        v32line_cfg lc = *base;
        char name[64];
        res_t x;
        snprintf(name, sizeof name, "%s, %d", what, bitrate[r]);
        x = call(name, &lc, 1u << r, 1u << r, SECS);
        CHECK(x.ok);
        CHECK_EQ_I(x.rate, bitrate[r]);
        CHECK(x.ab > (long)bitrate[r] * (SECS - 1)); CHECK(x.cb > (long)bitrate[r] * (SECS - 1));
        CHECK(x.ae <= maxerr); CHECK(x.ce <= maxerr);
    }
}

/* 7.2: a retrain from either end in data mode: back through the whole
 * start-up from AC / AA, data again afterwards */
static void retrain1(const char *name, int from_caller)
{
    v32line_cfg lc;
    long t0;
    int ok, seen = 0;
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    call_init(&k, &lc, V32_RALL, V32_RALL, A_LEVEL, A_LEVEL);
    ok = call_connect(&k, 8000L * 30);
    CHECK(ok);
    call_run(&k, 8000L * 2);
    CHECK_EQ_I(k.a_chk.errs, 0); CHECK_EQ_I(k.c_chk.errs, 0);
    CHECK(k.a_chk.bits > 20000); CHECK(k.c_chk.bits > 20000);
    if (from_caller) CHECK_EQ_I(v32call_retrain(&k.c), 0); else CHECK_EQ_I(v32_retrain(&k.m), 0);
    t0 = k.n;
    /* both ends must notice, stop passing data, and come back */
    while (k.n - t0 < 8000L * 4 && !seen) {
        call_tick(&k);
        seen = k.m.retraining && k.c.state != VCS_DATA;
    }
    CHECK(seen);
    printf("  %-34s both ends retraining %.0f ms after the request;", name, (k.n - t0) / 8.0);
    CHECK_EQ_I(k.m.connected, 1);                /* still a call: connected stays up through a retrain */
    call_reset_data(&k);
    ok = call_connect(&k, 8000L * 30);
    CHECK(ok);
    printf(" data again after %.2f s at %d bit/s;", (k.n - t0) / 8000.0, k.m.bitrate);
    CHECK_EQ_I(k.m.retrains, 1); CHECK_EQ_I(k.c.retrains, 1);
    call_run(&k, 8000L * SECS);
    printf(" then c->a %ld bits %ld errors, a->c %ld bits %ld errors\n", k.a_chk.bits, k.a_chk.errs, k.c_chk.bits, k.c_chk.errs);
    CHECK(k.a_chk.bits > (long)k.m.bitrate * (SECS - 1)); CHECK(k.c_chk.bits > (long)k.m.bitrate * (SECS - 1));
    CHECK_EQ_I(k.a_chk.errs, 0); CHECK_EQ_I(k.c_chk.errs, 0);
}

static void retrains(void)
{
    printf(" retrain (7.2), mu-law, echo\n");
    retrain1("asked for by the caller", 1);
#ifndef __arm__
    retrain1("asked for by the answerer", 0);
#endif
    {   /* not in data mode: refused */
        v32_t m; v32_cfg c;
        memset(&c, 0, sizeof c); c.level = 3000; c.rates = V32_RALL; c.getbit = a_get; c.putbit = a_put; c.user = &k;
        v32_init(&m, &c);
        CHECK_EQ_I(v32_retrain(&m), -1);
    }
}

#ifndef __arm__
/* a driver's buffering between the modem and the line: the near echo starts
 * late (cfg.echo_delay), and the 64 T turn-round must still hold at the line */
static void io_delay(void)
{
    static const int dl[] = { 40, 100, 160 };
    unsigned i;
    printf(" driver buffering between the modem and the line (mu-law, echo), echo_delay set to match\n");
    for (i = 0; i < sizeof dl / sizeof dl[0]; i++) {
        v32line_cfg lc;
        char name[64];
        res_t x;
        double turn;
        line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
        lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
        k.io_delay = dl[i];
        snprintf(name, sizeof name, "%d samples (%.1f ms)", dl[i], dl[i] / 8.0);
        x = call(name, &lc, V32_RALL, V32_RALL, SECS);
        turn = ((double)k.m.t_ac2 + dl[i] - ((double)k.c.t_cc + k.line.peer_lag + k.line.oneway_back)) * 0.3;
        printf("  %-34s turn-round at the line %.1f T\n", "", turn);
        k.io_delay = 0;
        CHECK(x.ok); CHECK_EQ_I(x.rate, 14400);
        CHECK_EQ_I((int)x.ae, 0); CHECK_EQ_I((int)x.ce, 0);
        /* up to about 150 samples the reversal can still be sent in time */
        if (dl[i] <= 100) CHECK_NEAR(turn, 64, 2);
    }
}

#endif

/* the caller hangs up in data mode: the answerer must notice, with its own
 * echo still coming back */
static int gone;
static double leaving_peer(void *u, double rx) { double tx = v32call_tick(&((call_t *)u)->c, rx); return gone ? 0 : tx; }

static void carrier_loss(void)
{
    v32line_cfg lc;
    long n, t0 = 0;
    printf(" the caller disappears in data mode (near echo 15 dB down stays)\n");
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    call_init(&k, &lc, V32_RALL, V32_RALL, A_LEVEL, A_LEVEL);
    gone = 0;
    for (n = 0; n < 8000L * 20; n++) {
        int16_t tx = v32_tick(&k.m, k.a_rx);
        k.a_rx = v32line_tick(&k.line, tx, leaving_peer, &k);
        if (!gone && k.m.connected && k.c.connected && ++t0 > 16000) { gone = 1; t0 = n; CHECK_EQ_I(k.m.carrier, 1); }
        if (gone && k.m.state == V32S_FAILED) break;
    }
    CHECK(gone);
    CHECK_EQ_I(k.m.state, V32S_FAILED);
    CHECK_EQ_I(k.m.error, V32_ERR_CARRIER);
    CHECK_EQ_I(k.m.connected, 0);
    CHECK(n - t0 > 8 * 400 && n - t0 < 8 * 700);
    printf("  answerer: error %d (carrier lost) %.0f ms after the caller went silent\n", k.m.error, (n - t0) / 8.0);
}

#ifndef __arm__
/* the line gets worse in data mode: the answerer retrains by itself and comes
 * back at a rate the line carries */
static void auto_retrain(void)
{
    v32line_cfg lc;
    long n, t0;
    int rate0;
    double rx_dbm0 = 20 * log10(A_LEVEL / v32line_rms(0)) - 6;
    printf(" the line turns noisy in data mode (linear, echo; SNR 40 dB, then 19 dB)\n");
    line_defaults(&lc); lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = rx_dbm0 - 40;
    call_init(&k, &lc, V32_RALL, V32_RALL, A_LEVEL, A_LEVEL);
    CHECK(call_connect(&k, 8000L * 30));
    call_run(&k, 8000L * 2);
    rate0 = k.m.bitrate;
    CHECK_EQ_I(rate0, 14400); CHECK_EQ_I(k.a_chk.errs, 0); CHECK_EQ_I(k.m.retrains, 0);
    k.line.cfg.noise_a_dbm0 = k.line.cfg.noise_c_dbm0 = rx_dbm0 - 19;
    t0 = k.n;
    for (n = 0; n < 8000L * 10 && !k.m.retrains; n++) call_tick(&k);
    CHECK_EQ_I(k.m.retrains, 1);
    printf("  14400 bit/s; noise up: %ld errors in %ld bits, retrain begun after %.2f s;", k.a_chk.errs, k.a_chk.bits, (k.n - t0) / 8000.0);
    for (n = 0; n < 8000L && !(k.m.retraining && k.c.state != VCS_DATA); n++) call_tick(&k);
    call_reset_data(&k);
    CHECK(call_connect(&k, 8000L * 30));
    call_run(&k, 8000L * SECS);
    printf(" back at %d bit/s: c->a %ld bits %ld errors, a->c %ld bits %ld errors\n", k.m.bitrate, k.a_chk.bits, k.a_chk.errs, k.c_chk.bits, k.c_chk.errs);
    CHECK(k.m.bitrate < rate0);
    CHECK(k.a_chk.bits > (long)k.m.bitrate * (SECS - 1));
    CHECK(k.a_chk.errs <= 5); CHECK(k.c_chk.errs <= 5);
    CHECK_EQ_I(k.m.retrains, 1);
}

#endif

/* nothing in common: the call must end with both ends silent, not hang */
static void no_rate(void)
{
    v32line_cfg lc;
    int ok;
    long i; double pw = 0;
    printf(" no rate in common (answerer 14400 only, caller 7200 only)\n");
    line_defaults(&lc);
    call_init(&k, &lc, V32_R14400, V32_R7200, A_LEVEL, A_LEVEL);
    ok = call_connect(&k, 8000L * 30);
    CHECK(!ok);
    CHECK_EQ_I(k.m.state, V32S_FAILED);
    CHECK_EQ_I(k.m.error, V32_ERR_NO_RATE);
    CHECK_EQ_I(k.m.connected, 0);
    for (i = 0; i < 4000; i++) { int16_t tx = v32_tick(&k.m, 0); if (i >= 400) pw += (double)tx * tx; }
    CHECK(pw == 0);
    printf("  answerer gave up after %.2f s with error %d (no rate), then silent\n", k.n / 8000.0, k.m.error);
}

int main(void)
{
    v32line_cfg lc;
    double rx_dbm0;

    /* (under qemu the double-precision peer is slow: the ARM build runs one
     * group at every rate and one or two calls of the others) */
#ifndef __arm__
    printf(" clean line (6 dB loss, no echo, linear)\n");
    line_defaults(&lc);
    all_rates("clean", &lc, 0);

    printf(" G.711 mu-law at the answerer's interface (sim/channel)\n");
    line_defaults(&lc); lc.mulaw = 1;
    all_rates("mu-law", &lc, 0);
#endif

    printf(" hybrid echo: near 15 dB down at both ends, far echo 20 dB down, 25 ms each way\n");
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    all_rates("echo", &lc, 0);

    printf(" low level: -30 dBm0 received at both ends, with the same echo (near echo 0 dB re signal)\n");
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.loss_db = 15.6;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
#ifndef __arm__
    all_rates("-30 dBm0", &lc, 0);
#else
    { res_t x = call("-30 dBm0, any rate", &lc, V32_RALL, V32_RALL, SECS);
      CHECK(x.ok); CHECK_EQ_I(x.rate, 14400); CHECK_EQ_I((int)x.ae, 0); CHECK_EQ_I((int)x.ce, 0); }
#endif

#ifndef __arm__
    printf(" sample clock offset (caller against network), with echo\n");
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.ppm = 100;  all_rates("+100 ppm", &lc, 0);
    lc.ppm = -100; all_rates("-100 ppm", &lc, 0);

    printf(" carrier offset (each way), near echo only\n");
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.c_near_db = 15;
    lc.freq_offset = 2;  all_rates("+2 Hz", &lc, 0);
    lc.freq_offset = -7; all_rates("-7 Hz", &lc, 0);

    /* with a far echo too, that echo comes back shifted by twice the offset
     * and the canceller, which does not track its phase, loses it: reported,
     * not asserted (see "Not done" in the README) */
    printf(" carrier offset with the far echo as well (20 dB down; its phase roll is not tracked): reported only\n");
    {
        static const double fo[] = { 0.2, 1, 2, 7 };
        unsigned i;
        for (i = 0; i < sizeof fo / sizeof fo[0]; i++) {
            char name[64];
            line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
            lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
            lc.freq_offset = fo[i];
            snprintf(name, sizeof name, "%.1f Hz + far echo, any rate", fo[i]);
            call(name, &lc, V32_RALL, V32_RALL, SECS);
        }
    }
#endif

    /* noise: the received level is A_LEVEL less the loss */
    rx_dbm0 = 20 * log10(A_LEVEL / v32line_rms(0)) - 6;
    printf(" white noise at both receivers (signal %.1f dBm0), mu-law, echo; one rate offered\n", rx_dbm0);
    {
        static const struct { int r; double snr; long maxerr; } nz[] = {
            /* -1: reported only (the rate is being pushed past where it works) */
            { 4, 32, 0 }, { 4, 26, 0 }, { 4, 22, -1 }, { 4, 20, -1 },
            { 3, 28, 0 }, { 3, 23, 0 }, { 3, 19, -1 },
            { 2, 24, 0 }, { 2, 20, 0 }, { 2, 16, -1 },
            { 1, 20, 0 }, { 1, 17, 0 }, { 1, 13, -1 },
            { 0, 16, 0 }, { 0, 13, 20 }, { 0, 9, -1 },
        };
        unsigned i;
        for (i = 0; i < sizeof nz / sizeof nz[0]; i++) {
            char name[64]; res_t x;
#ifdef __arm__
            if (!(nz[i].r == 4 && nz[i].snr == 26) && !(nz[i].r == 0 && nz[i].snr == 13)) continue;
#endif
            line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
            lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
            lc.noise_a_dbm0 = lc.noise_c_dbm0 = rx_dbm0 - nz[i].snr;
            snprintf(name, sizeof name, "SNR %2.0f dB, %d", nz[i].snr, bitrate[nz[i].r]);
            k.no_auto_retrain = 1;          /* the error rate of this rate at this SNR is what is wanted */
            x = call(name, &lc, 1u << nz[i].r, 1u << nz[i].r, SECS);
            k.no_auto_retrain = 0;
            if (nz[i].maxerr < 0) continue;
            CHECK(x.ok);
            CHECK(x.ae <= nz[i].maxerr); CHECK(x.ce <= nz[i].maxerr);
        }
    }
    printf(" the same, all rates offered: the answerer picks from its receiver's error\n");
    {
        static const double snr[] = { 40, 32, 28, 24, 20, 16, 12 };
        unsigned i;
        for (i = 0; i < sizeof snr / sizeof snr[0]; i++) {
            char name[64]; res_t x;
#ifdef __arm__
            if (snr[i] != 20) continue;
#endif
            line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
            lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
            lc.noise_a_dbm0 = lc.noise_c_dbm0 = rx_dbm0 - snr[i];
            snprintf(name, sizeof name, "SNR %2.0f dB, any rate", snr[i]);
            x = call(name, &lc, V32_RALL, V32_RALL, SECS);
            CHECK(x.ok);
            CHECK(x.ae <= 20); CHECK(x.ce <= 20);
        }
    }
    retrains();
#ifndef __arm__
    io_delay();
    auto_retrain();
#endif
    carrier_loss();
    no_rate();
    T_REPORT("test_call");
}
