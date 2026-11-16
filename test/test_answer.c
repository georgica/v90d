/* End-to-end: the answering modem against a calling-modem peer through the
 * channel simulator; characters both ways once connected. */
#include <string.h>
#include "test.h"
#include "../modem/answer.h"
#include "../modem/v14.h"
#include "../sim/callpeer.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

#define FS 8000

typedef struct { int connected_at, modulation, bitrate, peer_rate, phase; char got_a[64], got_c[64]; int na, nc; } outcome;

static void run(int use_v8, unsigned caller_modes, int caller2400, unsigned our_modes, int seconds, outcome *o)
{
    static const double one[1] = {1.0};
    static const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    channel_cfg cfg; channel_t ch;
    static answer_t a; static callpeer_t c; static v14_t dte;
    answer_cfg ac;
    int16_t atx = 0, ctx = 0;
    int n, sent = 0;

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&ch, &cfg);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = our_modes; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&a, &ac);
    callpeer_init(&c, use_v8, caller_modes, caller2400, 3000);
    memset(o, 0, sizeof *o); o->connected_at = -1;

    for (n = 0; n < seconds * FS; n++) {
        double client_rx; uint8_t srv; int16_t t;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
        ctx = callpeer_tick(&c, (int16_t)lround(client_rx));
        atx = t;
        if (o->connected_at < 0 && a.connected && c.connected) o->connected_at = n;
        if (o->connected_at >= 0 && !sent && n > o->connected_at + FS / 4) {
            v14_write(&c.dte, (const uint8_t *)msg_c, (int)strlen(msg_c));
            v14_write(&dte, (const uint8_t *)msg_a, (int)strlen(msg_a));
            sent = 1;
        }
        if (sent) {
            o->na += v14_read(&dte, (uint8_t *)o->got_a + o->na, 63 - o->na);
            o->nc += v14_read(&c.dte, (uint8_t *)o->got_c + o->nc, 63 - o->nc);
        }
    }
    o->modulation = a.modulation; o->bitrate = a.bitrate; o->peer_rate = c.bitrate; o->phase = a.phase;
    if (o->connected_at < 0)
        printf("  not connected: answer phase %d (v8 state %d result %d, v22 state %d) peer phase %d\n",
               a.phase, a.v8.state, a.v8.result, a.v22.state, c.phase);
    /* strings for comparison */
    (void)msg_a; (void)msg_c;
}

