/* The hand-over into V.32bis, the way modem/answer.c would do it:
 *  1. a V.8 caller: ANSam, CM / JM / CJ (modem/v8ans against sim/v8call) with
 *     V.32 as the lowest common modulation, 75 ms of silence, then the V.32bis
 *     start-up from AC and data;
 *  2. the AA detector on its own: 1800 Hz under the echo of our answer tone,
 *     and what must not set it off;
 *  3. a caller without V.8 (V.32bis 6.1 / A.2.2): it answers the answer tone
 *     with AA; the answerer sees that during ANSam and goes to AC. */
#include "test.h"
#include "calltest.h"
#include "v8ans.h"
#include "v8call.h"
#include "ansam.h"

static call_t k;
static v8ans_t va;
static v8call_t vc;
static long c_wait;                 /* part 3: caller's samples before it sends AA */

#ifdef __arm__
#define SECS 2
#else
#define SECS 5
#endif

/* caller's end: V.8 first, then the V.32bis caller */
static double v8_peer(void *u, double rx)
{
    (void)u;
    if (vc.state != V8C_DONE) {
        int16_t tx = 0;
        long q = lrint(rx);
        v8call_tick(&vc, (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q), &tx);
        return tx;
    }
    return v32call_tick(&k.c, rx);
}

/* caller's end without V.8: silent while it listens to a second of answer tone, then AA */
static double legacy_peer(void *u, double rx)
{
    (void)u;
    if (c_wait > 0) { c_wait--; return 0; }
    return v32call_tick(&k.c, rx);
}

static void test_v8_call(void)
{
    v8_menu_t local, cm;
    v32line_cfg lc;
    v32_cfg ac;
    int v32_on = 0;
    long t_done = 0, t_conn = 0, n;

    printf(" V.8, then V.32bis (mu-law, near echo 15 dB, far echo 20 dB down)\n");
    memset(&local, 0, sizeof local); memset(&cm, 0, sizeof cm);
    local.call_function = V8_CALLF_DATA; local.modes = V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21; local.n_mod_octets = 3;
    cm.call_function = V8_CALLF_DATA; cm.modes = V8_MOD_V32 | V8_MOD_V22; cm.n_mod_octets = 3;
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    call_init(&k, &lc, V32_RALL, V32_RALL, 3000, 3000);        /* the line and the V.32bis caller */
    v8ans_init(&va, 8000, 3000 * 181 / 128, &local);
    v8call_init(&vc, 8000, 4243, &cm);
    ac = k.m.cfg;

    for (n = 0; n < 8000L * 40; n++) {
        int16_t tx = 0;
        if (!v32_on) {
            if (v8ans_tick(&va, k.a_rx, &tx) == V8A_DONE) {
                /* the 75 ms of silence are over: straight into AC */
                CHECK_EQ_I(va.result, V8A_RES_OK);
                CHECK_EQ_I(va.selected, 3);                     /* Table 4/V.8 item 3: V.32 / V.32bis */
                if (va.result != V8A_RES_OK || va.selected != 3) break;
                v32_init(&k.m, &ac);
                v32_on = 1; t_done = n;
            }
        } else tx = v32_tick(&k.m, k.a_rx);
        k.a_rx = v32line_tick(&k.line, tx, v8_peer, &k);
        k.n++;
        if (v32_on && k.m.connected && k.c.connected) { t_conn = n; break; }
        if (v32_on && (k.m.state == V32S_FAILED || k.c.state == VCS_FAILED)) break;
    }
    CHECK(v32_on); CHECK(t_conn > 0);
    if (!t_conn) { printf("  FAILED: V.8 result %d, answerer state %d error %d, caller state %d\n", va.result, k.m.state, k.m.error, k.c.state); return; }
    for (n = 0; n < 8000L * SECS; n++) {
        int16_t tx = v32_tick(&k.m, k.a_rx);
        k.a_rx = v32line_tick(&k.line, tx, v8_peer, &k);
    }
    printf("  V.8 done at %.2f s (selected %d), V.32bis data at %.2f s, %d bit/s; c->a %ld bits %ld errors, a->c %ld bits %ld errors\n",
           t_done / 8000.0, va.selected, t_conn / 8000.0, k.m.bitrate, k.a_chk.bits, k.a_chk.errs, k.c_chk.bits, k.c_chk.errs);
    CHECK_EQ_I(k.m.bitrate, 14400);
    CHECK(k.a_chk.bits > 14400L * (SECS - 1)); CHECK(k.c_chk.bits > 14400L * (SECS - 1));
    CHECK_EQ_I(k.a_chk.errs, 0); CHECK_EQ_I(k.c_chk.errs, 0);
}

