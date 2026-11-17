/* Interop: our V.8 answer side against spandsp's independent V.8 CALLER,
 * through the channel simulator. Host-only (spandsp is a test peer, never
 * shipped): `make test-interop`. */
#include <string.h>
#include "test.h"
#include "peer_spandsp.h"
#include "../modem/v8ans.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

#define FS 8000

static int run(unsigned sp_mods, int sp_pcm, int send_ci, double noise, double echo,
               v8ans_t *ans)
{
    static const double one[1] = {1.0};
    channel_cfg cfg; channel_t ch;
    v8_menu_t local;
    int16_t atx = 0, ctx = 0, arx, crx;
    int n;

    memset(&local, 0, sizeof local);
    local.call_function = V8_CALLF_DATA;
    local.modes = (1u << 1) | (1u << 4) | (1u << 12);     /* V.34, V.22, V.21 */
    local.pcm = V8_PCM_DIGITAL; local.prot = V8_PROT_LAPM; local.access = V8_ACC_DIGITAL;

    sp_v8call_start(sp_mods, sp_pcm, send_ci);

    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = noise; cfg.us_noise_dbfs = noise;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = echo; cfg.echo_delay = 37; cfg.full_scale = 32124.0; cfg.seed = 99;
    channel_init(&ch, &cfg);
    v8ans_init(ans, FS, 5600, &local);

    for (n = 0; n < 20 * FS; n++) {
        double client_rx; uint8_t srv;
        channel_tick(&ch, linear_to_ulaw(atx), (double)ctx, &client_rx, &srv);
        arx = (int16_t)ulaw_to_linear(srv);
        crx = (int16_t)lround(client_rx);
        sp_v8call_tick(crx, &ctx);
        if (v8ans_tick(ans, arx, &atx) == V8A_DONE) break;
    }
    /* let spandsp finish its side */
    for (n = 0; n < FS && !sp_v8call_result()->final; n++) sp_v8call_tick(0, &ctx);
    sp_v8call_stop();
    return 0;
}

int main(void)
{
    v8ans_t ans;
    int ci;

    const sp_v8_result *r = sp_v8call_result();
    for (ci = 0; ci <= 1; ci++) {
        /* spandsp as a V.90 analogue caller (with and without CI) */
        run(SP_MOD_V90 | SP_MOD_V34 | SP_MOD_V32 | SP_MOD_V22 | SP_MOD_V21, 1 /* analogue */,
            ci, -50.0, 0.2, &ans);
        CHECK_EQ_I(ans.result, V8A_RES_OK);
        CHECK_EQ_I(ans.selected, V8_SEL_V90);
        CHECK_EQ_I(ans.cm.call_function, V8_CALLF_DATA);
        CHECK_EQ_I(ans.cm.modes, (1u << 1) | (1u << 3) | (1u << 4) | (1u << 12));  /* V.34 V.32 V.22 V.21 */
        CHECK(ans.cm.has_pcm && ans.cm.pcm == V8_PCM_ANALOG);
        CHECK(ans.cm.has_prot && ans.cm.prot == V8_PROT_LAPM);
        /* spandsp's view of our JM */
        CHECK(r->final);
        CHECK(r->ok);
        CHECK_EQ_I(r->call_function, 6);
        CHECK_EQ_I(r->modulations, SP_MOD_V90 | SP_MOD_V34 | SP_MOD_V22 | SP_MOD_V21);
        CHECK_EQ_I(r->pcm, 2);                 /* digital modem */
        CHECK(r->pstn_access & 4);            /* DCE on digital network */
        CHECK_EQ_I(r->protocol, 1);            /* LAPM */
    }

    /* spandsp as a plain V.22bis caller on a noisy, echoey line */
    run(SP_MOD_V22, 0, 0, -38.0, 0.35, &ans);
    CHECK_EQ_I(ans.result, V8A_RES_OK);
    CHECK_EQ_I(ans.selected, 4);
    CHECK(r->ok);
    CHECK_EQ_I(r->modulations, SP_MOD_V22);

    T_REPORT("interop_v8_spandsp");
}
