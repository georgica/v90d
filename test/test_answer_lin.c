/* The answering modem behind a codec that receives in 16-bit linear and
 * still sends G.711 levels (the box's SLIC in linear mode), through
 * answer_tick_lin, against the same modem behind a G.711 codec on the same
 * line.
 *
 * The line is the one measured on the unit (HARDWARE.md): our transmitter at
 * -12 dBm0, its echo 9 dB down at the codec (about -21 dBm0), the caller
 * arriving no louder than that. A G.711 encoder quantises the sum, so the
 * steps the caller's signal is measured with are those of the echo's peaks;
 * a linear codec has a step of 1.
 *
 *   (a) V.34 from the caller: a V.34 call (sim/v34peer over test/line34.h)
 *       and the V.34 upstream of a V.90 call (sim/v92call: p34a, channel);
 *   (b) V.92 PCM upstream (sim/v92call: v92a, v92chan);
 *   (c) V.32bis, V.22bis and V.21 through the linear entry;
 *   and the two bench settings of answer_cfg: rx_linear reaches the V.92
 *   design, max_tx_code reaches INFO0d. */
#include <stdio.h>
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/pcmenc.h"
#include "../modem/v14.h"
#include "../sim/v92call.h"
#include "../sim/v34peer.h"
#include "../sim/callpeer.h"
#include "../sim/channel.h"
#include "../sim/g711.h"
#include "../sim/v32call.h"
#include "../sim/v32line.h"
#include "line34.h"

/* x^7 + x^6 + 1 sent; the receiver predicts each bit from the seven before */
typedef struct { unsigned tx, rx; long n, bad; } prbs_t;
static int p_get(void *u)
{
    prbs_t *p = (prbs_t *)u;
    int b = ((p->tx >> 6) ^ (p->tx >> 5)) & 1;
    p->tx = ((p->tx << 1) | (unsigned)b) & 127;
    return b;
}
static void p_put(void *u, int bit)
{
    prbs_t *p = (prbs_t *)u;
    int want = ((p->rx >> 6) ^ (p->rx >> 5)) & 1;
    if (p->n >= 7 && want != (bit & 1)) p->bad++;
    p->rx = ((p->rx << 1) | (unsigned)(bit & 1)) & 127;
    p->n++;
}

#define TX_RMS   3961           /* -12 dBm0 */
#define ECHO_RMS 1405           /* 9 dB below it: -21 dBm0 */

static answer_t A; static v92call_t C; static v34peer_t P; static line34_t L; static prbs_t UA, UC;
static int LIN;
typedef struct { int ok, down, up, snr; long bad; } res_t;

/* one sample of the modem at whichever codec this run has; the codeword it
 * answers with goes to the line as that codeword's G.711 level */
static uint8_t a_octet(answer_t *a, int16_t rx)
{
    return LIN ? answer_tick_lin(a, rx) : answer_tick_pcm(a, pcm_encode(a->p2cfg.alaw, rx));
}
static int16_t a_tick(void *u, int16_t rx)
{
    answer_t *a = (answer_t *)u;
    return (int16_t)v90_octet_linear(a->p2cfg.alaw, a_octet(a, rx));
}
static int16_t c_tick(void *u, int16_t rx) { return v34peer_tick((v34peer_t *)u, rx); }

static void users(answer_cfg *ac, unsigned modes, int lin)
{
    memset(&UA, 0, sizeof UA); memset(&UC, 0, sizeof UC); UA.tx = 1; UC.tx = 77;
    memset(ac, 0, sizeof *ac);
    ac->level = TX_RMS; ac->modes = modes; ac->getbit = p_get; ac->putbit = p_put; ac->user = &UA;
    ac->rx_linear = lin;
    LIN = lin;
}

/* (a) a V.34 call. The hybrid of line34 has taps 0.6, 0.5, -0.3 (RMS gain
 * 0.837): 0.424 of it is 9 dB of loss; the caller (3000 RMS) arrives through
 * 0.468 at 1405 RMS. Line noise 8 RMS (-66 dBm0). */
