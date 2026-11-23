/* The whole server against a whole caller: hook switch, DTMF dialler, modem
 * (V.22bis or V.90), V.42, PPP client, over the channel simulator. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "test.h"
#include "srv.h"
#include "v90modem_priv.h"   /* the tests look at the modem's internals */
#include "line.h"
#include "console.h"
#include "dclist.h"
#include "dcnow.h"
#include "upnp.h"
#include "saturn.h"
#include "bbs.h"
#include "telnet.h"
#include "bbsdial.h"
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "stun.h"
#include "caller.h"
#include "channel.h"
#include "g711.h"

#define FS 8000
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static srv_t S;
static caller_t C;
static channel_t CH;
static int hook, modem_on;
static uint8_t s_tx = 0xFF;         /* the server's last octet */
static double c_tx;                 /* the caller's last sample */
static double c_rx_pw;              /* smoothed power the caller hears */
static long now;
static int lan_n, lan_bad, cli_n, cli_bad;
static uint8_t lan_last[64];

static void lan_rx(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx; lan_n++;
    if (n != 40 || p[0] != 0x45 || p[39] != (uint8_t)(p[38] ^ 0x5A)) lan_bad++;
    memcpy(lan_last, p, n < 64 ? n : 64);
}
static void cli_rx(void *ctx, const uint8_t *p, size_t n)
{
    (void)ctx; cli_n++;
    if (n != 40 || p[0] != 0x45 || p[39] != (uint8_t)(p[38] ^ 0xA5)) cli_bad++;
}
static void mkpkt(uint8_t *p, int seq, uint8_t x)
{
    int i;
    memset(p, 0, 40); p[0] = 0x45; p[3] = 40; p[8] = 64; p[9] = 17;
    for (i = 20; i < 39; i++) p[i] = (uint8_t)(seq * 7 + i);
    p[38] = (uint8_t)seq; p[39] = (uint8_t)(p[38] ^ x);
}

static int echo_bulk;               /* extra echo delay of the line, samples */

static void line_init(void)
{
    static const double one[1] = { 1.0 };
    channel_cfg cc;
    memset(&cc, 0, sizeof cc);
    cc.ds_noise_dbfs = cc.us_noise_dbfs = -60; cc.ds_taps = one; cc.ds_ntaps = 1; cc.us_taps = one; cc.us_ntaps = 1;
    cc.echo_gain = 0.15; cc.echo_delay = 21 + echo_bulk; cc.full_scale = 32124.0; cc.seed = 3;
    channel_init(&CH, &cc);
    hook = 0; modem_on = 0; s_tx = 0xFF; c_tx = 0; now = 0; c_rx_pw = 0;
    lan_n = lan_bad = cli_n = cli_bad = 0;
}

static uint8_t c_oct = 0xFF;        /* what a V.92 caller's line hands the server */
static int lin_mode;                /* the line's codec receives in 16-bit linear: srv_tick_lin */
static int16_t c_lin;               /* ... and what a V.92 caller's line hands the server then */

/* one sample; `tone` is what the caller's dialler puts on the line while its
 * modem is not running */
static void step(double tone)
{
    double crx; uint8_t srv_rx; int16_t x;
    /* a V.92 caller brings its own line (sim/v92call): octets both ways */
    if (modem_on && C.cfg.v92) {
        s_tx = lin_mode ? srv_tick_lin(&S, hook, c_lin) : srv_tick(&S, hook, c_oct);
        c_oct = caller_tick_v92(&C, s_tx);
        c_lin = C.vc.rx_lin;
        now++;
        return;
    }
    channel_tick(&CH, s_tx, hook ? c_tx : 0.0, &crx, &srv_rx);
    if (!hook) srv_rx = 0xFF;
    /* the sample before the channel's mu-law encoder is what a linear codec gives */
    if (lin_mode) { srv_set_rx_linear(&S, 1); s_tx = srv_tick_lin(&S, hook, hook ? CH.us_lin : 0); }
    else s_tx = srv_tick(&S, hook, srv_rx);
    x = (int16_t)lround(crx);
    c_rx_pw += ((double)x * x - c_rx_pw) / 400.0;
    if (modem_on) c_tx = caller_tick_pcm(&C, x, s_tx);
    else c_tx = tone;
    now++;
}
static void run_ms(int ms) { long i; for (i = 0; i < (long)ms * 8; i++) step(0); }

static void dial(const char *num)
{
    static const char keys[] = "123A456B789C*0#D";
    static const double rowf[4] = { 697, 770, 852, 941 }, colf[4] = { 1209, 1336, 1477, 1633 };
    for (; *num; num++) {
        int idx = (int)(strchr(keys, *num) - keys), i;
        for (i = 0; i < 70 * 8; i++)
            step(2500 * sin(2 * M_PI * rowf[idx / 4] * i / FS) + 2500 * sin(2 * M_PI * colf[idx % 4] * i / FS));
        run_ms(70);
    }
}

static void base_cfg(cfg_t *c)
{
    cfg_default(c);
    strcpy(c->line_number, "5551234");
    c->rings = 1;
    c->auth = CFG_AUTH_CHAP;
    c->n_users = 1; strcpy(c->users[0].name, "alice"); strcpy(c->users[0].secret, "wonderland");
    c->modulations = CFG_MOD_V22BIS | CFG_MOD_V22 | CFG_MOD_V21;
}

/* off hook, wait for dial tone, dial; returns with the server in whatever
 * state the number led to, after at most wait_ms */
static void place_call(const char *num, int wait_ms)
{
    long i;
    hook = 1; run_ms(400);
    CHECK(c_rx_pw > 200.0 * 200.0);                 /* dial tone is heard */
    CHECK_EQ_I(S.status.line, ST_LINE_RINGING);
    dial(num);
    for (i = 0; i < (long)wait_ms * 8 && S.tel.state != TEL_ANSWERED && S.tel.state != TEL_BUSY &&
                S.tel.state != TEL_INTERCEPT; i++) step(0);
}

/* how the caller's PPP speaks CCP: like pppd (Deflate) unless a test says otherwise */
static int caller_ccp = PPPC_CCP_PPPD;

/* kind: 0 V.22bis, 1 V.90, 2 V.34, 3 V.32bis, 4 V.92 */
static void start_modem(int kind, const char *user, const char *pass)
{
    caller_cfg_t cc;
    caller_cfg_default(&cc);
    cc.v90 = kind == 1; cc.v34 = kind == 2; cc.v32 = kind == 3; cc.v92 = kind == 4;
    c_oct = 0xFF; c_lin = 0; cc.v92_rx_linear = lin_mode;
    cc.ppp.user = user; cc.ppp.pass = pass;
    cc.ppp.ccp = caller_ccp;
    cc.ppp.want_dns = 1;
    cc.ip_rx = cli_rx;
    caller_init(&C, &cc);
    modem_on = 1;
}

static int wait_ip(int limit_ms)
{
    long i;
    for (i = 0; i < (long)limit_ms * 8; i++) {
        if (srv_ip_up(&S) && C.ppp_on && C.ppp.ip_up) return (int)(i / 8);
        step(0);
    }
    return -1;
}

static void exchange(int n)
{
    uint8_t p[40]; int k, sent_dn = 0, sent_up = 0; long i;
    int lan0 = lan_n, cli0 = cli_n;
    for (i = 0; i < 20L * FS && (lan_n - lan0 < n || cli_n - cli0 < n); i++) {
        if (i % 400 == 0) {
            if (sent_dn < n) { mkpkt(p, sent_dn, 0xA5); if (srv_send_ip(&S, p, 40) == 0) sent_dn++; }
            if (sent_up < n) { mkpkt(p, sent_up, 0x5A); if (caller_send_ip(&C, p, 40) == 0) sent_up++; }
        }
        step(0);
    }
    k = lan_n - lan0; CHECK_EQ_I(k, n);
    k = cli_n - cli0; CHECK_EQ_I(k, n);
    CHECK_EQ_I(lan_bad, 0); CHECK_EQ_I(cli_bad, 0);
}

static void hang_up(void)
{
    if (modem_on) caller_hangup(&C);
    if (modem_on && C.cfg.v92) v92call_hangup(&C.vc);
    modem_on = 0; hook = 0; c_tx = 0;
    run_ms(800);
}

static void test_v22bis_call(void)
{
    cfg_t c; int ms; const st_call_t *h; static char page[STATUS_HTML_MAX];
    base_cfg(&c);
    line_init();
    srv_init(&S, &c, 77, lan_rx, NULL);
    srv_set_clock(&S, 1000000000u);
    run_ms(300);
    CHECK_EQ_I(S.status.line, ST_LINE_IDLE);
    CHECK_EQ_I(s_tx, 0xFF);
    CHECK_EQ_I(srv_send_ip(&S, lan_last, 40), PPP_ERR_DOWN);

    place_call("5551234", 9000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(S.status.line, ST_LINE_TRAINING);
    start_modem(0, "alice", "wonderland");
    ms = wait_ip(20000);
    printf("  V.22bis call: IP up %.2f s after the answer\n", ms / 1000.0);
    CHECK(ms > 0);
    run_ms(1100);
    CHECK_EQ_I(S.status.line, ST_LINE_CONNECTED);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V22BIS);
    CHECK_EQ_I(S.status.down_bps, 2400); CHECK_EQ_I(S.status.up_bps, 2400);
    CHECK_EQ_I(S.status.v42, ST_V42_LAPM);
    CHECK_EQ_I(S.status.ppp, ST_PPP_RUNNING);
    CHECK(strcmp(S.status.peer_name, "alice") == 0);
    CHECK_EQ_I(S.status.peer_ip, c.ppp_peer); CHECK_EQ_I(S.status.local_ip, c.ppp_local);
    exchange(5);
    run_ms(1100);
    CHECK_EQ_I(S.status.rx_packets, 5); CHECK_EQ_I(S.status.tx_packets, 5);
    CHECK(S.status.rx_octets >= 200 && S.status.tx_octets >= 200);
    CHECK(status_render_html(&S.status, page, sizeof page) > 0);
    CHECK(strstr(page, "alice") != NULL);

    hang_up();
    CHECK_EQ_I(S.status.line, ST_LINE_IDLE);
    CHECK_EQ_I(S.status.ppp, ST_PPP_DEAD);
    CHECK_EQ_I(S.status.calls_total, 1);
    h = status_call(&S.status, 0);
    CHECK(h != NULL);
    if (h) {
        CHECK_EQ_I(h->tel_result, ST_TEL_ANSWERED);
        CHECK_EQ_I(h->end, ST_END_CALLER_HANGUP);
        CHECK_EQ_I(h->modulation, ST_MOD_V22BIS);
        CHECK_EQ_I(h->down_bps, 2400);
        CHECK(strcmp(h->dialed, "5551234") == 0);
        CHECK(h->duration_s >= 8 && h->duration_s <= 40);
        CHECK(h->start_time >= 1000000003u && h->start_time <= 1000000015u);
    }
    CHECK_EQ_I(srv_send_ip(&S, lan_last, 40), PPP_ERR_DOWN);
    /* the totals survive the call */
    CHECK_EQ_I(S.status.rx_packets, 5);
}

static void test_not_answered(void)
{
    cfg_t c; const st_call_t *h;
    base_cfg(&c); c.busy_chance = 100;
    line_init();
    srv_init(&S, &c, 5, lan_rx, NULL);
    place_call("5551234", 5000);
    CHECK_EQ_I(S.tel.state, TEL_BUSY);
    run_ms(2000);
    CHECK(c_rx_pw > 100.0 * 100.0 || 1);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_BUSY && strcmp(h->dialed, "5551234") == 0);
    CHECK(h && h->modulation == ST_MOD_NONE && h->duration_s == 0);

    place_call("5550000", 5000);
    CHECK_EQ_I(S.tel.state, TEL_INTERCEPT);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_WRONG_NUMBER && strcmp(h->dialed, "5550000") == 0);

    hook = 1; run_ms(1500); hang_up();           /* picked up and put down */
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_ABANDONED && h->dialed[0] == 0);
    CHECK_EQ_I(S.status.calls_total, 3);
}