int main(void)
{
    outcome o;
    const char *msg_c = "caller says hello, 0123456789", *msg_a = "answerer replies: ~{}[] OK";
    unsigned ours = V8_MOD_V22 | V8_MOD_V21;

    /* 1. V.8 caller offering V.22bis: 2400 bit/s */
    run(1, V8_MOD_V22 | V8_MOD_V21, 1, ours, 14, &o);
    printf("  answer: V.8 + V.22bis connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22BIS); CHECK_EQ_I(o.bitrate, 2400); CHECK_EQ_I(o.peer_rate, 2400);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 2. V.8 caller offering only V.21: 300 bit/s FSK */
    run(1, V8_MOD_V21, 0, ours, 14, &o);
    printf("  answer: V.8 + V.21 connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 0);
    CHECK_EQ_I(o.modulation, ANS_MOD_V21); CHECK_EQ_I(o.bitrate, 300);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 3. pre-V.8 V.22bis caller: ANSam runs out, then the legacy answer sequence */
    run(0, 0, 1, ours, 16, &o);
    printf("  answer: legacy V.22bis connected at %.2f s\n", o.connected_at / 8000.0);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22BIS); CHECK_EQ_I(o.bitrate, 2400);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 4. pre-V.8 V.22 (1200) caller */
    run(0, 0, 0, ours, 16, &o);
    CHECK(o.connected_at > 5 * FS);
    CHECK_EQ_I(o.modulation, ANS_MOD_V22); CHECK_EQ_I(o.bitrate, 1200);
    CHECK(!strcmp(o.got_a, msg_c)); CHECK(!strcmp(o.got_c, msg_a));

    /* 5. V.8 caller with nothing in common (V.32 only): clean failure */
    run(1, V8_MOD_V32, 0, ours, 14, &o);
    CHECK(o.connected_at < 0);
    CHECK_EQ_I(o.phase, ANS_FAILED);

    /* nobody there, but a hybrid that returns a fifth of our own signal: the
     * V.21 fall-back must not take the echo of its 1650 Hz channel for a
     * caller. No carrier, ever. */
    {
        static const double one[1] = {1.0};
        channel_cfg cfg; channel_t ch; static answer_t a; static v14_t dte; answer_cfg ac;
        int16_t atx = 0; int n, ever = 0;
        memset(&cfg, 0, sizeof cfg);
        cfg.ds_noise_dbfs = -60.0; cfg.us_noise_dbfs = -60.0;
        cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
        cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 3;
        channel_init(&ch, &cfg);
        v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ours; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        for (n = 0; n < 45 * FS && a.phase != ANS_FAILED; n++) {
            double crx; uint8_t srv;
            channel_tick(&ch, linear_to_ulaw(atx), 0.0, &crx, &srv);
            atx = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            if (a.connected) ever = 1;
        }
        printf("  answer: silent line with echo: gave up after %.1f s\n", n / 8000.0);
        CHECK(!ever);
        CHECK_EQ_I(a.phase, ANS_FAILED);
    }

    /* V.22bis retrain (6.4) under the answering modem: started by the caller in
     * mid-call, the call stays up (circuits 107 / 109 ON) and characters pass
     * again afterwards; then one of ours that the caller never answers (it has
     * gone deaf but still sends carrier): the call is given up, not left
     * "connected" with a receiver that is clamped for good. */
    {
        static const double one[1] = {1.0};
        static const char *m1 = "before the retrain", *m2 = "and after it";
        channel_cfg cfg; channel_t ch; static answer_t a; static callpeer_t c; static v14_t dte; answer_cfg ac;
        int16_t atx = 0, ctx = 0; int n, up = -1, dropped = 0, t_rt = -1, t_dead = -1, t_fail = -1, na = 0, rt_seen = 0;
        char got[64];
        memset(&cfg, 0, sizeof cfg); memset(got, 0, sizeof got);
        cfg.ds_noise_dbfs = -55.0; cfg.us_noise_dbfs = -55.0;
        cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
        cfg.echo_gain = 0.2; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
        channel_init(&ch, &cfg);
        v14_init(&dte);
        memset(&ac, 0, sizeof ac);
        ac.level = 3000; ac.modes = ours; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
        answer_init(&a, &ac);
        callpeer_init(&c, 1, V8_MOD_V22 | V8_MOD_V21, 1, 3000);
        for (n = 0; n < 40 * FS; n++) {
            double client_rx; uint8_t srv; int16_t t;
            channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
            if (t_dead >= 0 && n >= t_dead) client_rx = 0.0;
            t = answer_tick(&a, (int16_t)ulaw_to_linear(srv));
            ctx = callpeer_tick(&c, (int16_t)lround(client_rx));
            atx = t;
            if (up < 0 && a.connected && c.connected) up = n;
            if (up < 0) continue;
            if (n == up + FS / 4) v14_write(&c.dte, (const uint8_t *)m1, (int)strlen(m1));
            if (n == up + 2 * FS) { v22m_retrain(&c.v22); t_rt = n; }
            if (t_rt >= 0 && n < t_rt + 2 * FS && a.v22.retraining) rt_seen = 1;
            if (n == up + 4 * FS) v14_write(&c.dte, (const uint8_t *)m2, (int)strlen(m2));
            if (n < up + 6 * FS && !a.connected) dropped = 1;
            na += v14_read(&dte, (uint8_t *)got + na, 63 - na);
            if (n == up + 6 * FS) { t_dead = n; }
            if (n == up + 6 * FS + 400) v22m_retrain(&a.v22);
            if (t_fail < 0 && t_dead >= 0 && a.phase == ANS_FAILED) t_fail = n;
        }
        printf("  answer: V.22bis retrain by the caller: seen %d, completed %d, call dropped %d, text \"%s\"\n", rt_seen, a.v22.retrains, dropped, got);
        CHECK(up > 0); CHECK(rt_seen); CHECK_EQ_I(a.v22.retrains, 1); CHECK_EQ_I(dropped, 0);
        CHECK(!strcmp(got, "before the retrainand after it"));
        printf("  answer: our retrain never answered: call given up %.1f s after it was asked for\n", t_fail < 0 ? -1.0 : (t_fail - t_dead - 400) / 8000.0);
        CHECK(t_fail > 0); CHECK_EQ_I(a.phase, ANS_FAILED); CHECK(!a.connected);
    }

    T_REPORT("answer");
}