static res_t v34_call(int lin)
{
    answer_cfg ac; v34peer_cfg pc; line34_cfg lc; long t; res_t r;
    memset(&lc, 0, sizeof lc);
    lc.law = 1; lc.rx_lin = lin; lc.d_ca = 24; lc.d_ac = 20; lc.gain = 0.468; lc.echo_a = 0.424; lc.echo_c = 0.3; lc.noise = 8;
    users(&ac, V8_MOD_V34 | V8_MOD_V22 | V8_MOD_V21, lin);
    v34peer_default(&pc, 3000);
    pc.dx.getbit = p_get; pc.dx.putbit = p_put; pc.dx.user = &UC;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    v34peer_init(&P, &pc); line34_init(&L, &lc);
    for (t = 0; t < 8000L * 30 && !(A.connected && P.state == VP_DATA) && A.phase != ANS_FAILED; t++) (void)line34_step(&L, a_tick, &A, c_tick, &P);
    UA.n = UA.bad = UC.n = UC.bad = 0;
    for (t = 0; t < 8000L * 3; t++) (void)line34_step(&L, a_tick, &A, c_tick, &P);
    r.ok = A.connected && A.modulation == ANS_MOD_V34 && !A.retraining && UA.n > 2L * A.rx_bitrate && UC.n > 2L * A.bitrate;
    r.down = A.bitrate; r.up = A.rx_bitrate; r.bad = UA.bad + UC.bad; r.snr = answer_snr_db10(&A);
    return r;
}

/* a V.90 or V.92 caller (sim/v92call). Its two line models are set for the
 * same echo: sim/channel has a one-tap hybrid, and the simulated analogue
 * modem asks for a downstream of about +0.6 dBm0 whatever INFO0d says, so
 * 0.08 gives the 1405 RMS; sim/v92chan's hybrid at 0.158 gives 1409 RMS
 * (the modem's own figure, echo_rms). The caller's signal: V.34 upstream at
 * 1405 RMS; PCM upstream with unit symbols at 1400 RMS. Line noise: -70 dBFS
 * (10 RMS) on sim/channel, 3 RMS on sim/v92chan. */
static res_t pcm_call(int lin, int pcm_up, int max_tx_code)
{
    answer_cfg ac; v92call_cfg cc; long t; uint8_t atx = 0xFF; res_t r;
    users(&ac, ANS_MODE_V90 | ANS_MODE_V92 | V8_MOD_V22 | V8_MOD_V21, lin);
    ac.max_tx_code = max_tx_code;
    answer_init(&A, &ac);
    answer_v90_line(&A, -1, 0, 0);
    memset(&cc, 0, sizeof cc);
    cc.v92 = 1; cc.pcm_up = pcm_up; cc.lapm = 1; cc.rx_linear = lin;
    cc.echo_gain = 0.08; cc.echo_gain_pcm = 0.158; cc.echo_delay = 21; cc.level = ECHO_RMS; cc.up_level = 1400; cc.us_noise_dbfs = -70;
    cc.getbit = p_get; cc.putbit = p_put; cc.user = &UC;
    v92call_init(&C, &cc);
    for (t = 0; t < 8000L * 60; t++) {
        atx = a_octet(&A, lin ? v92call_tick_lin(&C, atx) : (int16_t)v90_octet_linear(0, v92call_tick(&C, atx)));
        if ((A.connected && !A.retraining && v92call_data(&C) && answer_moving(&A)) || A.phase == ANS_FAILED) break;
    }
    for (t = 0; t < 8400L * 3; t++) {
        if (t == 400) UA.n = UA.bad = UC.n = UC.bad = 0;    /* what was in the pipes when the pumps started */
        atx = a_octet(&A, lin ? v92call_tick_lin(&C, atx) : (int16_t)v90_octet_linear(0, v92call_tick(&C, atx)));
    }
    r.ok = A.connected && A.modulation == (pcm_up ? ANS_MOD_V92 : ANS_MOD_V90) && A.retrains == 0 && UA.n > 2L * A.rx_bitrate && UC.n > 2L * A.bitrate;
    r.down = A.bitrate; r.up = A.rx_bitrate; r.bad = UA.bad + UC.bad; r.snr = answer_snr_db10(&A);
    if (r.bad <= 14) r.bad = 0;                             /* the two predictors take 7 bits each to lock */
    return r;
}

/* (c) V.22bis and V.21 callers (sim/callpeer over sim/channel): the
 * channel's sample before its mu-law encoder is what a linear codec hands us */