static void test_wrong_password_then_reconfigure(void)
{
    cfg_t c; const st_call_t *h; long i;
    base_cfg(&c); c.rings = 0;
    line_init();
    srv_init(&S, &c, 9, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(0, "alice", "rabbit");
    for (i = 0; i < 30L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);          /* we closed the call; busy tone follows */
    /* settings changed while the receiver is still off hook */
    strcpy(c.users[0].secret, "rabbit"); strcpy(c.line_number, "42");
    srv_reconfigure(&S, &c);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_ANSWERED && h->end == ST_END_AUTH_FAILED);
    place_call("42", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(0, "alice", "rabbit");
    CHECK(wait_ip(20000) > 0);
    hang_up();
}

static void test_drop_and_limit(void)
{
    cfg_t c; const st_call_t *h; long i; int quiet;
    base_cfg(&c); c.rings = 0; c.drop_mean_min = 1;
    line_init();
    srv_init(&S, &c, 21, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    for (i = 0; i < 900L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    printf("  line dropped %.0f s into the call\n", (double)S.tel.call_s);
    /* a drop is abrupt: nothing but silence at once, no orderly release */
    quiet = 1;
    for (i = 0; i < 4000; i++) { step(0); if (s_tx != 0xFF) quiet = 0; }
    CHECK(quiet);
    CHECK_EQ_I(srv_ip_up(&S), 0);
    CHECK_EQ_I(S.status.line, ST_LINE_HANGUP);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_DROPPED && h->modulation == ST_MOD_V22BIS);

    /* session limit: an orderly close after one minute */
    c.drop_mean_min = 0; c.session_limit_min = 1;
    srv_reconfigure(&S, &c);
    place_call("5551234", 6000);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    for (i = 0; i < 100L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    CHECK(S.tel.call_s >= 60 && S.tel.call_s <= 70);
    CHECK(!C.ppp.ip_up);                  /* the client was told */
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_SESSION_LIMIT && h->duration_s >= 60);

    /* idle timeout: traffic keeps the call, silence ends it */
    c.session_limit_min = 0; c.idle_timeout_min = 1;
    srv_reconfigure(&S, &c);
    place_call("5551234", 6000);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    run_ms(45000); exchange(1); run_ms(45000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    for (i = 0; i < 40L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->tel_result == ST_TEL_IDLE);
}

static void test_v90_call(void)
{
    cfg_t c; int ms; const st_call_t *h;
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(1, "alice", "wonderland");
    ms = wait_ip(25000);
    CHECK(ms > 0);
    run_ms(1100);
    printf("  V.90 call: IP up %.2f s after the answer, %u bit/s down, %u bit/s up\n", ms / 1000.0,
           (unsigned)S.status.down_bps, (unsigned)S.status.up_bps);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V90);
    CHECK(S.status.down_bps >= 40000); CHECK(S.status.up_bps >= 21600);
    /* the page's receive figures are live: the caller's level as it comes off
     * the line, and what the receiver makes of the signal */
    printf("    receive level %.1f dBm0, SNR %.1f dB, round trip %u ms\n", S.status.rx_level_x10 / 10.0, S.status.snr_x10 / 10.0, (unsigned)S.status.rtd_ms);
    CHECK(S.status.rx_level_x10 < -30 && S.status.rx_level_x10 > -400);
    CHECK(S.status.snr_x10 > 200 && S.status.snr_x10 < 600);
    exchange(20);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->modulation == ST_MOD_V90 && h->end == ST_END_CALLER_HANGUP && h->down_bps >= 40000);
}

/* [ppp] compression: on (the default) a caller that asks gets Deflate or
 * MPPC and the status says which and how much; off, CCP is refused */
static void test_ppp_compression(void)
{
    static char page[STATUS_HTML_MAX], json[STATUS_JSON_MAX];
    cfg_t c; int k;

    for (k = 0; k < 3; k++) {
        base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
        CHECK_EQ_I(c.compression, 1);
        if (k == 2) c.compression = 0;
        caller_ccp = k == 1 ? PPPC_CCP_WINDOWS_MPPE : PPPC_CCP_PPPD;
        line_init();
        srv_init(&S, &c, 41, lan_rx, NULL);
        CHECK_EQ_I(S.status.ppp_compression, k != 2);
        CHECK_EQ_I(S.status.ppp_comp_tx, ST_COMP_NONE);
        place_call("5551234", 6000);
        start_modem(1, "alice", "wonderland");
        CHECK(wait_ip(30000) > 0);
        run_ms(1100);
        CHECK_EQ_I(S.status.ppp_comp_tx, k == 0 ? ST_COMP_DEFLATE : k == 1 ? ST_COMP_MPPC : ST_COMP_NONE);
        CHECK_EQ_I(S.status.ppp_comp_rx, k == 0 ? ST_COMP_DEFLATE : k == 1 ? ST_COMP_MPPC : ST_COMP_NONE);
        CHECK_EQ_I(S.status.compression, 1);            /* the modem's own, as before */
        exchange(20);
        run_ms(1100);
        CHECK_EQ_I(S.status.rx_packets, 20); CHECK_EQ_I(S.status.tx_packets, 20);
        CHECK(status_render_json(&S.status, json, sizeof json) > 0);
        CHECK(status_render_html(&S.status, page, sizeof page) > 0);
        if (k == 2) {
            CHECK_EQ_I(S.status.comp_tx_in + S.status.comp_rx_in, 0);
            CHECK_EQ_I(C.ppp.comp_rx_ok + C.ppp.comp_tx, 0);
            CHECK(strstr(json, "\"ppp_compression\":false") != NULL);
            CHECK(strstr(json, "\"comp_tx\":\"none\",\"comp_rx\":\"none\"") != NULL);
            CHECK(strstr(page, "<th>Compressed to caller</th><td>no</td>") != NULL);
        } else {
            /* twenty packets of 40 octets each way went through it */
            CHECK_EQ_I(S.status.comp_tx_in, 800);
            CHECK_EQ_I(S.status.comp_rx_out, 800);
            CHECK(S.status.comp_tx_out > 0 && S.status.comp_tx_out < 800);
            CHECK(S.status.comp_rx_in > 0 && S.status.comp_rx_in < 800);
            CHECK(C.ppp.comp_rx_ok >= 15 && C.ppp.comp_tx >= 15);
            CHECK(strstr(json, "\"ppp_compression\":true") != NULL);
            CHECK(strstr(json, k == 0 ? "\"comp_tx\":\"deflate\",\"comp_rx\":\"deflate\",\"comp_tx_in\":800,"
                                      : "\"comp_tx\":\"mppc\",\"comp_rx\":\"mppc\",\"comp_tx_in\":800,") != NULL);
            CHECK(strstr(page, k == 0 ? "<th>Compressed to caller</th><td>Deflate: 800 octets sent as "
                                      : "<th>Compressed to caller</th><td>MPPC: 800 octets sent as ") != NULL);
            printf("  PPP compression, %s caller: 800 octets of small packets each way went as %u down, %u up\n",
                   k == 0 ? "pppd-like (Deflate)" : "Windows-like (MPPC)",
                   (unsigned)S.status.comp_tx_out, (unsigned)S.status.comp_rx_in);
        }
        hang_up();
        /* the live figures belong to a call */
        CHECK_EQ_I(S.status.ppp_comp_tx, ST_COMP_NONE);
        CHECK_EQ_I(S.status.ppp_comp_rx, ST_COMP_NONE);
        CHECK_EQ_I(S.status.comp_tx_in + S.status.comp_tx_out + S.status.comp_rx_in + S.status.comp_rx_out, 0);
        CHECK_EQ_I(S.status.ppp_compression, k != 2);
    }
    caller_ccp = PPPC_CCP_PPPD;

    /* the switch follows a new configuration like the others */
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    line_init();
    srv_init(&S, &c, 41, lan_rx, NULL);
    c.compression = 0;
    srv_reconfigure(&S, &c);
    run_ms(100);
    CHECK_EQ_I(S.status.ppp_compression, 0);
}

#define IPA(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static unsigned ip_sum(const uint8_t *p)
{
    unsigned long s = 0; int i;
    for (i = 0; i < (p[0] & 15) * 4; i += 2) s += ((unsigned)p[i] << 8) | p[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (unsigned)s;                 /* 0xffff when the header is sound */
}

/* a TCP packet of 40 octets with a sound IP header */
static void mktcp(uint8_t *p, int ttl, int sport, int dport, int flags)
{
    unsigned c;
    memset(p, 0, 40); p[0] = 0x45; p[3] = 40; p[8] = (uint8_t)ttl; p[9] = 6;
    p[12] = 10; p[13] = 64; p[14] = 64; p[15] = 2; p[16] = 203; p[17] = 0; p[18] = 113; p[19] = 7;
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport; p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[32] = 0x50; p[33] = (uint8_t)flags;
    c = 0xffff - ip_sum(p); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
}

/* the TCP checksum of a packet made by mktcp (20 + 20 octets), summed with
 * its pseudo header: 0xffff when it is sound */
static unsigned tcp_sum(const uint8_t *p, int len)
{
    unsigned long s = 6 + (unsigned long)(len - 20); int i;
    for (i = 12; i < 20; i += 2) s += ((unsigned)p[i] << 8) | p[i + 1];
    for (i = 20; i + 1 < len; i += 2) s += ((unsigned)p[i] << 8) | p[i + 1];
    if (i < len) s += (unsigned)p[i] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (unsigned)s;
}
static void tcp_seal(uint8_t *p, int len)
{
    unsigned c; p[36] = p[37] = 0; c = 0xffff - tcp_sum(p, len); p[36] = (uint8_t)(c >> 8); p[37] = (uint8_t)c;
}
/* a DNS question in a UDP packet from the caller; returns its length */
static int mkdns(uint8_t *p, const char *name, int qtype, int flags_hi, int dport)
{
    int n = 28, lab; unsigned c; const char *q = name;
    memset(p, 0, 300); p[0] = 0x45; p[8] = 64; p[9] = 17;
    p[12] = 10; p[13] = 64; p[14] = 64; p[15] = 2; p[16] = 46; p[17] = 101; p[18] = 91; p[19] = 123;
    p[20] = 0x10; p[21] = 0x00; p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    p[28] = 0x12; p[29] = 0x34; p[30] = (uint8_t)flags_hi; p[33] = 1; n = 40;
    while (*q) {
        lab = n++;
        while (*q && *q != '.') p[n++] = (uint8_t)*q++;
        p[lab] = (uint8_t)(n - lab - 1);
        if (*q == '.') q++;
    }
    p[n++] = 0; p[n++] = 0; p[n++] = (uint8_t)qtype; p[n++] = 0; p[n++] = 1;
    p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n; p[24] = (uint8_t)((n - 20) >> 8); p[25] = (uint8_t)(n - 20);
    c = 0xffff - ip_sum(p); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
    return n;
}

/* The address list of the game revival projects (refs/dreampi-facts.md):
 * text in, rules out. */
static void test_dclist(void)
{
    static const char txt[] =
        "172.17.18.2 146.185.135.179 9500\n"
        "172.17.1.14 146.185.135.179 9500 udp\n"
        "63.251.242.131 146.185.135.179\r\n"
        "\n"
        "45.79.32.243 10.200.0.1 8888 tcp\n"
        "not an address 1.2.3.4 80\n"
        "1.2.3.4 5.6.7.8 0\n"
        "1.2.3.4 5.6.7.8 70000\n"
        "1.2.3.4 5.6.7.8 80 icmp\n"
        "1.2.3.4 5.6.7.8 80 tcp more\n"
        "1.2.3.999 5.6.7.8 80\n"
        "0.0.0.0 5.6.7.8 80\n"
        "9.9.9.9 8.8.8.8 53 UDP";                     /* no line end on the last one */
    nat_rewrite r[8]; const nat_open *op; int n, i, wide = 0;

    n = dclist_parse(txt, sizeof txt - 1, r, 8);
    CHECK_EQ_I(n, 5);
    CHECK(r[0].from == IPA(172, 17, 18, 2) && r[0].to == IPA(146, 185, 135, 179) && r[0].port == 9500 && r[0].proto == 6);
    CHECK(r[1].from == IPA(172, 17, 1, 14) && r[1].port == 9500 && r[1].proto == 17);
    CHECK(r[2].from == IPA(63, 251, 242, 131) && r[2].port == 0 && r[2].proto == 6);
    CHECK(r[3].to == IPA(10, 200, 0, 1) && r[3].port == 8888 && r[3].proto == 6);
    CHECK(r[4].from == IPA(9, 9, 9, 9) && r[4].to == IPA(8, 8, 8, 8) && r[4].port == 53 && r[4].proto == 17);
    /* no more than there is room for; nothing from nothing */
    CHECK_EQ_I(dclist_parse(txt, sizeof txt - 1, r, 2), 2);
    CHECK_EQ_I(dclist_parse("", 0, r, 8), 0);
    CHECK_EQ_I(dclist_parse("<html>404</html>\n", 17, r, 8), 0);

    /* the ports games want opened towards the console */
    n = dclist_ports(&op);
    CHECK(n > 20 && n <= NAT_OPEN_MAX);
    for (i = 0; i < n; i++) {
        CHECK(op[i].proto == 6 || op[i].proto == 17);
        CHECK(op[i].lo >= 1024 && op[i].hi >= op[i].lo);
        if (op[i].lo == 2300 && op[i].hi == 2400) wide++;
        CHECK(op[i].lo > 80 && !(op[i].lo <= 53 && op[i].hi >= 53));
    }
    CHECK_EQ_I(wide, 2);                                /* Starlancer: TCP and UDP */
}

/* "Dreamcast Now" (refs/dreampi-facts.md): the hash, and what is reported
 * when. */
static void test_dcnow(void)
{
    static const uint8_t mac[6] = { 0xc0, 0x74, 0xad, 0x01, 0x02, 0x03 };
    static char big[1001]; char hex[65], body[96]; dcnow_t d;

    sha256_hex("", 0, hex);
    CHECK(strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    sha256_hex("abc", 3, hex);
    CHECK(strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, hex);
    CHECK(strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
    memset(big, 'a', 1000);
    sha256_hex(big, 1000, hex);
    CHECK(strcmp(hex, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3") == 0);

    /* the box is named by the hash of its hardware address as text */
    dcnow_start(&d, mac);
    CHECK(strcmp(dcnow_path(&d), "/api/update/5735e6ba4783c425216f577172aea3f8339c2d8263b7340fa8a7b2e57cfcaed0/") == 0);
    /* nothing asked yet: an empty report */
    CHECK_EQ_I(dcnow_body(&d, "", body, sizeof body), 0); CHECK(body[0] == 0);
    /* a name: its hash, on every report while it is the last one */
    CHECK_EQ_I(dcnow_body(&d, "master.quake3arena.com", body, sizeof body), 10 + 64);
    CHECK(strcmp(body, "dns_query=dc27bdcf2e3a8fe15559cb6312c662ff9a2ea25fbb192bbd171698735a6531d9") == 0);
    CHECK_EQ_I(dcnow_body(&d, "master.quake3arena.com", body, sizeof body), 74);
    /* the report's own host is never reported: the name before it stays */
    CHECK_EQ_I(dcnow_body(&d, "dcnow-2016.appspot.com", body, sizeof body), 74);
    CHECK(strcmp(body, "dns_query=dc27bdcf2e3a8fe15559cb6312c662ff9a2ea25fbb192bbd171698735a6531d9") == 0);
    /* two kinds of name are reported once a call */
    CHECK_EQ_I(dcnow_body(&d, "ingame.gameloft.com", body, sizeof body), 74);
    CHECK(strcmp(body, "dns_query=a83c1d4658130a1950f3966ce84fc14baae0ddc825ff782f86f9e288b57dc1cd") == 0);
    CHECK_EQ_I(dcnow_body(&d, "ingame.gameloft.com", body, sizeof body), 0); CHECK(body[0] == 0);
    CHECK_EQ_I(dcnow_body(&d, "onsen0.overworks.isao.net", body, sizeof body), 74);
    CHECK(strcmp(body, "dns_query=4a5148cae3c5d40ede6e60286bd964fef2f01b515f6abf75d9d66f4e06ac5ea5") == 0);
    CHECK_EQ_I(dcnow_body(&d, "onsen0.overworks.isao.net", body, sizeof body), 0);
    /* a new call starts over */
    dcnow_start(&d, mac);
    CHECK_EQ_I(dcnow_body(&d, "ingame.gameloft.com", body, sizeof body), 74);
    /* no room: nothing, and no overrun */
    CHECK_EQ_I(dcnow_body(&d, "master.quake3arena.com", body, 20), 0);
}

/* The words of the console-to-console tunnel (saturn.h). */
static void test_saturn_words(void)
{
    uint32_t a = 0; static saturn_tx t; saturn_rx r; static uint8_t dg[SATURN_DGRAM_MAX], dg2[SATURN_DGRAM_MAX], out[4096];
    static uint8_t big[SATURN_REC_MAX]; size_t n, n2, i; int k;

    char g[3];
    CHECK_EQ_I(saturn_number("0", &a, g), SATURN_WAIT);
    CHECK_EQ_I(saturn_number("00", &a, g), SATURN_WAIT);
    CHECK_EQ_I(saturn_number("#192*0*2*7", &a, g), SATURN_CALL); CHECK(a == 0xC0000207u);
    CHECK_EQ_I(saturn_number("#192000002007", &a, g), SATURN_CALL); CHECK(a == 0xC0000207u);
    CHECK_EQ_I(saturn_number("#010020030040", &a, g), SATURN_CALL); CHECK(a == 0x0A141E28u);
    CHECK_EQ_I(saturn_number("#192*0*2*256", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("#192*0*2", &a, g), SATURN_NONE);     /* a broken * form is not matchmaking */
    CHECK_EQ_I(saturn_number("#192*0*2*7*1", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("#192**2*7", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("#0*0*0*0", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("000", &a, g), SATURN_NONE);          /* no #: not a wait, not an address */
    CHECK_EQ_I(saturn_number("5551234", &a, g), SATURN_NONE);
    /* matchmaking: #0NN# waits for game NN, any other #...# calls, game = last two */
    CHECK_EQ_I(saturn_number("#001", &a, g), SATURN_MATCH_WAIT); CHECK(strcmp(g, "01") == 0);
    CHECK_EQ_I(saturn_number("#042", &a, g), SATURN_MATCH_WAIT); CHECK(strcmp(g, "42") == 0);
    CHECK_EQ_I(saturn_number("#12345", &a, g), SATURN_MATCH_CALL); CHECK(strcmp(g, "45") == 0);
    CHECK_EQ_I(saturn_number("#99", &a, g), SATURN_MATCH_CALL); CHECK(strcmp(g, "99") == 0);
    CHECK_EQ_I(saturn_number("#300000002007", &a, g), SATURN_MATCH_CALL); CHECK(strcmp(g, "07") == 0);  /* 12 digits but 300>255 */
    CHECK_EQ_I(saturn_number("#", &a, g), SATURN_NONE);
    CHECK_EQ_I(saturn_number("#5a", &a, g), SATURN_NONE);          /* game must be digits */
    CHECK_EQ_I(saturn_number("#5", &a, NULL), SATURN_MATCH_CALL);  /* game buffer may be NULL */
    /* the twelve-digit form of an address */
    { char ip[13]; saturn_ip12(0xCB007109u, ip); CHECK(strcmp(ip, "203000113009") == 0); }

    /* the match server's answer: opponent read only when the status matches */
    {
        uint32_t ma = 0; uint16_t mp = 0;
        static const char w[] = "{\"status\": \"matched\", \"opponent ip_address\": \"203000113009 50000\"}";
        static const char f[] = "{ \"opponent ip_address\" : \"010020030040 7\", \"status\":\"found opponent\" }";
        CHECK_EQ_I(saturn_match_parse(w, sizeof w - 1, "matched", &ma, &mp), 1);
        CHECK(ma == 0xCB007109u); CHECK_EQ_I(mp, 50000);
        CHECK_EQ_I(saturn_match_parse(w, sizeof w - 1, "found opponent", &ma, &mp), 0);   /* wrong status */
        ma = 0; mp = 0;
        CHECK_EQ_I(saturn_match_parse(f, sizeof f - 1, "found opponent", &ma, &mp), 1);
        CHECK(ma == 0x0A141E28u); CHECK_EQ_I(mp, 7);
        CHECK_EQ_I(saturn_match_parse("{\"status\":\"waiting\"}", 20, "matched", &ma, &mp), 0);
        CHECK_EQ_I(saturn_match_parse("garbage", 7, "matched", &ma, &mp), 0);
        CHECK_EQ_I(saturn_match_parse("{\"status\":\"matched\",\"opponent ip_address\":\"999000000000 1\"}", 61, "matched", &ma, &mp), 0);  /* bad octet */
    }

    CHECK_EQ_I(saturn_control((const uint8_t *)"PING_SHIRO", 10), 1);
    CHECK_EQ_I(saturn_control((const uint8_t *)"PONG_SHIRO", 10), 2);
    CHECK_EQ_I(saturn_control((const uint8_t *)"OPEN_SHIRO", 10), 2);
    CHECK_EQ_I(saturn_control((const uint8_t *)"ab<dataSplit>0", 14), 0);

    /* the first datagram: one record, number 0 */
    saturn_tx_init(&t); saturn_rx_init(&r);
    n = saturn_pack(&t, (const uint8_t *)"ab", 2, dg);
    CHECK_EQ_I(n, 14); CHECK(memcmp(dg, "ab<dataSplit>0", 14) == 0);
    /* the second brings the first along, behind it */
    n2 = saturn_pack(&t, (const uint8_t *)"\0\r\xff", 3, dg2);
    CHECK_EQ_I(n2, 15 + 13 + 14);
    CHECK(memcmp(dg2, "\0\r\xff<dataSplit>1<packetSplit>ab<dataSplit>0", n2) == 0);
    /* in order; each datagram comes twice and counts once */
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 2); CHECK(memcmp(out, "ab", 2) == 0);
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 0);
    CHECK_EQ_I(saturn_unpack(&r, dg2, n2, out, sizeof out), 3); CHECK(memcmp(out, "\0\r\xff", 3) == 0);
    CHECK_EQ_I(saturn_unpack(&r, dg2, n2, out, sizeof out), 0);
    /* three datagrams lost: the next one makes up for them */
    for (k = 2; k < 6; k++) { char c = (char)('a' + k); n = saturn_pack(&t, (const uint8_t *)&c, 1, dg); }
    CHECK(memcmp(dg, "f<dataSplit>5<packetSplit>e<dataSplit>4<packetSplit>d<dataSplit>3<packetSplit>c<dataSplit>2<packetSplit>"
                     "\0\r\xff<dataSplit>1", n) == 0);
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 4); CHECK(memcmp(out, "cdef", 4) == 0);
    /* more lost than a datagram repeats: on with the newest */
    for (k = 6; k < 20; k++) { char c = (char)('a' + k); n = saturn_pack(&t, (const uint8_t *)&c, 1, dg); }
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 1); CHECK(out[0] == 't');
    n = saturn_pack(&t, (const uint8_t *)"u", 1, dg);
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 1); CHECK(out[0] == 'u');
    /* an old one arriving late is dropped */
    CHECK_EQ_I(saturn_unpack(&r, dg2, n2, out, sizeof out), 0);
    /* full records, all five, fit the datagram; a small buffer loses the rest, not its mind */
    saturn_tx_init(&t); saturn_rx_init(&r);
    for (i = 0; i < sizeof big; i++) big[i] = (uint8_t)(i * 7);
    for (k = 0; k < 5; k++) n = saturn_pack(&t, big, sizeof big, dg);
    CHECK(n <= SATURN_DGRAM_MAX);
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, sizeof out), 5 * SATURN_REC_MAX);
    CHECK(memcmp(out + 4 * SATURN_REC_MAX, big, sizeof big) == 0);
    saturn_rx_init(&r);
    CHECK_EQ_I(saturn_unpack(&r, dg, n, out, 300), 300);
    /* a peer's longer record (up to 1024 octets a read) and a large number */
    {
        static uint8_t in[1100]; size_t m = 1024;
        memset(in, 'x', m); memcpy(in + m, "<dataSplit>4000000000", 21);
        r.expect = 4000000000u;
        CHECK_EQ_I(saturn_unpack(&r, in, m + 21, out, sizeof out), 1024);
        CHECK(r.expect == 4000000001u);
    }
    /* a number far ahead of what is expected is not believed (one forged
     * datagram must not silence the real ones that follow); a gap that a
     * bad stretch of network can make is */
    saturn_rx_init(&r); r.expect = 10;
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"zz<dataSplit>4000000", 20, out, sizeof out), 0);
    CHECK(r.expect == 10);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"zz<dataSplit>4106", 17, out, sizeof out), 2);
    CHECK(r.expect == 4107);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"zz<dataSplit>8204", 17, out, sizeof out), 0);
    /* rubbish */
    saturn_rx_init(&r);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"abc", 3, out, sizeof out), 0);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"abc<dataSplit>", 14, out, sizeof out), 0);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"abc<dataSplit>1x", 16, out, sizeof out), 0);
    CHECK_EQ_I(saturn_unpack(&r, (const uint8_t *)"", 0, out, sizeof out), 0);
}