/* ---- the detector alone ---- */
#define PI 3.14159265358979323846
static uint32_t rnd = 12345;
static double noise1(void) { rnd = rnd * 1664525u + 1013904223u; return ((rnd >> 8) / 8388608.0 - 1.0) * 1.7320508; }   /* unit variance */

/* run `n` samples of ANSam echo (rms `echo`), a tone f at rms `tone` starting
 * at sample `t0`, noise rms `nz`; returns the sample of first detection or -1 */
static long aadet_run(double echo, double f, double tone, long t0, double nz, long n)
{
    v32_aadet_t d;
    ansam_t an;
    long i, hit = -1;
    v32_aadet_init(&d);
    ansam_init(&an, 8000, 16384, 1);
    for (i = 0; i < n; i++) {
        int16_t a;
        double x;
        ansam_run(&an, &a, 1);
        x = a / 16384.0 * echo * 1.41421356 + nz * noise1();
        if (i >= t0) x += tone * 1.41421356 * sin(2 * PI * f * i / 8000.0 + 1.0);
        if (v32_aadet_tick(&d, (int16_t)lrint(x)) && hit < 0) hit = i;
    }
    return hit;
}

static void test_aadet(void)
{
    double r0 = v32line_rms(0);
    long t;
    printf(" AA detector (1800 Hz during the answer tone)\n");
    /* AA at -20, -30, -40 dBm0 under an echo of the answer tone at -10 dBm0 */
    t = aadet_run(v32line_rms(-10), 1800, v32line_rms(-20), 8000, v32line_rms(-50), 24000);
    printf("  AA -20 dBm0, answer tone echo -10 dBm0: detected %.0f ms after it starts\n", (t - 8000) / 8.0);
    CHECK(t >= 8000 + V32_AADET_RUN - 1 && t < 8000 + 1600);
    t = aadet_run(v32line_rms(-10), 1800, v32line_rms(-30), 8000, v32line_rms(-50), 24000);
    printf("  AA -30 dBm0, answer tone echo -10 dBm0: detected %.0f ms after it starts\n", (t - 8000) / 8.0);
    CHECK(t >= 8000 + V32_AADET_RUN - 1 && t < 8000 + 1600);
    t = aadet_run(v32line_rms(-10), 1800, v32line_rms(-40), 8000, v32line_rms(-55), 24000);
    printf("  AA -40 dBm0, answer tone echo -10 dBm0: detected %.0f ms after it starts\n", (t - 8000) / 8.0);
    CHECK(t >= 8000 + V32_AADET_RUN - 1 && t < 8000 + 2000);
    /* 1793 and 1807 Hz: still AA (6.2: 1800 +- 7 Hz) */
    t = aadet_run(v32line_rms(-10), 1793, v32line_rms(-30), 8000, v32line_rms(-50), 24000);
    CHECK(t >= 8000 + V32_AADET_RUN - 1 && t < 8000 + 1600);
    t = aadet_run(v32line_rms(-10), 1807, v32line_rms(-30), 8000, v32line_rms(-50), 24000);
    CHECK(t >= 8000 + V32_AADET_RUN - 1 && t < 8000 + 1600);
    printf("  1793 Hz and 1807 Hz at -30 dBm0: detected\n");
    /* not AA */
    t = aadet_run(v32line_rms(-6), 1800, 0, 0, v32line_rms(-50), 40000);
    printf("  answer tone echo alone at -6 dBm0, 5 s: %s\n", t < 0 ? "nothing" : "FALSE DETECTION");
    CHECK(t < 0);
    t = aadet_run(v32line_rms(-10), 1800, v32line_rms(-48), 0, v32line_rms(-60), 40000);
    printf("  1800 Hz at -48 dBm0 (below the -43 dBm0 threshold): %s\n", t < 0 ? "nothing" : "FALSE DETECTION");
    CHECK(t < 0);
    t = aadet_run(v32line_rms(-10), 1650, v32line_rms(-20), 0, v32line_rms(-60), 40000);
    CHECK(t < 0);
    t = aadet_run(v32line_rms(-10), 1180, v32line_rms(-12), 0, v32line_rms(-60), 40000);
    CHECK(t < 0);
    t = aadet_run(v32line_rms(-10), 980, v32line_rms(-12), 0, v32line_rms(-60), 40000);
    CHECK(t < 0);
    printf("  1650 Hz at -20 dBm0, V.21 CM tones (980 / 1180 Hz) at -12 dBm0: nothing\n");
    t = aadet_run(0, 1800, 0, 0, v32line_rms(-15), 80000);
    printf("  white noise at -15 dBm0, 10 s: %s\n", t < 0 ? "nothing" : "FALSE DETECTION");
    CHECK(t < 0);
    (void)r0;
}

