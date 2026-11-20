/* dce in the calling role (dce_cfg_t.originate, the box's bench mode)
 * against dce answering, both carrying octets with V.42 error control
 * (raw_rx with raw_ec), over a 2-wire line (libv90modem's sim/v32line). */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "test.h"
#include "dce.h"
#include "v32line.h"

#define FS 8000
#define MS(ms) ((long)(ms) * FS / 1000)

typedef struct { uint8_t got[512]; size_t n; } sink_t;
static sink_t s_orig, s_ans;
static void rx_orig(void *ctx, const uint8_t *o, size_t n) { (void)ctx; if (s_orig.n + n <= sizeof s_orig.got) { memcpy(s_orig.got + s_orig.n, o, n); s_orig.n += n; } }
static void rx_ans(void *ctx, const uint8_t *o, size_t n) { (void)ctx; if (s_ans.n + n <= sizeof s_ans.got) { memcpy(s_ans.got + s_ans.n, o, n); s_ans.n += n; } }

static dce_t O, A;
static int16_t clip(double x) { long q = lrint(x); return (int16_t)(q > 32767 ? 32767 : q < -32768 ? -32768 : q); }
static int a_alive = 1;
static double peer(void *u, double rx)
{
    int16_t tx = 0;
    (void)u;
    if (a_alive) a_alive = dce_tick(&A, clip(rx), &tx);
    return tx;
}

static void call(unsigned modulations, int realistic, const char *what)
{
    static const char up[] = "octets from the box, the calling side", down[] = "octets from the answering modem";
    dce_cfg_t oc, ac; v32line_cfg lc; v32line_t *L; static v32line_t line;
    dce_status_t so, sa;
    int16_t rx = 0, tx; long n; int sent = 0;
    L = &line;
    memset(&s_orig, 0, sizeof s_orig); memset(&s_ans, 0, sizeof s_ans);
    memset(&lc, 0, sizeof lc);
    lc.mulaw = 1; lc.seed = 5; lc.delay = 40;
    lc.noise_a_dbm0 = lc.noise_c_dbm0 = -300; lc.near_db = lc.far_db = lc.c_near_db = lc.c_far_db = 300;
    if (realistic) {
        lc.loss_db = 10; lc.delay = 160; lc.near_db = 12; lc.far_db = 15; lc.c_near_db = 12; lc.c_far_db = 18;
        lc.noise_a_dbm0 = lc.noise_c_dbm0 = -45; lc.freq_offset = 1; lc.ppm = 50;
    }
    v32line_init(L, &lc);
    dce_cfg_default(&oc); dce_cfg_default(&ac);
    oc.originate = 1; oc.modulations = modulations; oc.raw_rx = rx_orig; oc.raw_ec = 1;
    ac.modulations = V90M_MOD_V22 | V90M_MOD_V21; ac.raw_rx = rx_ans; ac.raw_ec = 1;
    memset(&O, 0, sizeof O); memset(&A, 0, sizeof A);
    dce_init(&O, &oc); dce_init(&A, &ac); a_alive = 1;
    for (n = 0; n < MS(30000); n++) {
        if (!dce_tick(&O, rx, &tx)) break;
        rx = v32line_tick(L, tx, peer, NULL);
        if (!sent && O.state == DCE_ONLINE && A.state == DCE_ONLINE) {
            dce_raw_write(&O, (const uint8_t *)up, sizeof up - 1);
            dce_raw_write(&A, (const uint8_t *)down, sizeof down - 1);
            sent = 1;
        }
        if (sent && s_orig.n == sizeof down - 1 && s_ans.n == sizeof up - 1) break;
    }
    dce_get_status(&O, &so); dce_get_status(&A, &sa);
    printf("  %-40s calling side: state %d, modulation %d, %d bit/s, error control %d (V.42 state %d); answering: state %d, error control %d; "
           "got %zu and %zu octets\n", what, so.state, so.modulation, so.bitrate, so.error_control, so.v42_state, sa.state, sa.error_control,
           s_orig.n, s_ans.n);
    CHECK_EQ_I(so.state, DCE_ONLINE);
    CHECK_EQ_I(sa.state, DCE_ONLINE);
    CHECK_EQ_I(so.error_control, 1);
    CHECK_EQ_I(sa.error_control, 1);
    CHECK(s_orig.n == sizeof down - 1 && !memcmp(s_orig.got, down, sizeof down - 1));
    CHECK(s_ans.n == sizeof up - 1 && !memcmp(s_ans.got, up, sizeof up - 1));
}

int main(void)
{
    call(V90M_MOD_V22 | V90M_MOD_V21, 0, "V.22bis, clean line");
    call(V90M_MOD_V22 | V90M_MOD_V21, 1, "V.22bis, loss, echoes, noise");
    T_REPORT("dce_orig");
}