static void slow_call(int use_v8, unsigned caller_modes, int want_mod, int want_rate)
{
    static const double one[1] = { 1.0 };
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch; static callpeer_t c; static v14_t dte; answer_cfg ac;
    char got_a[64], got_c[64]; int na = 0, nc = 0, n, up = -1, sent = 0; int16_t ctx = 0; uint8_t atx = 0xFF;
    memset(&cfg, 0, sizeof cfg); memset(got_a, 0, sizeof got_a); memset(got_c, 0, sizeof got_c);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    ac.rx_linear = 1;
    answer_init(&A, &ac);
    callpeer_init(&c, use_v8, caller_modes, 1, 3000);
    for (n = 0; n < 26 * 8000; n++) {
        double client_rx;
        channel_tick(&ch, atx, (double)ctx, &client_rx, NULL);
        atx = answer_tick_lin(&A, ch.us_lin);
        ctx = callpeer_tick(&c, (int16_t)lround(client_rx));
        if (up < 0 && A.connected && c.connected) up = n;
        if (up >= 0 && !sent && n > up + 2000) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            na += v14_read(&dte, (uint8_t *)got_a + na, 63 - na);
            nc += v14_read(&c.dte, (uint8_t *)got_c + nc, 63 - nc);
        }
    }
    CHECK(up > 0); CHECK_EQ_I(A.modulation, want_mod); CHECK_EQ_I(A.bitrate, want_rate);
    CHECK(!strcmp(got_a, msg_c)); CHECK(!strcmp(got_c, msg_a));
    printf("  linear entry, %s: connected after %.1f s at %d bit/s, text both ways %s\n", want_mod == ANS_MOD_V21 ? "V.21" : "V.22bis",
           up / 8000.0, A.bitrate, !strcmp(got_a, msg_c) && !strcmp(got_c, msg_a) ? "intact" : "DAMAGED");
}

/* (c) V.32bis (sim/v32call over sim/v32line without a codec of its own: our
 * codewords go to the line as their G.711 levels, what comes back is linear) */
static v32call_t VC; static long vc_wait;
static struct { uint32_t l; int n; } s_a, s_c;
static struct { uint32_t l; int zeros, synced; long bits, errs; } k_a, k_c;
static int v32_src(void *u)
{
    (void)u;
    if (s_a.n < 65) return s_a.n++ == 64;
    s_a.l = s_a.l * 1103515245u + 12345u;
    return (s_a.l >> 16) & 1;
}
static int v32_csrc(void *u)
{
    (void)u;
    if (s_c.n < 65) return s_c.n++ == 64;
    s_c.l = s_c.l * 1103515245u + 12345u;
    return (s_c.l >> 16) & 1;
}
static void v32_chk(void *u, int b)
{
    (void)u;
    if (!k_a.synced) { if (!b) k_a.zeros++; else { if (k_a.zeros >= 48) k_a.synced = 1; k_a.zeros = 0; } return; }
    k_a.l = k_a.l * 1103515245u + 12345u; k_a.bits++; k_a.errs += b != (int)((k_a.l >> 16) & 1);
}
static void v32_cchk(void *u, int b)
{
    (void)u;
    if (!k_c.synced) { if (!b) k_c.zeros++; else { if (k_c.zeros >= 48) k_c.synced = 1; k_c.zeros = 0; } return; }
    k_c.l = k_c.l * 1103515245u + 12345u; k_c.bits++; k_c.errs += b != (int)((k_c.l >> 16) & 1);
}
static double v32_peer(void *u, double rx)
{
    (void)u;
    if (vc_wait > 0) { vc_wait--; return 0; }
    return v32call_tick(&VC, rx);
}
static void v32_call(void)
{
    static v32line_t VL;
    answer_cfg ac; vc_cfg cc; v32line_cfg lc; long t; int16_t rx = 0;
    memset(&s_a, 0, sizeof s_a); memset(&s_c, 0, sizeof s_c); memset(&k_a, 0, sizeof k_a); memset(&k_c, 0, sizeof k_c);
    s_a.l = k_c.l = 1; s_c.l = k_a.l = 7;
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 0; lc.loss_db = 6; lc.delay = 200; lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.noise_a_dbm0 = -300; lc.noise_c_dbm0 = -300; lc.seed = 1;
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V32 | V8_MOD_V22 | V8_MOD_V21; ac.getbit = v32_src; ac.putbit = v32_chk; ac.rx_linear = 1;
    answer_init(&A, &ac);
    answer_v90_line(&A, 0, 0, 0);
    memset(&cc, 0, sizeof cc);
    cc.level = 3000; cc.rates = V32_RALL; cc.trn_len = 1280; cc.getbit = v32_csrc; cc.putbit = v32_cchk; cc.user = NULL;
    v32call_init(&VC, &cc);
    v32line_init(&VL, &lc);
    vc_wait = 8000;                     /* a caller without V.8: AA after a second of answer tone */
    for (t = 0; t < 8000L * 40; t++) {
        rx = v32line_tick(&VL, (int16_t)v90_octet_linear(0, answer_tick_lin(&A, rx)), v32_peer, NULL);
        if (A.connected && !A.retraining && VC.connected && VC.state == VCS_DATA) break;
    }
    for (t = 0; t < 8000L * 3; t++) rx = v32line_tick(&VL, (int16_t)v90_octet_linear(0, answer_tick_lin(&A, rx)), v32_peer, NULL);
    CHECK_EQ_I(A.modulation, ANS_MOD_V32BIS); CHECK_EQ_I(A.bitrate, 14400);
    CHECK(k_a.bits > 14400L * 2); CHECK(k_c.bits > 14400L * 2);
    CHECK_EQ_I(k_a.errs, 0); CHECK_EQ_I(k_c.errs, 0);
    printf("  linear entry, V.32bis: %d bit/s; %ld / %ld bits, %ld / %ld bad\n", A.bitrate, k_c.bits, k_a.bits, k_c.errs, k_a.errs);
}

