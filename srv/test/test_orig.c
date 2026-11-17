/* The server's bench mode where the box calls (srv_set_originate): a modem
 * on the port goes off hook as if told ATA, and the box's calling modem
 * reaches V.22bis with it. The far end is libv90modem's answering modem over
 * its 2-wire line model. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "srv.h"
#include "v90modem_priv.h"
#include "v32line.h"
#include "v14.h"

#define FS 8000
#define MS(ms) ((long)(ms) * FS / 1000)

static srv_t S;
static answer_t A;
static v14_t dte;
static void lan_rx(void *ctx, const uint8_t *p, size_t n) { (void)ctx; (void)p; (void)n; }
static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }
static double peer(void *u, double rx) { (void)u; return answer_tick(&A, clip(rx)); }

int main(void)
{
    static v32line_t L;
    v32line_cfg lc; cfg_t c; answer_cfg ac;
    int16_t rx = 0; long n, connected_at = -1, first_sound = -1;
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 1; lc.seed = 9; lc.delay = 80; lc.loss_db = 6;
    lc.near_db = 15; lc.far_db = 20; lc.c_near_db = 15; lc.c_far_db = 20;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = -50;
    v32line_init(&L, &lc);
    cfg_default(&c);
    srv_init(&S, &c, 3, lan_rx, NULL);
    srv_set_originate(&S, 1);
    v14_init(&dte);
    memset(&ac, 0, sizeof ac);
    ac.level = 3000; ac.modes = V8_MOD_V22 | V8_MOD_V21; ac.getbit = v14_getbit; ac.putbit = v14_putbit; ac.user = &dte;
    answer_init(&A, &ac);

    for (n = 0; n < MS(20000); n++) {
        uint8_t tx = srv_tick_lin(&S, 1, rx);              /* the modem is off hook from the start */
        int16_t lin = v90m_g711_decode(0, tx);
        if (first_sound < 0 && lin != 0) first_sound = n;
        rx = v32line_tick(&L, lin, peer, NULL);
        if (connected_at < 0 && S.status.line == ST_LINE_CONNECTED) connected_at = n;
        if (connected_at >= 0 && n > connected_at + MS(500)) break;
    }
    printf("  the box calls: first sound from the box at %.2f s (after the answerer's ANSam), connected at %.2f s, "
           "modulation %d, %u / %u bit/s; the answerer: %d, modulation %d\n", first_sound / (double)FS, connected_at / (double)FS,
           S.status.modulation, S.status.down_bps, S.status.up_bps, A.connected, A.modulation);
    CHECK(first_sound > MS(300));                           /* no dial tone */
    CHECK(connected_at > 0);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V22BIS);
    CHECK_EQ_I(A.connected, 1);
    CHECK_EQ_I(A.modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(A.used_v8, 1);
    T_REPORT("srv_orig");
}