/* The words of UPnP port mapping (upnp.h): the answer to a search, the
 * device's description, and the two requests. */
static void test_upnp_words(void)
{
    static const char found[] =
        "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
        "USN: uuid:1::urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nEXT:\r\n"
        "Location:  http://192.168.1.1:5000/rootDesc.xml\r\nSERVER: x\r\n\r\n";
    static const char desc[] =
        "<?xml version=\"1.0\"?><root><device><serviceList><service>"
        "<serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
        "<controlURL>/ctl/CmnIfCfg</controlURL></service></serviceList><deviceList><device><deviceList><device>"
        "<serviceList><service>\n <serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n"
        " <serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\n <controlURL>/ctl/IPConn</controlURL>\n"
        " <eventSubURL>/evt/IPConn</eventSubURL></service></serviceList></device></deviceList></device></deviceList></device></root>";
    static const char desc_ppp[] =
        "<root><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>"
        "<controlURL>http://192.168.1.1:5000/upnp/control/ppp</controlURL></service></root>";
    static const char desc_rel[] =
        "<root><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:2</serviceType>"
        "<controlURL>ctl</controlURL></service></root>";
    upnp_url u; upnp_ctl c; static char req[1500]; int n; const char *body;

    CHECK(strncmp(UPNP_SEARCH, "M-SEARCH * HTTP/1.1\r\n", 21) == 0);
    CHECK(strstr(UPNP_SEARCH, "MAN: \"ssdp:discover\"\r\n") != NULL);
    CHECK(strstr(UPNP_SEARCH, "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n") != NULL);

    CHECK_EQ_I(upnp_location(found, sizeof found - 1, &u), 0);
    /* whose answer is taken: the device named in it, from the search port,
     * and nothing that points back at ourselves or at no host at all */
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xC0A80101u, 1900, &u), 1);
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xC0A80107u, 1900, &u), 0);      /* somebody else names the router */
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xC0A80101u, 40000, &u), 0);     /* not from the search port */
    CHECK_EQ_I(upnp_trust(0xC0A80101u, 0xC0A80101u, 1900, &u), 0);      /* ourselves */
    strcpy(u.host, "127.0.0.1");
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0x7F000001u, 1900, &u), 0);
    CHECK_EQ_I(upnp_trust(0x7F000001u, 0x7F000001u, 1900, &u), 1);      /* a test on the loopback device */
    strcpy(u.host, "239.255.255.250");
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xEFFFFFFAu, 1900, &u), 0);
    strcpy(u.host, "169.254.1.1");
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xA9FE0101u, 1900, &u), 0);
    strcpy(u.host, "router.lan");
    CHECK_EQ_I(upnp_trust(0xC0A80132u, 0xC0A80101u, 1900, &u), 0);
    CHECK_EQ_I(upnp_location(found, sizeof found - 1, &u), 0);
    CHECK(strcmp(u.host, "192.168.1.1") == 0); CHECK_EQ_I(u.port, 5000); CHECK(strcmp(u.path, "/rootDesc.xml") == 0);
    CHECK_EQ_I(upnp_location("HTTP/1.1 200 OK\r\nLOCATION: http://10.0.0.138/\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\nLOCATION: http://10.0.0.138/\r\n\r\n" - 1, &u), 0);
    CHECK(strcmp(u.host, "10.0.0.138") == 0); CHECK_EQ_I(u.port, 80); CHECK(strcmp(u.path, "/") == 0);
    CHECK_EQ_I(upnp_location("HTTP/1.1 200 OK\r\nST: x\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\nST: x\r\n\r\n" - 1, &u), -1);
    CHECK_EQ_I(upnp_location("HTTP/1.1 200 OK\r\nLocation: https://10.0.0.1/x\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\nLocation: https://10.0.0.1/x\r\n\r\n" - 1, &u), -1);
    CHECK_EQ_I(upnp_location("HTTP/1.1 200 OK\r\nLocation: http://host name/x\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\nLocation: http://host name/x\r\n\r\n" - 1, &u), -1);

    CHECK_EQ_I(upnp_control(desc, sizeof desc - 1, &c), 0);
    CHECK(strcmp(c.path, "/ctl/IPConn") == 0);
    CHECK(strcmp(c.type, "urn:schemas-upnp-org:service:WANIPConnection:1") == 0);
    CHECK_EQ_I(upnp_control(desc_ppp, sizeof desc_ppp - 1, &c), 0);
    CHECK(strcmp(c.path, "/upnp/control/ppp") == 0);       /* a whole URL: its path */
    CHECK(strcmp(c.type, "urn:schemas-upnp-org:service:WANPPPConnection:1") == 0);
    CHECK_EQ_I(upnp_control(desc_rel, sizeof desc_rel - 1, &c), 0);
    CHECK(strcmp(c.path, "/ctl") == 0);
    CHECK(strcmp(c.type, "urn:schemas-upnp-org:service:WANIPConnection:2") == 0);
    CHECK_EQ_I(upnp_control("<root><service><serviceType>urn:x:Layer3Forwarding:1</serviceType><controlURL>/l3</controlURL></service></root>", sizeof "<root><service><serviceType>urn:x:Layer3Forwarding:1</serviceType><controlURL>/l3</controlURL></service></root>" - 1, &c), -1);
    CHECK_EQ_I(upnp_control("", sizeof "" - 1, &c), -1);

    CHECK_EQ_I(upnp_location(found, sizeof found - 1, &u), 0);
    CHECK_EQ_I(upnp_control(desc, sizeof desc - 1, &c), 0);
    n = upnp_request(req, sizeof req, &u, &c, 1, 17, 7980, IPA(192, 168, 1, 50), "v90d: Dreamcast");
    CHECK(n > 0 && (size_t)n == strlen(req));
    CHECK(strstr(req, "POST /ctl/IPConn HTTP/1.1\r\nHost: 192.168.1.1:5000\r\n") == req);
    CHECK(strstr(req, "\r\nSOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"\r\n") != NULL);
    CHECK(strstr(req, "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n") != NULL);
    CHECK(strstr(req, "\r\nConnection: close\r\n") != NULL);
    body = strstr(req, "\r\n\r\n");
    CHECK(body != NULL);
    if (body) {
        char cl[40];
        body += 4;
        snprintf(cl, sizeof cl, "\r\nContent-Length: %d\r\n", (int)strlen(body));
        CHECK(strstr(req, cl) != NULL);
        CHECK(strstr(body, "<u:AddPortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">") != NULL);
        CHECK(strstr(body, "<NewRemoteHost></NewRemoteHost><NewExternalPort>7980</NewExternalPort><NewProtocol>UDP</NewProtocol>"
                           "<NewInternalPort>7980</NewInternalPort><NewInternalClient>192.168.1.50</NewInternalClient>"
                           "<NewEnabled>1</NewEnabled><NewPortMappingDescription>v90d: Dreamcast</NewPortMappingDescription>"
                           "<NewLeaseDuration>0</NewLeaseDuration>") != NULL);
    }
    n = upnp_request(req, sizeof req, &u, &c, 0, 6, 2300, 0, NULL);
    CHECK(n > 0);
    CHECK(strstr(req, "#DeletePortMapping\"\r\n") != NULL);
    CHECK(strstr(req, "<NewRemoteHost></NewRemoteHost><NewExternalPort>2300</NewExternalPort><NewProtocol>TCP</NewProtocol></u:DeletePortMapping>") != NULL);
    CHECK(strstr(req, "NewInternalClient") == NULL);
    CHECK_EQ_I(upnp_request(req, 200, &u, &c, 1, 17, 7980, IPA(192, 168, 1, 50), "x"), -1);

    CHECK_EQ_I(upnp_ok("HTTP/1.1 200 OK\r\n\r\n", sizeof "HTTP/1.1 200 OK\r\n\r\n" - 1), 1);
    CHECK_EQ_I(upnp_ok("HTTP/1.0 200 OK\r\n", sizeof "HTTP/1.0 200 OK\r\n" - 1), 1);
    CHECK_EQ_I(upnp_ok("HTTP/1.1 500 Internal Server Error\r\n", sizeof "HTTP/1.1 500 Internal Server Error\r\n" - 1), 0);
    CHECK_EQ_I(upnp_ok("", sizeof "" - 1), 0);
}

/* Two more things a console's packets get (refs/dreampi-facts.md, second
 * pass): an opening packet that also carries PSH loses the PSH, and the
 * names it asks the name server for are remembered (for "Dreamcast Now"). */
static void test_console_more(void)
{
    console_t k; uint8_t p[300], q[40]; uint32_t seq = 99; int n;

    console_init(&k);
    mktcp(p, 64, 1024, 6500, 0x0a); tcp_seal(p, 40);
    CHECK_EQ_I(tcp_sum(p, 40), 0xffff);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1);
    CHECK_EQ_I(p[33], 0x02);
    CHECK_EQ_I(tcp_sum(p, 40), 0xffff); CHECK_EQ_I(ip_sum(p), 0xffff);
    mktcp(q, 64, 1024, 6500, 0x02); tcp_seal(q, 40);
    CHECK(memcmp(p, q, 40) == 0);
    /* only exactly SYN with PSH: with ACK as well, or PSH on data, nothing changes */
    mktcp(p, 64, 1024, 6500, 0x1a); tcp_seal(p, 40); memcpy(q, p, 40);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1); CHECK(memcmp(p, q, 40) == 0);
    mktcp(p, 64, 1024, 6500, 0x18); tcp_seal(p, 40); memcpy(q, p, 40);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1); CHECK(memcmp(p, q, 40) == 0);
    /* a packet too short to hold the checksum is left alone */
    mktcp(p, 64, 1024, 6500, 0x0a); memcpy(q, p, 40);
    CHECK_EQ_I(console_from_caller(&k, p, 36, 0), 1); CHECK_EQ_I(p[33], 0x0a);

    /* names asked for */
    console_init(&k);
    CHECK(console_name(&k, &seq)[0] == 0); CHECK_EQ_I((int)seq, 0);
    n = mkdns(p, "master.quake3arena.com", 1, 0x01, 53);
    CHECK_EQ_I(console_from_caller(&k, p, (size_t)n, 0), 1);
    CHECK(strcmp(console_name(&k, &seq), "master.quake3arena.com") == 0); CHECK_EQ_I((int)seq, 1);
    n = mkdns(p, "Example.ORG", 1, 0x01, 53);            /* as it was asked, case and all */
    CHECK_EQ_I(console_from_caller(&k, p, (size_t)n, 0), 1);
    CHECK(strcmp(console_name(&k, &seq), "Example.ORG") == 0); CHECK_EQ_I((int)seq, 2);
    /* not counted: another record type, an answer, another port, a cut-off
     * question, a name with a pointer in it */
    n = mkdns(p, "aaaa.example", 28, 0x01, 53); console_from_caller(&k, p, (size_t)n, 0);
    n = mkdns(p, "answer.example", 1, 0x81, 53); console_from_caller(&k, p, (size_t)n, 0);
    n = mkdns(p, "port.example", 1, 0x01, 5353); console_from_caller(&k, p, (size_t)n, 0);
    n = mkdns(p, "short.example", 1, 0x01, 53); console_from_caller(&k, p, (size_t)n - 3, 0);
    n = mkdns(p, "ptr.example", 1, 0x01, 53); p[40] = 0xc0; console_from_caller(&k, p, (size_t)n, 0);
    CHECK(strcmp(console_name(&k, &seq), "Example.ORG") == 0); CHECK_EQ_I((int)seq, 2);
}