/* ---- a caller without V.8 ---- */
static void test_legacy(void)
{
    v8_menu_t local;
    v32line_cfg lc;
    v32_cfg ac;
    v32_aadet_t det;
    int phase = 0;                  /* 0: answer tone, 1: 75 ms silent, 2: V.32bis */
    long t_aa = 0, t_conn = 0, n, gap = 0;

    printf(" a V.32bis caller without V.8: AA during the answer tone\n");
    memset(&local, 0, sizeof local);
    local.call_function = V8_CALLF_DATA; local.modes = V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21; local.n_mod_octets = 3;
    line_defaults(&lc); lc.mulaw = 1; lc.delay = 200;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    call_init(&k, &lc, V32_RALL, V32_RALL, 3000, 3000);
    v8ans_init(&va, 8000, 3000 * 181 / 128, &local);
    v32_aadet_init(&det);
    ac = k.m.cfg;
    c_wait = 8000L * 14 / 10;       /* 0.2 s of silence + a good second of answer tone (6.1) */

    for (n = 0; n < 8000L * 40; n++) {
        int16_t tx = 0;
        if (phase == 0) {
            v8ans_tick(&va, k.a_rx, &tx);
            if (v32_aadet_tick(&det, k.a_rx)) { phase = 1; t_aa = n; gap = 600; }
            else if (va.state == V8A_DONE) break;               /* (V.8 took over or timed out: not this test) */
        } else if (phase == 1) {
            if (--gap == 0) { v32_init(&k.m, &ac); phase = 2; }
        } else tx = v32_tick(&k.m, k.a_rx);
        k.a_rx = v32line_tick(&k.line, tx, legacy_peer, &k);
        k.n++;
        if (phase == 2 && k.m.connected && k.c.connected) { t_conn = n; break; }
        if (phase == 2 && (k.m.state == V32S_FAILED || k.c.state == VCS_FAILED)) break;
    }
    CHECK(t_aa > 0); CHECK(t_conn > 0);
    if (!t_conn) { printf("  FAILED: AA seen at %ld, answerer state %d error %d, caller state %d\n", t_aa, k.m.state, k.m.error, k.c.state); return; }
    /* AA began at 1.4 s (caller's clock) and takes the line delay to arrive */
    CHECK(t_aa > 11200 + 800 && t_aa < 11200 + 2400);
    for (n = 0; n < 8000L * SECS; n++) {
        int16_t tx = v32_tick(&k.m, k.a_rx);
        k.a_rx = v32line_tick(&k.line, tx, legacy_peer, &k);
    }
    printf("  AA sent from 1.40 s, detected at %.2f s; V.32bis data at %.2f s, %d bit/s; c->a %ld bits %ld errors, a->c %ld bits %ld errors\n",
           t_aa / 8000.0, t_conn / 8000.0, k.m.bitrate, k.a_chk.bits, k.a_chk.errs, k.c_chk.bits, k.c_chk.errs);
    CHECK_EQ_I(k.m.bitrate, 14400);
    CHECK(k.a_chk.bits > 14400L * (SECS - 1)); CHECK(k.c_chk.bits > 14400L * (SECS - 1));
    CHECK_EQ_I(k.a_chk.errs, 0); CHECK_EQ_I(k.c_chk.errs, 0);
}

int main(void)
{
    test_v8_call();
    test_aadet();
    test_legacy();
    T_REPORT("test_v8");
}
