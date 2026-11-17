/* The rig's PPP call (rig/pppcall) as the SIP rig drives it: 20 ms frames,
 * the answer to a frame reaching the line one frame later. */
#include <string.h>
#include "test.h"
#include "pppcall.h"
#include "caller.h"
#include "channel.h"

#define FR 160
static pppcall_t P;
static caller_t C;
static channel_t CH;
static uint8_t prev_tx[FR], rxf[FR]; static int fn;
static double c_tx;
static uint8_t got[1600]; static size_t got_len; static int got_n;
static char logbuf[4000];

static void on_log(const char *l) { if (strlen(logbuf) + strlen(l) + 2 < sizeof logbuf) { strcat(logbuf, l); strcat(logbuf, "\n"); } printf("    rig: %s\n", l); }
static void cli_rx(void *ctx, const uint8_t *p, size_t n) { (void)ctx; got_n++; got_len = n < sizeof got ? n : sizeof got; memcpy(got, p, got_len); }

static void step(void)
{
    double crx; uint8_t srx;
    channel_tick(&CH, prev_tx[fn], c_tx, &crx, &srx);
    rxf[fn] = srx;
    c_tx = caller_tick_pcm(&C, (int16_t)lround(crx), prev_tx[fn]);
    if (++fn == FR) { fn = 0; pppcall_frame(&P, rxf, prev_tx, FR); }
}

static uint16_t csum(const uint8_t *p, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static void ping(uint8_t *p, uint32_t dst, int seq)
{
    uint16_t c; int i;
    memset(p, 0, 60);
    p[0] = 0x45; p[3] = 60; p[8] = 64; p[9] = 1;
    p[12] = 10; p[13] = 64; p[14] = 64; p[15] = 2;
    p[16] = (uint8_t)(dst >> 24); p[17] = (uint8_t)(dst >> 16); p[18] = (uint8_t)(dst >> 8); p[19] = (uint8_t)dst;
    c = csum(p, 20); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
    p[20] = 8; p[27] = (uint8_t)seq;
    for (i = 28; i < 60; i++) p[i] = (uint8_t)(i + seq);
    c = csum(p + 20, 40); p[22] = (uint8_t)(c >> 8); p[23] = (uint8_t)c;
}

static void one_call(int v90, const char *user, const char *pass)
{
    static const double one[1] = { 1.0 };
    channel_cfg cc; caller_cfg_t kc; pppcall_cfg pc; uint8_t p[60]; long i; int sent;
    memset(&cc, 0, sizeof cc);
    cc.ds_noise_dbfs = cc.us_noise_dbfs = -60; cc.ds_taps = one; cc.ds_ntaps = 1; cc.us_taps = one; cc.us_ntaps = 1;
    cc.echo_gain = 0.15; cc.echo_delay = 300; cc.full_scale = 32124.0; cc.seed = 4;     /* a long RTP round trip */
    channel_init(&CH, &cc);
    memset(prev_tx, 0xFF, sizeof prev_tx); fn = 0; c_tx = 0; got_n = 0; logbuf[0] = 0;
    memset(&pc, 0, sizeof pc);
    pc.level = 3000; pc.modulations = V90M_MOD_V22 | V90M_MOD_V21 | (v90 ? V90M_MOD_V90 : 0); pc.user = user; pc.pass = pass; pc.log = on_log;
    pppcall_init(&P, &pc);
    caller_cfg_default(&kc);
    kc.v90 = v90; kc.ip_rx = cli_rx;
    caller_init(&C, &kc);
    for (i = 0; i < 30L * 8000 && !(C.ppp_on && C.ppp.ip_up); i++) step();
    CHECK(C.ppp_on && C.ppp.ip_up);
    CHECK(strstr(logbuf, v90 ? "CONNECT V.90" : "CONNECT V.22bis") != NULL);
    /* a ping to the rig's address is answered */
    ping(p, PPPCALL_LOCAL, 7);
    for (i = 0, sent = 0; i < 5L * 8000 && got_n == 0; i++) { if (!sent && i % 800 == 0 && caller_send_ip(&C, p, 60) == 0) sent = 1; step(); }
    CHECK_EQ_I(got_n, 1);
    CHECK(got_len == 60 && got[20] == 0 && got[27] == 7 && got[12] == 10 && got[15] == 1 && got[19] == 2);
    CHECK_EQ_I(csum(got, 20), 0); CHECK_EQ_I(csum(got + 20, 40), 0);
    CHECK(memcmp(got + 28, p + 28, 32) == 0);
    /* one to anywhere else is counted, not answered */
    ping(p, PPP_IP4(192, 0, 2, 1), 8);
    for (i = 0, sent = 0; i < 3L * 8000; i++) { if (!sent && i % 800 == 0 && caller_send_ip(&C, p, 60) == 0) sent = 1; step(); }
    CHECK_EQ_I(got_n, 1);
    CHECK_EQ_I(P.other_ip, 1); CHECK_EQ_I(P.pings, 1);
    /* the caller hangs up: the rig says why the call ended, and stays silent */
    caller_hangup(&C);
    for (i = 0; i < 4L * 8000; i++) step();
    CHECK(strstr(logbuf, "call over") != NULL);
    CHECK(strstr(logbuf, "carrier lost") != NULL);
    CHECK_EQ_I(prev_tx[0], 0xFF);
}

int main(void)
{
    printf("  V.22bis, no authentication\n");
    one_call(0, NULL, NULL);
    printf("  V.90, CHAP\n");
    one_call(1, "alice", "wonderland");
    T_REPORT("rig");
}