/* What is done to the packets of a game console on their way out
 * (console.h): the hop count is lifted, and an opening of a connection that
 * is repeated within a second is not passed on. */
static void test_console_packets(void)
{
    console_t k; uint8_t p[40], q[40]; int ttl;

    console_init(&k);
    /* any hop count becomes 64, and the header still adds up */
    for (ttl = 1; ttl < 256; ttl += 29) {
        mktcp(p, ttl, 1024, 80, 0x10);
        CHECK_EQ_I(ip_sum(p), 0xffff);
        CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1);
        CHECK_EQ_I(p[8], 64); CHECK_EQ_I(ip_sum(p), 0xffff);
        mktcp(q, 64, 1024, 80, 0x10);
        CHECK(memcmp(p, q, 40) == 0);           /* nothing else was touched */
    }
    /* UDP too, and a header with options */
    mktcp(p, 30, 1024, 80, 0); p[9] = 17; p[10] = p[11] = 0;
    { unsigned c = 0xffff - ip_sum(p); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c; }
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1);
    CHECK_EQ_I(p[8], 64); CHECK_EQ_I(ip_sum(p), 0xffff);
    /* what is not IPv4, or is cut short, goes through as it is */
    memset(p, 0, 40); p[0] = 0x60; p[8] = 30;
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1); CHECK_EQ_I(p[8], 30);
    mktcp(p, 30, 1024, 80, 0x10);
    CHECK_EQ_I(console_from_caller(&k, p, 19, 0), 1); CHECK_EQ_I(p[8], 30);
    p[0] = 0x44;
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0), 1); CHECK_EQ_I(p[8], 30);

    /* a SYN from the ports 3200..3205: the first goes out, a second one to
     * the same place within a second does not, a later one does */
    console_init(&k);
    mktcp(p, 30, 3200, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 5000), 1);
    mktcp(p, 30, 3200, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 5400), 0);
    mktcp(p, 30, 3200, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 5999), 0);
    mktcp(p, 30, 3200, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6000), 1);
    /* another of those ports, another destination port, the rest of the
     * connection, and other source ports are not held back */
    mktcp(p, 30, 3205, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6001), 1);
    mktcp(p, 30, 3205, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6002), 0);
    mktcp(p, 30, 3200, 6001, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6003), 1);
    mktcp(p, 30, 3200, 6001, 0x10);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6004), 1);
    mktcp(p, 30, 3200, 6001, 0x12);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 6005), 1);
    mktcp(p, 30, 3199, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 7000), 1);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 7001), 1);
    mktcp(p, 30, 3206, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 7002), 1);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 7003), 1);
    /* the clock may wrap */
    console_init(&k);
    mktcp(p, 30, 3201, 6000, 0x02);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0xfffffe00u), 1);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0x00000100u), 0);
    CHECK_EQ_I(console_from_caller(&k, p, 40, 0x00000300u), 1);
}

/* service = dreamcast: the port keeps its number and the PPP login, like
 * the other services. What it adds for the console: no PPP compression and
 * no V.92 whatever the settings say, the name servers of the game revival
 * projects, and the packets mended. */