static void show(const char *what, res_t c, res_t l)
{
    printf("  %-28s companded receive: %d bit/s up (%d down), SNR %d.%d dB; linear receive: %d bit/s up (%d down), SNR %d.%d dB\n",
           what, c.up, c.down, c.snr / 10, c.snr % 10, l.up, l.down, l.snr / 10, l.snr % 10);
}

int main(void)
{
    res_t c, l;
    uint32_t q_comp, q_lin;

    /* (a) V.34 */
    c = v34_call(0); l = v34_call(1);
    show("V.34 call:", c, l);
    CHECK(c.ok); CHECK(l.ok); CHECK_EQ_I(c.bad, 0); CHECK_EQ_I(l.bad, 0);
    CHECK(c.up >= 21600 && c.up <= 26400);          /* what the unit gets today */
    CHECK(l.up >= c.up + 4800);
    CHECK(l.down >= c.down);

    /* (a) the V.34 upstream of a V.90 call */
    c = pcm_call(0, 0, 0); l = pcm_call(1, 0, 0);
    show("V.90, V.34 upstream:", c, l);
    CHECK(c.ok); CHECK(l.ok); CHECK_EQ_I(c.bad, 0); CHECK_EQ_I(l.bad, 0);
    CHECK(l.up >= c.up + 2400);
    CHECK(l.snr >= c.snr + 60);
    CHECK_EQ_I(l.down, c.down);
    CHECK_EQ_I(A.p2cfg.max_tx_code, ANS_MAX_TX_CODE);
    CHECK_EQ_I(C.p2.info0d.max_tx_power, ANS_MAX_TX_CODE);

    /* (b) V.92 PCM upstream. The companded design keeps the points apart by
     * the quantiser's steps at 1.5 times the echo RMS; the linear one does
     * not, and allows 40 dB below the echo for what the canceller leaves
     * (34 dB with a quantiser in the path): v92/v92_design.c */
    c = pcm_call(0, 1, 0);
    q_comp = A.u.v92.q_quiet;
    CHECK_EQ_I(A.u.v92.cfg.rx_linear, 0); CHECK_EQ_I(A.u.v92.dsn.cfg.rx_linear, 0);
    l = pcm_call(1, 1, 0);
    q_lin = A.u.v92.q_quiet;
    show("V.92, PCM upstream:", c, l);
    CHECK(c.ok); CHECK(l.ok); CHECK_EQ_I(c.bad, 0); CHECK_EQ_I(l.bad, 0);
    CHECK(c.up >= 24000 && c.up <= 29333);
    CHECK(l.up >= c.up + 8000);
    CHECK_EQ_I(l.down, c.down);
    CHECK_EQ_I(A.u.v92.cfg.rx_linear, 1); CHECK_EQ_I(A.u.v92.dsn.cfg.rx_linear, 1);
    CHECK(q_comp > 100); CHECK_EQ_I(q_lin, 0); CHECK_EQ_I(A.u.v92.q_trn, 0);
    printf("    echo at the codec %lu RMS; quantiser variance while the caller is silent: companded %lu, linear %lu\n",
           (unsigned long)A.u.v92.echo_rms, (unsigned long)q_comp, (unsigned long)q_lin);

    /* the announced maximum power is a run-time setting: INFO0d carries it,
     * and the V.92 design predicts the data mode echo from it */
    {
        int rms23 = A.u.v92.cfg.down_rms;
        l = pcm_call(1, 1, 11);
        CHECK(l.ok); CHECK_EQ_I(l.bad, 0);
        CHECK_EQ_I(A.p2cfg.max_tx_code, 11);
        CHECK_EQ_I(C.p2.info0d.max_tx_power, 11);
        CHECK(A.u.v92.cfg.down_rms > rms23 * 19 / 10 && A.u.v92.cfg.down_rms < rms23 * 21 / 10);   /* 6 dB */
        printf("    maximum power code 11 (-6 dBm0) announced: V.92 %d bit/s up, %d down\n", l.up, l.down);
    }

    /* (c) the slow modulations through the linear entry */
    v32_call();
    slow_call(1, V8_MOD_V22 | V8_MOD_V21, ANS_MOD_V22BIS, 2400);
    slow_call(1, V8_MOD_V21, ANS_MOD_V21, 300);
    T_REPORT("answer_lin");
}
