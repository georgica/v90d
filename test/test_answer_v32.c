/* The answering modem (modem/answer) with a V.32bis caller (sim/v32call on
 * sim/v32line, mu-law at our side): through V.8, and a caller without V.8
 * that answers the answer tone with AA; data both ways, the rate ceiling,
 * our retrain, loss of the caller; and V.32bis not offered. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "../modem/answer.h"
#include "../sim/v8call.h"
#include "../sim/v32call.h"
#include "../sim/v32line.h"

/* 64 zeros, a one, then a pseudo-random stream; the checker finds the one */
typedef struct { uint32_t l; int n; } src_t;
typedef struct { uint32_t l; int zeros, synced; long bits, errs; } chk_t;
static int src_bit(void *u)
{
    src_t *s = u;
    if (s->n < 65) return s->n++ == 64;
    s->l = s->l * 1103515245u + 12345u;
    return (s->l >> 16) & 1;
}
static void chk_bit(void *u, int b)
{
    chk_t *c = u;
    if (!c->synced) {
        if (!b) c->zeros++;
        else { if (c->zeros >= 48) c->synced = 1; c->zeros = 0; }
        return;
    }
    c->l = c->l * 1103515245u + 12345u;
    c->bits++;
    c->errs += b != (int)((c->l >> 16) & 1);
}

static answer_t A; static v32call_t C; static v8call_t V8; static v32line_t L;
static src_t a_src, c_src; static chk_t a_chk, c_chk;
static int use_v8, mute; static long c_wait; static int16_t a_rx; static long now;

static void data_reset(void)
{
    memset(&a_src, 0, sizeof a_src); memset(&c_src, 0, sizeof c_src);
    memset(&a_chk, 0, sizeof a_chk); memset(&c_chk, 0, sizeof c_chk);
    a_src.l = c_chk.l = 1; c_src.l = a_chk.l = 7;
}

static int  a_get(void *u) { (void)u; return src_bit(&a_src); }
static void a_put(void *u, int b) { (void)u; chk_bit(&a_chk, b); }
static int  c_get(void *u) { (void)u; return src_bit(&c_src); }
static void c_put(void *u, int b) { (void)u; chk_bit(&c_chk, b); }

static double peer(void *u, double rx)
{
    (void)u;
    if (mute) return 0;
    if (use_v8 && V8.state != V8C_DONE) {
        int16_t tx = 0; long q = lrint(rx);
        v8call_tick(&V8, (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q), &tx);
        return tx;
    }
    if (c_wait > 0) { c_wait--; return 0; }
    return v32call_tick(&C, rx);
}

static void start(unsigned modes, int with_v8, int max_down, int max_up)
{
    answer_cfg ac; vc_cfg cc; v32line_cfg lc; v8_menu_t cm;
    data_reset();
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 1; lc.loss_db = 6; lc.delay = 200; lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.noise_a_dbm0 = -300; lc.noise_c_dbm0 = -300; lc.seed = 1;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = modes; ac.getbit = a_get; ac.putbit = a_put;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, max_down, max_up);
    memset(&cc, 0, sizeof cc);
    cc.level = 3000; cc.rates = V32_RALL; cc.trn_len = 1280; cc.getbit = c_get; cc.putbit = c_put; cc.user = NULL;
    v32call_init(&C, &cc);
    memset(&cm, 0, sizeof cm);
    cm.call_function = V8_CALLF_DATA; cm.modes = V8_MOD_V32 | V8_MOD_V22; cm.n_mod_octets = 3;
    v8call_init(&V8, 8000, 4243, &cm);
    v32line_init(&L, &lc);
    use_v8 = with_v8; mute = 0; c_wait = with_v8 ? 0 : 8000; a_rx = 0; now = 0;
}

static void run(long samples)
{
    while (samples-- > 0) { a_rx = v32line_tick(&L, answer_tick(&A, a_rx), peer, NULL); now++; }
}

static int to_data(int max_ms)
{
    long end = now + 8L * max_ms;
    while (now < end) {
        run(8);
        if (A.connected && !A.retraining && C.connected && C.state == VCS_DATA) return 1;
        if (A.phase == ANS_FAILED || C.state == VCS_FAILED) return 0;
    }
    return 0;
}

int main(void)
{
    long t;
    const unsigned ALL = ANS_MODE_V90 | V8_MOD_V34 | V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21;

    /* V.8 caller whose best mode is V.32bis */
    start(ALL, 1, 0, 0);
    CHECK_EQ_I(to_data(30000), 1);
    CHECK_EQ_I(A.phase, ANS_V32);
    CHECK_EQ_I(A.used_v8, 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V32BIS);
    CHECK_EQ_I(A.bitrate, 14400); CHECK_EQ_I(A.rx_bitrate, 14400);
    run(8000L * 3);
    CHECK(a_chk.bits > 14400L * 2); CHECK(c_chk.bits > 14400L * 2);
    CHECK_EQ_I(a_chk.errs, 0); CHECK_EQ_I(c_chk.errs, 0);
    printf("  V.8, then V.32bis: %d bit/s; %ld / %ld bits, %ld / %ld bad\n", A.bitrate, c_chk.bits, a_chk.bits, c_chk.errs, a_chk.errs);
    /* our retrain */
    CHECK_EQ_I(answer_retrain(&A), 0);
    run(2000);
    CHECK_EQ_I(A.connected, 1); CHECK_EQ_I(A.retraining, 1); CHECK_EQ_I(A.retrains, 1);
    for (t = 0; t < 10000 && A.retraining; t += 10) run(80);
    CHECK_EQ_I(A.retraining, 0); CHECK_EQ_I(A.phase, ANS_V32);
    /* nothing to say good-bye with: the owner just stops */
    CHECK_EQ_I(answer_cleardown(&A), -1);
    /* the caller goes away */
    mute = 1;
    for (t = 0; t < 3000 && A.phase != ANS_FAILED; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_FAILED); CHECK_EQ_I(A.connected, 0);

    /* ceiling: one rate both ways, the lower of the two limits */
    start(ALL, 1, 12000, 9600);
    CHECK_EQ_I(to_data(30000), 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V32BIS); CHECK_EQ_I(A.bitrate, 9600); CHECK_EQ_I(A.rx_bitrate, 9600);

    /* a caller without V.8: AA during our answer tone */
    start(ALL, 0, 0, 0);
    CHECK_EQ_I(to_data(30000), 1);
    CHECK_EQ_I(A.phase, ANS_V32); CHECK_EQ_I(A.used_v8, 0);
    CHECK_EQ_I(A.modulation, ANS_MOD_V32BIS); CHECK_EQ_I(A.bitrate, 14400);
    run(8000L * 2);
    CHECK(a_chk.bits > 14400L); CHECK_EQ_I(a_chk.errs, 0); CHECK_EQ_I(c_chk.errs, 0);
    printf("  no V.8, AA: V.32bis data at %.2f s\n", (now - 16000) / 8000.0);

    /* V.32bis not offered: V.8 picks V.22bis for this caller, AA is not listened for */
    start(ALL & ~V8_MOD_V32, 1, 0, 0);
    for (t = 0; t < 8000 && A.phase == ANS_V8; t += 10) run(80);
    CHECK_EQ_I(A.phase, ANS_V22);
    start(ALL & ~V8_MOD_V32, 0, 0, 0);
    run(8000L * 4);
    CHECK(A.phase != ANS_V32); CHECK_EQ_I(A.connected, 0);
    T_REPORT("answer_v32");
}