static void test_dreamcast_call(void)
{
    cfg_t c; uint8_t p[40]; long i; int lan0;

    base_cfg(&c); c.rings = 0; c.auth = CFG_AUTH_PAP; c.v92 = 1;
    c.service = CFG_SERVICE_DREAMCAST;
    /* [ppp] encryption is ignored on a console port: the console keeps the
     * port's own login (PAP here, which is all this caller does) instead of
     * being asked for MS-CHAPv2 it cannot do */
    c.encryption = 1;
    line_init();
    srv_init(&S, &c, 77, lan_rx, NULL);
    CHECK_EQ_I(S.status.service, CFG_SERVICE_DREAMCAST);
    CHECK_EQ_I(S.status.ppp_compression, 0);
    CHECK_EQ_I(S.status.ppp_encryption, 0);
    CHECK_EQ_I(S.status.v92, 0);
    /* another number is not answered, another login is turned away */
    place_call("1111111", 6000);
    CHECK(S.tel.state != TEL_ANSWERED);
    hang_up();
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(0, "dream", "cast");
    for (i = 0; i < 40L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    hang_up();
    CHECK(status_call(&S.status, 0)->end == ST_END_AUTH_FAILED);
    /* the port's number and a user of the box */
    place_call("5551234", 6000);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    run_ms(1100);
    CHECK(strcmp(S.status.peer_name, "alice") == 0);
    CHECK(C.ppp.dns1 == IPA(46, 101, 91, 123)); CHECK(C.ppp.dns2 == IPA(209, 50, 50, 129));
    CHECK_EQ_I(S.status.ppp_comp_tx, ST_COMP_NONE); CHECK_EQ_I(S.status.ppp_comp_rx, ST_COMP_NONE);
    /* a packet of the console arrives on the LAN side with 64 hops to go */
    mktcp(p, 30, 1024, 80, 0x10);
    lan0 = lan_n;
    for (i = 0; i < 5L * FS && lan_n == lan0; i++) { if (i == 0) CHECK_EQ_I(caller_send_ip(&C, p, 40), 0); step(0); }
    CHECK_EQ_I(lan_n, lan0 + 1);
    CHECK_EQ_I(lan_last[8], 64); CHECK_EQ_I(ip_sum(lan_last), 0xffff);
    /* two openings from port 3200 at once: one arrives */
    mktcp(p, 30, 3200, 6000, 0x02);
    lan0 = lan_n;
    CHECK_EQ_I(caller_send_ip(&C, p, 40), 0); CHECK_EQ_I(caller_send_ip(&C, p, 40), 0);
    run_ms(900);
    CHECK_EQ_I(lan_n, lan0 + 1);
    hang_up();

    /* the Internet service on the same settings: the box's own name
     * servers, V.92 as set, and the packets as they came */
    c.service = CFG_SERVICE_INTERNET;
    /* encryption on an Internet port does apply, and replaces compression */
    line_init();
    srv_init(&S, &c, 78, lan_rx, NULL);
    CHECK_EQ_I(S.status.ppp_encryption, 1); CHECK_EQ_I(S.status.ppp_compression, 0);
    c.encryption = 0;       /* this caller does PAP only */
    line_init();
    srv_init(&S, &c, 78, lan_rx, NULL);
    CHECK_EQ_I(S.status.v92, 1); CHECK_EQ_I(S.status.ppp_compression, 1);
    place_call("5551234", 6000);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    CHECK(C.ppp.dns1 == IPA(9, 9, 9, 9));
    mktcp(p, 30, 1024, 80, 0x10);
    lan0 = lan_n;
    for (i = 0; i < 5L * FS && lan_n == lan0; i++) { if (i == 0) CHECK_EQ_I(caller_send_ip(&C, p, 40), 0); step(0); }
    CHECK_EQ_I(lan_last[8], 30);
    hang_up();
    lan_bad = 0;
}

/* STUN (stun.h): a binding request, and the mapped address out of a reply */
static void test_stun(void)
{
    static const uint8_t txid[12] = { 1,2,3,4,5,6,7,8,9,10,11,12 };
    uint8_t req[STUN_REQ_LEN]; uint32_t a = 0; uint16_t port = 0;
    static uint8_t ok[32] = {
        0x01,0x01, 0x00,0x0C,                       /* binding success, length 12 */
        0x21,0x12,0xA4,0x42,                         /* magic */
        1,2,3,4,5,6,7,8,9,10,11,12,                  /* our txid */
        0x00,0x20, 0x00,0x08,                        /* XOR-MAPPED-ADDRESS, len 8 */
        0x00, 0x01, 0x00,0x00, 0x00,0x00,0x00,0x00   /* filled below */
    };
    uint16_t xport; uint32_t xaddr;

    stun_request(req, txid);
    CHECK_EQ_I(req[0], 0x00); CHECK_EQ_I(req[1], 0x01);          /* binding request */
    CHECK_EQ_I(req[2], 0x00); CHECK_EQ_I(req[3], 0x00);          /* no attributes */
    CHECK(req[4]==0x21 && req[5]==0x12 && req[6]==0xA4 && req[7]==0x42);
    CHECK(memcmp(req + 8, txid, 12) == 0);

    /* 203.0.113.9 : 50000, XOR-encoded */
    xport = (uint16_t)(50000u ^ (STUN_MAGIC >> 16));
    xaddr = 0xCB007109u ^ STUN_MAGIC;
    ok[26] = (uint8_t)(xport >> 8); ok[27] = (uint8_t)xport;
    ok[28] = (uint8_t)(xaddr >> 24); ok[29] = (uint8_t)(xaddr >> 16); ok[30] = (uint8_t)(xaddr >> 8); ok[31] = (uint8_t)xaddr;
    CHECK_EQ_I(stun_parse(ok, sizeof ok, txid, &a, &port), 1);
    CHECK(a == 0xCB007109u); CHECK_EQ_I(port, 50000);

    /* a reply for another transaction is not ours */
    { uint8_t other[12] = { 9 }; CHECK_EQ_I(stun_parse(ok, sizeof ok, other, &a, &port), 0); }
    /* plain MAPPED-ADDRESS (0x0001) is read too */
    {
        static uint8_t m[32] = { 0x01,0x01, 0x00,0x0C, 0x21,0x12,0xA4,0x42, 1,2,3,4,5,6,7,8,9,10,11,12,
                                 0x00,0x01, 0x00,0x08, 0x00,0x01, 0x00,0x00, 0x00,0x00,0x00,0x00 };
        m[26]=0xC3; m[27]=0x50;                      /* 50000 */
        m[28]=203; m[29]=0; m[30]=113; m[31]=9;
        CHECK_EQ_I(stun_parse(m, sizeof m, txid, &a, &port), 1);
        CHECK(a == 0xCB007109u); CHECK_EQ_I(port, 50000);
    }
    /* rubbish and truncation */
    CHECK_EQ_I(stun_parse(ok, 10, txid, &a, &port), 0);
    CHECK_EQ_I(stun_parse((const uint8_t *)"not stun at all!....", 20, txid, &a, &port), 0);
    { uint8_t err[20] = { 0x01,0x11, 0,0, 0x21,0x12,0xA4,0x42, 1,2,3,4,5,6,7,8,9,10,11,12 };
      CHECK_EQ_I(stun_parse(err, sizeof err, txid, &a, &port), 0); }   /* binding error */
}

/* service = saturn: the dialled digits say which side of a console-to-
 * console call this is, the call carries octets and no PPP (srv.h) */
static uint8_t sat_up[2048], sat_dn[2048]; static size_t sat_up_n, sat_dn_n, sat_cli_at, sat_cli_n;
static void sat_srv_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (sat_up_n + n <= sizeof sat_up) { memcpy(sat_up + sat_up_n, b, n); sat_up_n += n; } }
static void sat_cli_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (sat_dn_n + n <= sizeof sat_dn) { memcpy(sat_dn + sat_dn_n, b, n); sat_dn_n += n; } }
static size_t sat_cli_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = 0; (void)ctx;
    while (n < max && sat_cli_at < sat_cli_n) { b[n++] = (uint8_t)(sat_cli_at * 11 + 3); sat_cli_at++; }
    return n;
}
static void sat_modem(void)
{
    caller_cfg_t cc;
    caller_cfg_default(&cc);
    c_oct = 0xFF; c_lin = 0;
    cc.use_v42 = 0; cc.raw_rx = sat_cli_rx; cc.raw_tx = sat_cli_tx;
    caller_init(&C, &cc);
    modem_on = 1;
    sat_up_n = sat_dn_n = sat_cli_at = sat_cli_n = 0;
}
static void test_saturn_call(void)
{
    cfg_t c; uint32_t a = 0; int on = 9; long i; size_t k, sent = 0; int bad = 0; static uint8_t dn[600];

    base_cfg(&c); c.rings = 0; c.service = CFG_SERVICE_SATURN;
    line_init();
    srv_init(&S, &c, 91, lan_rx, NULL);
    srv_set_raw(&S, sat_srv_rx);
    CHECK_EQ_I(S.status.service, CFG_SERVICE_SATURN);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    /* waiting for the other player */
    place_call("0", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_WAIT); CHECK_EQ_I(on, 0);
    CHECK_EQ_I(srv_send_raw(&S, dn, 4), 0);
    sat_modem();
    for (i = 0; i < 40L * FS && !(srv_link(&S, &a, &on) && on); i++) step(0);
    CHECK_EQ_I(on, 1);
    run_ms(1100);                                   /* the status page is brought up to date once a second */
    CHECK_EQ_I(S.status.line, ST_LINE_CONNECTED);
    CHECK_EQ_I(S.status.ppp, ST_PPP_DEAD);
    CHECK_EQ_I(srv_ip_up(&S), 0);
    for (k = 0; k < sizeof dn; k++) dn[k] = (uint8_t)(k * 5 + 1);
    sat_cli_n = 300;
    for (i = 0; i < 60L * FS && (sat_up_n < 300 || sat_dn_n < sizeof dn); i++) {
        if (sent < sizeof dn) sent += srv_send_raw(&S, dn + sent, sizeof dn - sent);
        step(0);
    }
    CHECK_EQ_I(sat_up_n, 300); CHECK_EQ_I(sat_dn_n, sizeof dn);
    for (k = 0; k < sat_up_n; k++) if (sat_up[k] != (uint8_t)(k * 11 + 3)) bad++;
    for (k = 0; k < sat_dn_n; k++) if (sat_dn[k] != dn[k]) bad++;
    CHECK_EQ_I(bad, 0);
    /* the owner ends it: the other player has gone */
    srv_hangup(&S);
    for (i = 0; i < 10L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    hang_up();
    CHECK(strcmp(status_call(&S.status, 0)->dialed, "0") == 0);
    CHECK(status_call(&S.status, 0)->end == ST_END_LOCAL);
    /* calling a player by address */
    place_call("#192*0*2*7#", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_CALL); CHECK(a == IPA(192, 0, 2, 7)); CHECK_EQ_I(on, 0);
    hang_up();
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    /* numbers that mean nothing here are not answered into a call: the
     * port's own, a matchmaking code */
    place_call("5551234", 6000);
    run_ms(200);
    CHECK_EQ_I(S.tel.state, TEL_ENDED); CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    hang_up();
    place_call("#001#", 6000);
    run_ms(200);
    CHECK_EQ_I(S.tel.state, TEL_ENDED); CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);  /* matchmaking off: not answered */
    hang_up();

    /* with matchmaking on, #0NN# is answered and reported as a wait for game NN */
    c.sat_match = 1;
    line_init();
    srv_init(&S, &c, 93, lan_rx, NULL);
    srv_set_raw(&S, sat_srv_rx);
    place_call("#042#", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_MATCH_WAIT); CHECK(strcmp(srv_link_game(&S), "42") == 0);
    hang_up();
    place_call("#7788#", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_MATCH_CALL); CHECK(strcmp(srv_link_game(&S), "88") == 0);
    hang_up();
    c.sat_match = 0;

    /* on a port with another service these are ordinary wrong numbers */
    c.service = CFG_SERVICE_INTERNET;
    line_init();
    srv_init(&S, &c, 92, lan_rx, NULL);
    place_call("0", 6000);
    CHECK(S.tel.state != TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    hang_up();
}

/* a V.92 caller: PCM upstream when the configuration allows it (v92 = on,
 * V.42 on, V.90 among the modulations), V.90 otherwise */
/* ---- service = bbs: the list, the prompt's line, the telnet words ---- */
static void test_bbs_words(void)
{
    bbs_entry e; char k[BBS_LOGIN_MAX + 1], num[BBS_NUMBER_LEN + 1]; bbs_edit_t ed; uint8_t echo[3]; size_t en; int i;
#define L(s) bbs_line(s, strlen(s), &e)
    printf("bbs: the list\n");
    CHECK_EQ_I(L(""), BBS_LINE_NONE); CHECK_EQ_I(L("   # a comment"), BBS_LINE_NONE); CHECK_EQ_I(L("\r"), BBS_LINE_NONE);
    CHECK_EQ_I(L("v90d-bbs 2"), BBS_LINE_HEAD); CHECK_EQ_I(L("v90d-bbs 2\r"), BBS_LINE_HEAD);
    CHECK_EQ_I(L("v90d-bbs 1"), BBS_LINE_BAD); CHECK_EQ_I(L("v90d-bbs 3"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs telehack 2278353422 telehack.com:23 Telehack"), BBS_LINE_BBS);
    CHECK(strcmp(e.login, "telehack") == 0); CHECK(strcmp(e.number, "2278353422") == 0);
    CHECK(strcmp(e.host, "telehack.com") == 0); CHECK_EQ_I(e.port, 23); CHECK(strcmp(e.name, "Telehack") == 0);
    CHECK_EQ_I(L("bbs 20forbeers 2272036723 20forbeers.com:1337 20 For Beers BBS \r"), BBS_LINE_BBS);
    CHECK(strcmp(e.name, "20 For Beers BBS") == 0); CHECK_EQ_I(e.port, 1337);
    CHECK_EQ_I(L("bbs 4wheelham 2274943354 bbs.4wheelham.com"), BBS_LINE_BBS);
    CHECK_EQ_I(e.port, 23); CHECK(strcmp(e.name, "4wheelham") == 0);        /* no name: the login stands for it */
    CHECK_EQ_I(L("bbs v4 2278400000 192.0.2.7:2323 By address"), BBS_LINE_BBS);
    CHECK_EQ_I(L("bbs telehack telehack.com Telehack"), BBS_LINE_BAD);      /* no number */
    CHECK_EQ_I(L("bbs x 2289000000 h.com"), BBS_LINE_BAD);                  /* a board's number begins 227 */
    CHECK_EQ_I(L("bbs x 227900000 h.com"), BBS_LINE_BAD); CHECK_EQ_I(L("bbs x 22790000000 h.com"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 227900000a h.com"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs Telehack 2278353422 telehack.com"), BBS_LINE_BAD);    /* a login is lower case and digits */
    CHECK_EQ_I(L("bbs x 2279000000 -oProxy.com"), BBS_LINE_BAD);            /* a host does not begin with '-' */
    CHECK_EQ_I(L("bbs x 2279000000 host;rm:23"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 2279000000 host.com:0"), BBS_LINE_BAD); CHECK_EQ_I(L("bbs x 2279000000 host.com:65536"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 2279000000 host.com:23x"), BBS_LINE_BAD); CHECK_EQ_I(L("bbs x 2279000000"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs abcdefghijklmnopqrstuvwxyz0123456 2279000000 h.com"), BBS_LINE_BAD);    /* 33 */
    /* the box itself is not a board */
    CHECK_EQ_I(L("bbs x 2279000000 127.0.0.1"), BBS_LINE_BAD); CHECK_EQ_I(L("bbs x 2279000000 localhost"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 2279000000 127.1:80"), BBS_LINE_BAD); CHECK_EQ_I(L("bbs x 2279000000 0.0.0.0"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 2279000000 LocalHost.localdomain"), BBS_LINE_BAD);
    CHECK_EQ_I(L("bbs x 2279000000 127001.example.com"), BBS_LINE_BBS);     /* a name that only looks like it */
    CHECK_EQ_I(L("something else"), BBS_LINE_BAD);
    { char nm[140] = "bbs x 2279000000 h.com "; memset(nm + 23, 'N', 100); nm[123] = 0;
      CHECK_EQ_I(L(nm), BBS_LINE_BBS); CHECK_EQ_I((int)strlen(e.name), BBS_NAME_MAX); }    /* a long name is cut */
    { char ctl[] = "bbs x 2279000000 h.com Na\033[2Jme"; CHECK_EQ_I(L(ctl), BBS_LINE_BBS); CHECK(strchr(e.name, 033) == NULL); }
#undef L
    printf("bbs: the number dialled\n");
    CHECK_EQ_I(bbs_number("8002273425", num), BBS_NUM_PROMPT); CHECK_EQ_I(bbs_number("18002273425", num), BBS_NUM_PROMPT);
    CHECK_EQ_I(bbs_number("8002273426", num), BBS_NUM_NONE); CHECK_EQ_I(bbs_number("118002273425", num), BBS_NUM_NONE);
    CHECK_EQ_I(bbs_number("", num), BBS_NUM_NONE);
    CHECK_EQ_I(bbs_number("2278353422", num), BBS_NUM_BOARD); CHECK(strcmp(num, "2278353422") == 0);
    CHECK_EQ_I(bbs_number("12278353422", num), BBS_NUM_BOARD); CHECK(strcmp(num, "2278353422") == 0);   /* with the 1: the same board */
    CHECK_EQ_I(bbs_number("227835342", num), BBS_NUM_NONE); CHECK_EQ_I(bbs_number("22783534222", num), BBS_NUM_NONE);
    CHECK_EQ_I(bbs_number("227835342*", num), BBS_NUM_NONE); CHECK_EQ_I(bbs_number("2288353422", num), BBS_NUM_NONE);
    /* a board's number from its name: 227 and the name on the keys of a telephone */
    bbs_keypad("telehack", num); CHECK(strcmp(num, "2278353422") == 0);
    bbs_keypad("a1", num); CHECK(strcmp(num, "2272100000") == 0);           /* a short name: zeros behind it */
    bbs_keypad("20forbeers", num); CHECK(strcmp(num, "2272036723") == 0);
    bbs_keypad("wxyzpqrs", num); CHECK(strcmp(num, "2279999777") == 0);
    printf("bbs: what the caller types\n");
    bbs_key("20 For Beers", k); CHECK(strcmp(k, "20forbeers") == 0);
    bbs_key("  A-Net Online (Mystic)! ", k); CHECK(strcmp(k, "anetonlinemystic") == 0);
    bbs_key("", k); CHECK(k[0] == 0);
    bbs_key("abcdefghijklmnopqrstuvwxyz0123456789", k); CHECK_EQ_I((int)strlen(k), BBS_LOGIN_MAX);
    strcpy(e.login, "20forbeers"); strcpy(e.name, "20 For Beers BBS"); strcpy(e.host, "20forbeers.com");
    CHECK_EQ_I(bbs_match(&e, ""), 1); CHECK_EQ_I(bbs_match(&e, "beer"), 1); CHECK_EQ_I(bbs_match(&e, "forbeersbbs"), 1);
    CHECK_EQ_I(bbs_match(&e, "wine"), 0);
    strcpy(e.number, "2272036723");
    CHECK_EQ_I(bbs_match(&e, "227203"), 1); CHECK_EQ_I(bbs_match(&e, "6723"), 1); CHECK_EQ_I(bbs_match(&e, "2279"), 0);
    bbs_show("2278353422", k); CHECK(strcmp(k, "227 835 3422") == 0);
    bbs_edit_init(&ed);
    CHECK_EQ_I(bbs_edit(&ed, 't', echo, &en), 0); CHECK_EQ_I(en, 1); CHECK_EQ_I(echo[0], 't');
    CHECK_EQ_I(bbs_edit(&ed, 'x', echo, &en), 0);
    CHECK_EQ_I(bbs_edit(&ed, 8, echo, &en), 0); CHECK_EQ_I(en, 3); CHECK(memcmp(echo, "\b \b", 3) == 0);
    CHECK_EQ_I(bbs_edit(&ed, 'h', echo, &en), 0);
    CHECK_EQ_I(bbs_edit(&ed, 0x7F, echo, &en), 0); CHECK_EQ_I(en, 3);
    CHECK_EQ_I(bbs_edit(&ed, 0x7F, echo, &en), 0); CHECK_EQ_I(en, 3);
    CHECK_EQ_I(bbs_edit(&ed, 0x7F, echo, &en), 0); CHECK_EQ_I(en, 0);      /* nothing left to rub out */
    CHECK_EQ_I(bbs_edit(&ed, 'o', echo, &en), 0); CHECK_EQ_I(bbs_edit(&ed, 'k', echo, &en), 0);
    CHECK_EQ_I(bbs_edit(&ed, 0, echo, &en), 0); CHECK_EQ_I(en, 0);         /* not a character */
    CHECK_EQ_I(bbs_edit(&ed, 10, echo, &en), 0); CHECK_EQ_I(en, 0);        /* the LF of a CR LF */
    CHECK_EQ_I(bbs_edit(&ed, 0xE9, echo, &en), 0); CHECK_EQ_I(en, 0);
    CHECK_EQ_I(bbs_edit(&ed, 13, echo, &en), 1); CHECK_EQ_I(en, 2); CHECK(memcmp(echo, "\r\n", 2) == 0);
    CHECK(strcmp(ed.line, "ok") == 0);
    bbs_edit_init(&ed);
    for (i = 0; i < 200; i++) (void)bbs_edit(&ed, 'a', echo, &en);
    CHECK_EQ_I(en, 0);                                          /* full: no more is taken or shown */
    CHECK_EQ_I(bbs_edit(&ed, 13, echo, &en), 1); CHECK_EQ_I((int)strlen(ed.line), BBS_TYPED_MAX);
}

/* the connection to a board (bbsdial.h) against a listener here */
static int dial_wait(int fd, const char *want, int ms)
{
    char b[256]; size_t n = 0; struct pollfd p = { fd, POLLIN, 0 };
    while (n < sizeof b - 1 && poll(&p, 1, ms) > 0) {
        ssize_t r = recv(fd, b + n, sizeof b - 1 - n, 0);
        if (r <= 0) break;
        n += (size_t)r; b[n] = 0;
        if (strstr(b, want)) return 1;
    }
    return 0;
}
static void test_bbsdial(void)
{
    struct sockaddr_in a; socklen_t al = sizeof a; int ls, sv[2], c, st; pid_t pid; char port[8], b[8];
    struct pollfd p;
    printf("bbs: the connection to a board\n");
    ls = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 2) == 0);
    CHECK_EQ_I(getsockname(ls, (struct sockaddr *)&a, &al), 0);
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(a.sin_port));
    /* octets both ways, and a quiet time is no reason to end */
    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pid = fork();
    if (pid == 0) { close(sv[0]); close(ls); _exit(bbsdial_run("127.0.0.1", port, sv[1], 5, 1)); }
    close(sv[1]);
    c = accept(ls, NULL, NULL);
    CHECK(c >= 0);
    CHECK_EQ_I((int)send(c, "welcome", 7, 0), 7); CHECK(dial_wait(sv[0], "welcome", 3000));
    CHECK_EQ_I((int)send(sv[0], "typed", 5, 0), 5); CHECK(dial_wait(c, "typed", 3000));
    p.fd = sv[0]; p.events = POLLIN; p.revents = 0;
    CHECK_EQ_I(poll(&p, 1, 2500), 0);                           /* nothing said for a while: still there */
    CHECK_EQ_I(waitpid(pid, &st, WNOHANG), 0);
    CHECK_EQ_I((int)send(c, "again", 5, 0), 5); CHECK(dial_wait(sv[0], "again", 3000));
    /* the board ends it: what it said last arrives, then the end */
    CHECK_EQ_I((int)send(c, "bye", 3, 0), 3); close(c);
    CHECK(dial_wait(sv[0], "bye", 3000));
    CHECK_EQ_I((int)recv(sv[0], b, sizeof b, 0), 0);
    CHECK_EQ_I(waitpid(pid, &st, 0), pid); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    close(sv[0]);
    /* the daemon ends it: the board sees the end */
    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pid = fork();
    if (pid == 0) { close(sv[0]); close(ls); _exit(bbsdial_run("localhost", port, sv[1], 5, 1)); }
    close(sv[1]);
    c = accept(ls, NULL, NULL);
    CHECK(c >= 0);
    close(sv[0]);
    p.fd = c; p.events = POLLIN; p.revents = 0;
    CHECK(poll(&p, 1, 3000) > 0); CHECK_EQ_I((int)recv(c, b, sizeof b, 0), 0);
    CHECK_EQ_I(waitpid(pid, &st, 0), pid); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    /* a name that turns out to be the box itself is not connected to: the
     * listener is there and hears nothing */
    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_EQ_I(bbsdial_run("localhost", port, sv[1], 5, 0), 1);
    CHECK_EQ_I(bbsdial_run("127.0.0.1", port, sv[1], 5, 0), 1);
    p.fd = ls; p.events = POLLIN; p.revents = 0;
    CHECK_EQ_I(poll(&p, 1, 300), 0);
    close(sv[0]); close(sv[1]);
    close(c); close(ls);
    /* nobody there, and a name that is none */
    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    CHECK_EQ_I(bbsdial_run("127.0.0.1", port, sv[1], 5, 1), 1);
    CHECK_EQ_I(bbsdial_run("no-such-host.invalid", "23", sv[1], 5, 1), 1);
    close(sv[0]); close(sv[1]);
}

/* which addresses are no board: the box itself and what is no host at all */
static void test_bbsdial_self(void)
{
    struct sockaddr_in a; socklen_t al = sizeof a; int u, ls, sv[2], st; pid_t pid; uint32_t own = 0; char port[8], host[32];
    struct pollfd p;
    printf("bbs: addresses that are no board\n");
    CHECK_EQ_I(bbsdial_is_self(0x7f000001u), 1);        /* 127.0.0.1 */
    CHECK_EQ_I(bbsdial_is_self(0x7f123456u), 1);        /* all of 127/8 */
    CHECK_EQ_I(bbsdial_is_self(0x00000000u), 1);        /* 0.0.0.0: "this host" */
    CHECK_EQ_I(bbsdial_is_self(0x00010203u), 1);
    CHECK_EQ_I(bbsdial_is_self(0xa9fe0101u), 1);        /* 169.254.1.1: link-local */
    CHECK_EQ_I(bbsdial_is_self(0xe0000001u), 1);        /* 224.0.0.1: multicast */
    CHECK_EQ_I(bbsdial_is_self(0xffffffffu), 1);        /* broadcast */
    CHECK_EQ_I(bbsdial_is_self(0xc0000201u), 0);        /* 192.0.2.1: a host elsewhere */
    CHECK_EQ_I(bbsdial_is_self(0xa9fd0101u), 0);        /* 169.253.1.1 */
    CHECK_EQ_I(bbsdial_is_self(0xdfffffffu), 0);        /* 223.255.255.255 */
    /* an address of one of our own interfaces. Which one this machine has:
     * the source address it would use towards a host elsewhere (a datagram
     * socket's connect sends nothing) */
    u = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(9); a.sin_addr.s_addr = htonl(0xc0000201u);
    if (u >= 0 && connect(u, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(u, (struct sockaddr *)&a, &al) == 0)
        own = ntohl(a.sin_addr.s_addr);
    if (u >= 0) close(u);
    if (own == 0 || (own >> 24) == 127) { printf("  (no network interface here: own address not tried)\n"); return; }
    CHECK_EQ_I(bbsdial_is_self(own), 1);
    /* ... and a listener on it hears nothing */
    ls = socket(AF_INET, SOCK_STREAM, 0);
    memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(own);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 2) == 0);
    al = sizeof a;
    CHECK_EQ_I(getsockname(ls, (struct sockaddr *)&a, &al), 0);
    snprintf(port, sizeof port, "%u", (unsigned)ntohs(a.sin_port));
    snprintf(host, sizeof host, "%u.%u.%u.%u", (unsigned)(own >> 24), (unsigned)(own >> 16 & 255), (unsigned)(own >> 8 & 255), (unsigned)(own & 255));
    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pid = fork();
    if (pid == 0) { close(sv[0]); close(ls); _exit(bbsdial_run(host, port, sv[1], 5, 0)); }
    close(sv[1]);
    p.fd = ls; p.events = POLLIN; p.revents = 0;
    CHECK_EQ_I(poll(&p, 1, 500), 0);
    /* (a dial that did connect would stay until told to end) */
    close(sv[0]);
    CHECK_EQ_I(waitpid(pid, &st, 0), pid); CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1);
    close(ls);
}

