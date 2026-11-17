/* V.32bis echo canceller: convergence on the TRN segment through a hybrid
 * (near echo) plus a delayed far echo, the attenuation it then holds while
 * the far end talks, and the bulk delay. */
#include "test.h"
#include "v32_priv.h"
#include "noise.h"

#define MAXD 8192
static double hist[MAXD]; static int hp;

static int ones(void *u) { (void)u; return 1; }

/* echo path: near = a short dispersive hybrid at about -15 dB; far = the
 * transmit signal `delay` samples later through another short response */
static double echo(double tx, int delay, double far_gain)
{
    static const double hn[8] = { 0.02, 0.11, -0.13, 0.06, -0.03, 0.015, -0.008, 0.004 };
    static const double hf[5] = { 0.3, 0.8, -0.4, 0.2, -0.1 };
    double v = 0; int i;
    hist[hp] = tx;
    for (i = 0; i < 8; i++) v += hn[i] * hist[(hp - 3 - i) & (MAXD - 1)];
    for (i = 0; i < 5; i++) v += far_gain * hf[i] * hist[(hp - delay - i) & (MAXD - 1)];
    hp = (hp + 1) & (MAXD - 1);
    return v;
}

/* returns the echo attenuation in dB over the last `tail` samples of TRN;
 * far_rms > 0 then adds a far-end signal and runs `dt` more samples at
 * step 2^-dt_mu, returning the attenuation of the echo alone there */
static double run(int delay, double far_gain, int bulk, int trn_syms, int dt, int dt_mu, double far_rms, double *dt_db)
{
    static v32ec_t ec;
    v32tx_t tx, far;
    rng_t rng;
    double pe = 0, pr = 0;
    int n, tail = 2000, total = trn_syms * 10 / 3, i;
    for (i = 0; i < MAXD; i++) hist[i] = 0;
    hp = 0;
    rng_seed(&rng, 5);
    v32ec_init(&ec);
    if (bulk >= 0) v32ec_set_bulk(&ec, bulk);
    v32tx_init(&tx, 1, 4000, ones, 0);
    v32tx_init(&far, 0, (int)far_rms, ones, 0);
    v32tx_set_mode(&tx, V32_TX_TRN);
    for (n = 0; n < total + dt; n++) {
        int16_t s;
        double e, f = 0;
        int32_t rx, out;
        int mu;
        if (n == total) { tx.rate = 4; v32tx_set_mode(&tx, V32_TX_ONES); far.rate = 4; v32tx_set_mode(&far, V32_TX_ONES); pe = pr = 0; }
        s = v32tx_tick(&tx);
        e = echo(s, delay, far_gain) + 0.3 * rng_gauss(&rng);
        if (n >= total) f = v32tx_tick(&far);
        rx = (int32_t)lrint(e + f);
        /* gear: full step, then 1/4 for the last quarter */
        mu = n >= total ? dt_mu : (n < total * 3 / 4 ? 0 : 2);
        out = v32ec_tick(&ec, tx.ref, rx, mu);
        if (n < total ? n >= total - tail : n >= total + dt - 8000) { pe += (out - f) * (out - f); pr += e * e; }
        if (n == total - 1 && dt) { *dt_db = 0; }
        if (n == total - 1) { double a = 10 * log10(pr / pe); if (!dt) return a; *dt_db = a; }
    }
    { double a = *dt_db; *dt_db = 10 * log10(pr / pe); return a; }
}

int main(void)
{
    double a, b = 0;
    /* near echo only, minimum TRN (1280 symbols) and a longer one */
    a = run(0, 0.0, -1, 1280, 0, 0, 0, 0);
    printf("  near echo, 1280 symbols of TRN: %.1f dB\n", a);
    CHECK(a > 45);
    a = run(0, 0.0, -1, 3000, 0, 0, 0, 0);
    printf("  near echo, 3000 symbols of TRN: %.1f dB\n", a);
    CHECK(a > 45);
    /* near + far echo 40 ms away at -12 dB re near: window from 35 ms */
    a = run(320, 0.25, 320 - 40, 3000, 0, 0, 0, 0);
    printf("  near + far echo at 40 ms, bulk delay set: %.1f dB\n", a);
    CHECK(a > 45);
    /* the same far echo with the window in the wrong place is not cancelled */
    a = run(320, 0.25, 1000, 3000, 0, 0, 0, 0);
    printf("  near + far echo at 40 ms, bulk delay wrong: %.1f dB\n", a);
    CHECK(a < 20);
    /* a satellite hop: 600 ms */
    a = run(4800, 0.25, 4800 - 40, 3000, 0, 0, 0, 0);
    printf("  near + far echo at 600 ms: %.1f dB\n", a);
    CHECK(a > 45);
    /* double talk: far-end signal 6 dB below our transmit level, 16 s. The
     * wanted signal is noise to the adaptation: only a crawl is harmless */
    a = run(320, 0.25, 320 - 40, 3000, 128000, -1, 2000, &b);
    printf("  then 16 s of double talk, frozen: %.1f dB before, %.1f dB after\n", a, b);
    CHECK(b > 45);
    a = run(320, 0.25, 320 - 40, 3000, 128000, 15, 2000, &b);
    printf("  then 16 s of double talk at step 2^-15: %.1f dB before, %.1f dB after\n", a, b);
    CHECK(b > 40);
    a = run(320, 0.25, 320 - 40, 3000, 128000, 11, 2000, &b);
    printf("  then 16 s of double talk at step 2^-11: %.1f dB after (too fast)\n", b);
    CHECK(b < 40);
    a = run(320, 0.25, 320 - 40, 3000, 16000, 2, 2000, &b);
    printf("  double talk at the training step 2^-2: %.1f dB after\n", b);
    CHECK(b < 10);
    T_REPORT("test_ec");
}