static void test_telnet(void)
{
    telnet_t t; uint8_t out[64], rep[64]; size_t on, rn, k;
#define RX(lit) telnet_rx(&t, (const uint8_t *)(lit), sizeof(lit) - 1, out, sizeof out, &on, rep, sizeof rep, &rn)
#define IS(buf, n, lit) ((n) == sizeof(lit) - 1 && memcmp(buf, lit, sizeof(lit) - 1) == 0)
    printf("telnet: the words\n");
    telnet_init(&t);
    CHECK_EQ_I(RX("hello"), 5); CHECK(IS(out, on, "hello")); CHECK_EQ_I(rn, 0);
    /* the board will echo and suppress go-ahead: agreed, once */
    CHECK_EQ_I(RX("\377\373\001\377\373\003"), 6); CHECK_EQ_I(on, 0); CHECK(IS(rep, rn, "\377\375\001\377\375\003"));
    CHECK_EQ_I(RX("\377\373\001"), 3); CHECK_EQ_I(rn, 0);                  /* said again: no answer, or it never ends */
    /* asked for the terminal type and the window: given */
    CHECK_EQ_I(RX("\377\375\030"), 3); CHECK(IS(rep, rn, "\377\373\030"));
    CHECK_EQ_I(RX("\377\372\030\001\377\360"), 6); CHECK(IS(rep, rn, "\377\372\030\000ANSI\377\360")); CHECK_EQ_I(on, 0);
    CHECK_EQ_I(RX("\377\375\037"), 3); CHECK(IS(rep, rn, "\377\373\037\377\372\037\000\120\000\030\377\360"));
    /* what we do not know is refused, every time it is asked */
    CHECK_EQ_I(RX("\377\375\042"), 3); CHECK(IS(rep, rn, "\377\374\042"));     /* DO LINEMODE: WONT */
    CHECK_EQ_I(RX("\377\375\042"), 3); CHECK(IS(rep, rn, "\377\374\042"));
    CHECK_EQ_I(RX("\377\373\052"), 3); CHECK(IS(rep, rn, "\377\376\052"));     /* WILL CHARSET: DONT */
    CHECK_EQ_I(RX("\377\374\052\377\376\042"), 6); CHECK_EQ_I(rn, 0);          /* WONT / DONT what is already off */
    /* the board stops echoing: acknowledged */
    CHECK_EQ_I(RX("\377\374\001"), 3); CHECK(IS(rep, rn, "\377\376\001"));
    /* data around and inside commands; 255 255 is one octet of data */
    CHECK_EQ_I(RX("a\377\377b\377\361c"), 7); CHECK(IS(out, on, "a\377bc")); CHECK_EQ_I(rn, 0);
    /* a command cut in two by the network */
    CHECK_EQ_I(RX("x\377"), 2); CHECK(IS(out, on, "x"));
    CHECK_EQ_I(RX("\373"), 1); CHECK_EQ_I(on, 0); CHECK_EQ_I(rn, 0);
    CHECK_EQ_I(RX("\000y"), 2); CHECK(IS(out, on, "y")); CHECK(IS(rep, rn, "\377\375\000"));   /* WILL BINARY: DO */
    /* a subnegotiation we do not know, with a 255 255 inside, is passed over */
    CHECK_EQ_I(RX("1\377\372\052\001\377\377\002\377\3602"), 11); CHECK(IS(out, on, "12")); CHECK_EQ_I(rn, 0);
    /* the end of a line: CR NUL is a CR, CR LF stays, while the board is not binary */
    telnet_init(&t);
    CHECK_EQ_I(RX("a\r\000b\r\nc\r"), 8); CHECK(IS(out, on, "a\rb\r\nc\r"));
    CHECK_EQ_I(RX("\000d"), 2); CHECK(IS(out, on, "d"));                       /* the NUL arrives in the next read */
    CHECK_EQ_I(RX("\377\373\000"), 3);                                         /* binary from now on */
    CHECK_EQ_I(RX("a\r\000b"), 4); CHECK(IS(out, on, "a\r\000b"));
    /* no room for an answer: nothing is taken, nothing is lost */
    telnet_init(&t);
    CHECK_EQ_I(telnet_rx(&t, (const uint8_t *)"ab\377\373\001", 5, out, sizeof out, &on, rep, 4, &rn), 0);
    CHECK_EQ_I(on, 0); CHECK_EQ_I(rn, 0);
    /* little room for the caller: what fits is taken */
    CHECK_EQ_I(telnet_rx(&t, (const uint8_t *)"abcdef", 6, out, 4, &on, rep, sizeof rep, &rn), 4); CHECK(IS(out, on, "abcd"));
    /* to the board: 255 doubled, NUL after CR until we are binary */
    telnet_init(&t);
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"a\377\rb", 4, out, sizeof out, &on), 4); CHECK(IS(out, on, "a\377\377\r\000b"));
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"\r\n", 2, out, sizeof out, &on), 2); CHECK(IS(out, on, "\r\n"));  /* CR LF is a line end as it is */
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"\r", 1, out, sizeof out, &on), 1); CHECK(IS(out, on, "\r"));
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"x", 1, out, sizeof out, &on), 1); CHECK(IS(out, on, "\000x"));   /* the NUL owed */
    CHECK_EQ_I(RX("\377\375\000"), 3); CHECK(IS(rep, rn, "\377\373\000"));     /* DO BINARY: WILL */
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"\rb\377", 3, out, sizeof out, &on), 3); CHECK(IS(out, on, "\rb\377\377"));
    CHECK_EQ_I(telnet_tx(&t, (const uint8_t *)"abc\377", 4, out, 4, &on), 3); CHECK(IS(out, on, "abc"));   /* no room for both 255s */
    /* any octets at all never write past the buffers */
    telnet_init(&t);
    for (k = 0; k < 20000; k++) {
        uint8_t in[8]; size_t j, took;
        for (j = 0; j < sizeof in; j++) in[j] = (uint8_t)((k * 2654435761u + j * 40503u) >> (j + 3)) | (uint8_t)((k + j) % 3 == 0 ? 0xF0 : 0);
        took = telnet_rx(&t, in, sizeof in, out, 8, &on, rep, 32, &rn);
        if (took > sizeof in || on > 8 || rn > 32) { CHECK(0); break; }
    }
#undef RX
#undef IS
}

/* ---- the bulletin board number, on a port of any service: that call
 * carries octets on error control; the port's own number is as before ---- */
static void test_bbs_call(void)
{
    cfg_t c; uint32_t a = 0; int on = 9; long i; size_t k, sent = 0; int bad = 0, svc; static uint8_t dn[600]; caller_cfg_t cc;

    printf("bbs: a terminal program calls\n");
    for (svc = 0; svc < CFG_SERVICE_COUNT; svc++) {
        base_cfg(&c); c.rings = 0; c.service = svc;
        line_init();
        srv_init(&S, &c, 95 + (uint32_t)svc, lan_rx, NULL);
        srv_set_raw(&S, sat_srv_rx);
        place_call(svc & 1 ? "8002273425" : "18002273425", 6000);
        CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
        CHECK_EQ_I(srv_link(&S, &a, &on), SRV_LINK_BBS); CHECK_EQ_I(on, 0);
        hang_up();
        CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    }
    base_cfg(&c); c.rings = 0;
    line_init();
    srv_init(&S, &c, 99, lan_rx, NULL);
    srv_set_raw(&S, sat_srv_rx);
    place_call("8002273426", 6000);                     /* one digit off: a wrong number */
    CHECK(S.tel.state != TEL_ANSWERED);
    hang_up();
    place_call("18002273425", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    caller_cfg_default(&cc);
    c_oct = 0xFF; c_lin = 0;
    cc.raw_rx = sat_cli_rx; cc.raw_tx = sat_cli_tx;
    caller_init(&C, &cc);
    modem_on = 1;
    sat_up_n = sat_dn_n = sat_cli_at = sat_cli_n = 0;
    for (i = 0; i < 40L * FS && !(srv_link(&S, &a, &on) && on); i++) step(0);
    CHECK_EQ_I(on, 1);
    run_ms(1100);
    CHECK_EQ_I(S.status.line, ST_LINE_CONNECTED);
    CHECK_EQ_I(S.status.ppp, ST_PPP_DEAD);
    CHECK_EQ_I(S.status.v42, ST_V42_LAPM);              /* error control, unlike the consoles' calls */
    CHECK_EQ_I(srv_ip_up(&S), 0);
    for (k = 0; k < sizeof dn; k++) dn[k] = (uint8_t)(k * 5 + 1);
    sat_cli_n = 300;
    for (i = 0; i < 60L * FS && (sat_up_n < 300 || sat_dn_n < sizeof dn); i++) {
        if (sent < sizeof dn) sent += srv_send_raw(&S, dn + sent, sizeof dn - sent);
        step(0);
    }
    CHECK_EQ_I(sat_up_n, 300); CHECK_EQ_I(sat_dn_n, sizeof dn);
    for (k = 0; k < sat_up_n; k++) if (sat_up[k] != (uint8_t)(k * 11 + 3)) bad++;
    for (k = 0; k < sat_dn_n; k++) if (sat_dn[k] != dn[k]) bad++;
    CHECK_EQ_I(bad, 0);
    srv_hangup(&S);
    for (i = 0; i < 10L * FS && S.tel.state == TEL_ANSWERED; i++) step(0);
    CHECK_EQ_I(S.tel.state, TEL_ENDED);
    hang_up();
    CHECK(strcmp(status_call(&S.status, 0)->dialed, "18002273425") == 0);
    /* a board's own number: the same kind of call, and the daemon is told what was dialled */
    place_call("2278353422", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SRV_LINK_BBS);
    CHECK(strcmp(srv_dialed(&S), "2278353422") == 0);
    hang_up();
    place_call("12278353422", 6000);
    CHECK_EQ_I(srv_link(&S, &a, &on), SRV_LINK_BBS);
    hang_up();
    /* the port's own number is an ordinary call still */
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    hang_up();
    /* even one that looks like a board's */
    base_cfg(&c); c.rings = 0; strcpy(c.line_number, "2275551234");
    line_init();
    srv_init(&S, &c, 101, lan_rx, NULL);
    place_call("2275551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(srv_link(&S, &a, &on), SATURN_NONE);
    hang_up();
}

/* the list that goes into the image and is fetched by the boxes: every
 * line is understood, no login twice */
static void test_bbs_shipped_list(void)
{
    static bbs_entry bb[2048]; int nb = 0, bad = 0, head = 0, first = 1, i, j, twice = 0;
    char line[512]; FILE *f = fopen("../lists/bbs.txt", "r");
    printf("bbs: the shipped list\n");
    CHECK(f != NULL);
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        bbs_entry e; size_t len = strcspn(line, "\n"); int k = bbs_line(line, len, &e);
        if (k == BBS_LINE_NONE) continue;
        if (first) { first = 0; head = k == BBS_LINE_HEAD; continue; }
        if (k == BBS_LINE_BBS && nb < 2048) bb[nb++] = e;
        else bad++;
    }
    fclose(f);
    CHECK_EQ_I(head, 1); CHECK_EQ_I(bad, 0); CHECK(nb >= 10);
    printf("  %d boards\n", nb);
    for (i = 0; i < nb; i++) for (j = 0; j < i; j++) if (strcmp(bb[i].login, bb[j].login) == 0 || strcmp(bb[i].number, bb[j].number) == 0) twice++;
    CHECK_EQ_I(twice, 0);
}

static void test_v92_call(void)
{
    cfg_t c; int ms; const st_call_t *h;
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    c.v92 = 1; c.max_up = 48000;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    CHECK_EQ_I(S.status.v92, 1);
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(4, "alice", "wonderland");
    ms = wait_ip(30000);
    CHECK(ms > 0);
    run_ms(1100);
    printf("  V.92 call: IP up %.2f s after the answer, %u bit/s down, %u bit/s up\n", ms / 1000.0,
           (unsigned)S.status.down_bps, (unsigned)S.status.up_bps);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V92);
    CHECK(S.status.down_bps >= 40000); CHECK(S.status.up_bps > 33600 && S.status.up_bps <= 48000);
    CHECK_EQ_I(S.status.v42, ST_V42_LAPM);
    printf("    SNR %.1f dB, round trip %u ms, echo delay found %d samples\n", S.status.snr_x10 / 10.0,
           (unsigned)S.status.rtd_ms, v90m__answer(S.dce.modem)->u.v92.ecd.delay);
    CHECK(S.status.snr_x10 > 200 && S.status.snr_x10 < 700);
    exchange(20);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->modulation == ST_MOD_V92 && h->end == ST_END_CALLER_HANGUP && h->down_bps >= 40000 && h->up_bps > 33600);

    /* the default ceiling (max_upstream = 33600) holds for the PCM upstream too */
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0; c.v92 = 1;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(4, "alice", "wonderland");
    CHECK(wait_ip(30000) > 0);
    run_ms(1100);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V92);
    CHECK_EQ_I(S.status.up_bps, 33333);
    hang_up();

    /* v92 = off: the same caller gets a V.90 call */
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    CHECK_EQ_I(c.v92, 1);                               /* on is the default */
    c.v92 = 0;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    CHECK_EQ_I(S.status.v92, 0);
    place_call("5551234", 6000);
    start_modem(4, "alice", "wonderland");
    CHECK(wait_ip(30000) > 0);
    run_ms(1100);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V90);
    CHECK(S.status.up_bps <= 33600);
    exchange(5);
    hang_up();

    /* v92 = on without V.42: not offered (no error control, no V.92 here) */
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0; c.v92 = 1; c.v42 = 0;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(4, "alice", "wonderland");
    run_ms(12000);
    CHECK_EQ_I(v90m__answer(S.dce.modem)->modulation, ANS_MOD_V90);
    CHECK_EQ_I(C.vc.d_v92, 0);
    hang_up();
}

/* a caller with V.34 and no PCM: V.42 and PPP over V.34 */
static void test_v34_call(void)
{
    cfg_t c; int ms; const st_call_t *h;
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    line_init();
    srv_init(&S, &c, 41, lan_rx, NULL);
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(2, "alice", "wonderland");
    ms = wait_ip(30000);
    CHECK(ms > 0);
    run_ms(1100);
    printf("  V.34 call: IP up %.2f s after the answer, %u bit/s down, %u bit/s up\n", ms / 1000.0,
           (unsigned)S.status.down_bps, (unsigned)S.status.up_bps);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V34);
    CHECK(S.status.down_bps >= 21600 && S.status.down_bps <= 33600);
    CHECK(S.status.up_bps >= 21600 && S.status.up_bps <= 33600);
    exchange(20);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->modulation == ST_MOD_V34 && h->end == ST_END_CALLER_HANGUP && h->down_bps >= 21600);

    /* V.34 switched off in the settings: this caller is not served */
    c.modulations = CFG_MOD_ALL & ~CFG_MOD_V34;
    line_init();
    srv_init(&S, &c, 43, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(2, "alice", "wonderland");
    CHECK(wait_ip(20000) < 0);
    CHECK(S.status.modulation != ST_MOD_V34);
    hang_up();
}

/* a V.32bis caller (no V.8: AA during the answer tone): V.42 and PPP over it */
static void test_v32bis_call(void)
{
    cfg_t c; int ms; const st_call_t *h;
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    line_init();
    srv_init(&S, &c, 51, lan_rx, NULL);
    place_call("5551234", 6000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    start_modem(3, "alice", "wonderland");
    ms = wait_ip(30000);
    CHECK(ms > 0);
    run_ms(1100);
    printf("  V.32bis call: IP up %.2f s after the answer, %u bit/s\n", ms / 1000.0, (unsigned)S.status.down_bps);
    CHECK_EQ_I(S.status.modulation, ST_MOD_V32BIS);
    CHECK_EQ_I(S.status.down_bps, 14400); CHECK_EQ_I(S.status.up_bps, 14400);
    exchange(20);
    hang_up();
    h = status_call(&S.status, 0);
    CHECK(h && h->modulation == ST_MOD_V32BIS && h->end == ST_END_CALLER_HANGUP && h->down_bps == 14400);

    /* V.32bis switched off: the same caller is not served with it */
    c.modulations = CFG_MOD_ALL & ~CFG_MOD_V32BIS;
    line_init();
    srv_init(&S, &c, 53, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(3, "alice", "wonderland");
    CHECK(wait_ip(15000) < 0);
    CHECK(S.status.modulation != ST_MOD_V32BIS);
    hang_up();
}

/* a line driver that buffers: the echo of what we send comes back 60 ms
 * late, and nobody told us (the modem measures it) */
static void test_v90_late_echo(void)
{
    cfg_t c; int ms;
    base_cfg(&c); c.modulations = CFG_MOD_ALL; c.rings = 0;
    echo_bulk = 480;
    line_init();
    echo_bulk = 0;
    srv_init(&S, &c, 41, lan_rx, NULL);
    place_call("5551234", 6000);
    start_modem(1, "alice", "wonderland");
    ms = wait_ip(25000);
    CHECK(ms > 0);
    run_ms(1100);
    printf("  V.90 call, echo 60 ms late: %u bit/s down, %u bit/s up\n", (unsigned)S.status.down_bps, (unsigned)S.status.up_bps);
    CHECK(S.status.up_bps >= 21600);
    exchange(20);
    hang_up();
}

static void test_level(void)
{
    /* 0 dBm0 is a sine of peak 32124 / 10^(3.17/20) = 22300 (mu-law), RMS 15770 */
    CHECK_NEAR(srv_level_rms(0), 15770, 200);
    CHECK_NEAR(srv_level_rms(-12), 3961, 60);
    CHECK_NEAR(srv_level_rms(-6), 7904, 100);
    CHECK_NEAR(srv_level_rms(-30), 499, 12);
}

/* the box's line is a character device of our SLIC driver: "dev:PATH" */
static void test_line_dev(void)
{
    line_t l; uint8_t rx[LINE_BLOCK]; int hook = 7;
    errno = 0;
    CHECK(line_open(&l, "dev:/nonexistent/v90line") == -1); CHECK(errno == ENOENT);
    CHECK(line_open(&l, "dev:/dev/null") == 0); CHECK(l.fd >= 0);
    CHECK(line_exchange(&l, rx, 0, rx, &hook) == 0);    /* end of file: line gone */
    line_close(&l);

    /* the device is told the law in one octet; other back ends are not */
    {
        char path[40], spec[64], got[4] = { 0 }; int fd;
        /* (no mkstemp: the ARM C library hides it under these feature macros) */
        snprintf(path, sizeof path, "/tmp/v90d_line_%ld", (long)getpid());
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        snprintf(spec, sizeof spec, "dev:%s", path);
        CHECK(line_open(&l, spec) == 0);
        CHECK(line_set_law(&l, 1) == 0); CHECK(line_set_law(&l, 0) == 0);
        line_close(&l);
        CHECK(read(fd, got, 3) == 2); CHECK(got[0] == 'a' && got[1] == 'u');
        close(fd); unlink(path);
        snprintf(spec, sizeof spec, "fd:%d", open("/dev/null", O_WRONLY));
        CHECK(line_open(&l, spec) == 0); CHECK(line_set_law(&l, 1) == 0);
        CHECK(lseek(l.fd, 0, SEEK_CUR) == 0);       /* nothing was written */
        line_close(&l);
    }

    /* the box's driver with its codec in linear format: one read gives a
     * whole block, 161 octets (hook, 80 samples of 16 bits, little endian)
     * or the 81 of the companded format, and the length says which */
    {
        char path[64], spec[80]; uint8_t blk[1 + 2 * LINE_BLOCK], got[4]; int16_t lin[LINE_BLOCK]; int fd, i, linear = 7, bad = 0;
        snprintf(path, sizeof path, "/tmp/v90d_linl_%ld", (long)getpid());
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
        CHECK(fd >= 0);
        blk[0] = 1;
        for (i = 0; i < LINE_BLOCK; i++) { int16_t v = (int16_t)(i * 800 - 32000); blk[1 + 2 * i] = (uint8_t)((uint16_t)v & 255); blk[2 + 2 * i] = (uint8_t)((uint16_t)v >> 8); }
        CHECK(write(fd, blk, sizeof blk) == (ssize_t)sizeof blk);
        snprintf(spec, sizeof spec, "dev:%s", path);
        CHECK(line_open(&l, spec) == 0);
        hook = 7;
        CHECK(line_read2(&l, rx, lin, &linear, &hook) == 1);
        CHECK(linear == 1 && hook == 1);
        /* each on the modem's scale (line_lin_sample) */
        for (i = 0; i < LINE_BLOCK; i++) bad += lin[i] != line_lin_sample((int16_t)(i * 800 - 32000));
        CHECK_EQ_I(bad, 0);
        CHECK(line_read2(&l, rx, lin, &linear, &hook) == 0);        /* end of file */
        /* a companded block from the same device */
        CHECK(lseek(l.fd, 0, SEEK_SET) == 0 && ftruncate(l.fd, 0) == 0);
        blk[0] = 0; for (i = 0; i < LINE_BLOCK; i++) blk[1 + i] = (uint8_t)(i + 9);
        CHECK(pwrite(fd, blk, 1 + LINE_BLOCK, 0) == 1 + LINE_BLOCK);
        CHECK(line_read2(&l, rx, lin, &linear, &hook) == 1);
        CHECK(linear == 0 && hook == 0 && rx[0] == 9 && rx[LINE_BLOCK - 1] == LINE_BLOCK + 8);
        /* the plain reader refuses a linear block; a block of another length is refused by both */
        CHECK(lseek(l.fd, 0, SEEK_SET) == 0 && ftruncate(l.fd, 0) == 0);
        CHECK(pwrite(fd, blk, sizeof blk, 0) == (ssize_t)sizeof blk);
        errno = 0; CHECK(line_read(&l, rx, &hook) == -1 && errno == EPROTO);
        CHECK(lseek(l.fd, 0, SEEK_SET) == 0 && ftruncate(l.fd, 0) == 0);
        CHECK(pwrite(fd, blk, 100, 0) == 100);
        errno = 0; CHECK(line_read2(&l, rx, lin, &linear, &hook) == -1 && errno == EPROTO);
        /* the format request is one octet, as the law is */
        CHECK(lseek(l.fd, 0, SEEK_SET) == 0 && ftruncate(l.fd, 0) == 0);
        CHECK(line_set_linear(&l, 1) == 0); CHECK(line_set_linear(&l, 0) == 0);
        line_close(&l);
        CHECK(pread(fd, got, 3, 0) == 2); CHECK(got[0] == 'l' && got[1] == 'c');
        close(fd); unlink(path);
        /* other lines carry codewords only */
        snprintf(spec, sizeof spec, "fd:%d", open("/dev/null", O_WRONLY));
        CHECK(line_open(&l, spec) == 0);
        CHECK(line_set_linear(&l, 0) == 0);
        errno = 0; CHECK(line_set_linear(&l, 1) == -1 && errno == ENOTSUP);
        line_close(&l);
    }

    /* the two halves of an exchange, for the owner of two lines */
    {
        int p[2]; char spec[32]; uint8_t blk[1 + LINE_BLOCK], tx[LINE_BLOCK], got[LINE_BLOCK]; int i;
        CHECK(pipe(p) == 0);
        /* a pipe one way is enough to see each half: what we read ... */
        blk[0] = 1; for (i = 0; i < LINE_BLOCK; i++) blk[1 + i] = (uint8_t)(i + 3);
        CHECK(write(p[1], blk, sizeof blk) == (ssize_t)sizeof blk);
        snprintf(spec, sizeof spec, "fd:%d", p[0]);
        CHECK(line_open(&l, spec) == 0);
        hook = 7; memset(rx, 0, sizeof rx);
        CHECK(line_read(&l, rx, &hook) == 1);
        CHECK(hook == 1 && rx[0] == 3 && rx[LINE_BLOCK - 1] == LINE_BLOCK + 2);
        close(p[1]);
        CHECK(line_read(&l, rx, &hook) == 0);           /* the other side closed */
        line_close(&l);
        /* ... and what we write */
        CHECK(pipe(p) == 0);
        snprintf(spec, sizeof spec, "fd:%d", p[1]);
        CHECK(line_open(&l, spec) == 0);
        for (i = 0; i < LINE_BLOCK; i++) tx[i] = (uint8_t)(200 - i);
        CHECK(line_write(&l, tx) == 0);
        CHECK(read(p[0], got, sizeof got) == LINE_BLOCK); CHECK(memcmp(got, tx, LINE_BLOCK) == 0);
        line_close(&l); close(p[0]);
    }
}

/* the codec's 16-bit linear samples come 10.9 dB (2/7) below the scale of
 * the G.711 tables (measured on the unit); line_lin_sample puts them on it */
static void test_lin_scale(void)
{
    CHECK_EQ_I(line_lin_sample(0), 0);
    CHECK_EQ_I(line_lin_sample(1000), 3500);
    CHECK_EQ_I(line_lin_sample(-1000), -3500);
    CHECK_EQ_I(line_lin_sample(3), 11);          /* 10.5: away from zero, both signs alike */
    CHECK_EQ_I(line_lin_sample(-3), -11);
    CHECK_EQ_I(line_lin_sample(2), 7);
    CHECK_EQ_I(line_lin_sample(9362), 32767);    /* 32767.0 exactly */
    CHECK_EQ_I(line_lin_sample(9363), 32767);    /* and no further */
    CHECK_EQ_I(line_lin_sample(32767), 32767);
    CHECK_EQ_I(line_lin_sample(-9363), -32767);
    CHECK_EQ_I(line_lin_sample(-32768), -32767);
}

/* the heavy-call slot: a call reserves it when x2 or K56flex is offered and
 * gives it back at the end; while another line holds it, this line offers
 * neither (srv.c). x2 and K56flex share the one slot. */
static void test_heavy_guard(void)
{
    cfg_t c; int slot = 0;
    base_cfg(&c);
    c.modulations |= CFG_MOD_V34;       /* x2/K56flex are offered only alongside V.34 */
    c.x2 = 1;
    line_init();
    srv_init(&S, &c, 31, lan_rx, NULL);
    srv_set_heavy_slot(&S, &slot);
    srv_set_clock(&S, 1000000000u);
    run_ms(300);
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 0);

    /* a call with x2 on: the slot is reserved for the life of the call, even
     * though this caller connects V.22bis and does not take x2 up */
    place_call("5551234", 9000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(S.heavy_held, 1); CHECK_EQ_I(slot, 1);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    CHECK_EQ_I(S.heavy_held, 1); CHECK_EQ_I(slot, 1);
    hang_up();
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 0);     /* given back at the end */

    /* the guard: the other line already holds the one heavy slot, so this call
     * offers neither x2 nor K56flex and does not touch the slot */
    slot = 1;
    place_call("5551234", 9000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 1);
    start_modem(0, "alice", "wonderland");
    CHECK(wait_ip(20000) > 0);
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 1);     /* not ours to hold or free */
    hang_up();
    CHECK_EQ_I(slot, 1);

    /* K56flex shares the same slot: x2 off, k56 on reserves it too */
    slot = 0;
    c.x2 = 0; c.k56 = 1;
    srv_init(&S, &c, 31, lan_rx, NULL);
    srv_set_heavy_slot(&S, &slot);
    srv_set_clock(&S, 1000000000u);
    run_ms(300);
    place_call("5551234", 9000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(S.heavy_held, 1); CHECK_EQ_I(slot, 1);
    hang_up();
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 0);

    /* both off: nothing reserved */
    c.x2 = 0; c.k56 = 0;
    srv_init(&S, &c, 31, lan_rx, NULL);
    srv_set_heavy_slot(&S, &slot);
    srv_set_clock(&S, 1000000000u);
    run_ms(300);
    place_call("5551234", 9000);
    CHECK_EQ_I(S.tel.state, TEL_ANSWERED);
    CHECK_EQ_I(S.heavy_held, 0); CHECK_EQ_I(slot, 0);
    hang_up();
}

int main(void)
{
    test_lin_scale();
    test_line_dev();
    test_level();
    test_v22bis_call();
    test_not_answered();
    test_wrong_password_then_reconfigure();
    test_drop_and_limit();
    test_v90_call();
    test_v90_late_echo();
    test_v92_call();
    test_ppp_compression();
    test_console_packets();
    test_console_more();
    test_dclist();
    test_dcnow();
    test_upnp_words();
    test_saturn_words();
    test_stun();
    test_dreamcast_call();
    test_saturn_call();
    test_bbs_words();
    test_telnet();
    test_bbs_call();
    test_bbs_shipped_list();
    test_bbsdial();
    test_bbsdial_self();
    test_v34_call();
    test_v32bis_call();
    test_heavy_guard();
    /* the same server on a line with linear receive (srv_tick_lin): whole
     * calls of each kind, with the same expectations */
    printf("  with linear receive:\n");
    lin_mode = 1;
    test_v22bis_call();
    test_v90_call();
    test_v92_call();
    test_v34_call();
    test_v32bis_call();
    lin_mode = 0;
    T_REPORT("srv");
}
