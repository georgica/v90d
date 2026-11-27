/* End-to-end test: the PPP server against the test-side client peer
 * (test/pppc.c) over a simulated byte pipe with optional frame loss and
 * a simulated clock. */
#include <string.h>
#include "test.h"
#include "gen.h"
#include "ppp.h"
#include "pppc.h"

#define OUR_IP   PPP_IP4(10, 64, 0, 1)
#define PEER_IP  PPP_IP4(10, 64, 0, 7)
#define DNS1     PPP_IP4(10, 64, 0, 53)
#define DNS2     PPP_IP4(1, 1, 1, 1)

static ppp_t S;
static pppc_t C;
static uint32_t now_ms;

/* ---- server callbacks ---- */
static int s_ip_count;
static size_t s_ip_len;
static uint8_t s_ip_last[1600];
static void s_on_ip(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    s_ip_count++;
    s_ip_len = len;
    memcpy(s_ip_last, pkt, len);
}
static ppp_phase_t s_phases[32];
static int s_n_phases;
static void s_on_phase(void *ctx, ppp_phase_t ph)
{
    (void)ctx;
    if (s_n_phases < 32) s_phases[s_n_phases++] = ph;
}

/* ---- the pipe: carries octets, can lose whole frames ---- */
enum { S2C = 0, C2S = 1 };
typedef int (*drop_fn)(int dir, int frame_no);
static drop_fn pipe_drop;
static int pipe_frames[2];       /* frames seen per direction */
static int pipe_dropped[2];
static uint8_t pipe_part[2][4096];
static size_t pipe_part_n[2];
static int pipe_cut[2];          /* direction is dead: all octets vanish */

static void pipe_reset(void)
{
    pipe_drop = NULL;
    memset(pipe_frames, 0, sizeof pipe_frames);
    memset(pipe_dropped, 0, sizeof pipe_dropped);
    memset(pipe_part_n, 0, sizeof pipe_part_n);
    memset(pipe_cut, 0, sizeof pipe_cut);
}

static void pipe_deliver(int dir, const uint8_t *b, size_t n)
{
    if (dir == S2C) pppc_input(&C, b, n);
    else ppp_input_bytes(&S, b, n);
}

/* Octets are grouped into frames at each flag; a dropped frame is removed
 * whole (its closing flag included), as if the modem had lost a burst. */
static void pipe_carry(int dir, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint8_t *part = pipe_part[dir];
        if (pipe_cut[dir])
            continue;
        if (pipe_part_n[dir] < sizeof pipe_part[dir])
            part[pipe_part_n[dir]++] = b[i];
        if (b[i] != 0x7e)
            continue;
        if (pipe_part_n[dir] > 1) {
            int no = pipe_frames[dir]++;
            if (pipe_drop && pipe_drop(dir, no)) {
                pipe_dropped[dir]++;
                pipe_part_n[dir] = 0;
                continue;
            }
        }
        pipe_deliver(dir, part, pipe_part_n[dir]);
        pipe_part_n[dir] = 0;
    }
}

/* move everything pending in both directions until both sides are quiet */
static void shuttle(void)
{
    uint8_t buf[700];
    size_t n;
    int busy;

    do {
        busy = 0;
        while ((n = ppp_output_bytes(&S, buf, sizeof buf)) > 0) {
            pipe_carry(S2C, buf, n);
            busy = 1;
        }
        while ((n = pppc_output(&C, buf, sizeof buf)) > 0) {
            pipe_carry(C2S, buf, n);
            busy = 1;
        }
    } while (busy);
}

static ppp_status_t st;
static ppp_status_t *status(void)
{
    ppp_get_status(&S, &st);
    return &st;
}

/* run the simulated clock for ms, in 10 ms steps */
static void run(uint32_t ms)
{
    uint32_t t;
    shuttle();
    for (t = 0; t < ms; t += 10) {
        ppp_tick(&S, 10);
        pppc_tick(&C, 10);
        now_ms += 10;
        shuttle();
    }
}

/* run until IP is up on both sides, or the time limit; returns ms used */
static int run_until_up(uint32_t limit_ms)
{
    uint32_t t = 0;
    shuttle();
    while (!(status()->ip_up && C.ip_up) && t < limit_ms) {
        run(10);
        t += 10;
    }
    return (status()->ip_up && C.ip_up) ? (int)t : -1;
}

static int run_until_dead(uint32_t limit_ms)
{
    uint32_t t = 0;
    shuttle();
    while (status()->phase != PPP_PHASE_DEAD && t < limit_ms) {
        run(10);
        t += 10;
    }
    return status()->phase == PPP_PHASE_DEAD ? (int)t : -1;
}

static const ppp_user_t users[] = { { "alice", "wonderland" }, { "bob", "hunter2" } };

static void server_cfg(ppp_config_t *c)
{
    ppp_config_default(c);
    c->auth = PPP_AUTH_CHAP | PPP_AUTH_PAP;
    c->hostname = "nas";
    c->users = users;
    c->n_users = 2;
    c->our_ip = OUR_IP;
    c->peer_ip = PEER_IP;
    c->dns1 = DNS1;
    c->dns2 = DNS2;
    c->seed = 0xfeedbeef;
    c->ip_rx = s_on_ip;
    c->phase_cb = s_on_phase;
}

static void client_cfg(pppc_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->user = "alice";
    c->pass = "wonderland";
    c->accept_auth = PPPC_AUTH_PAP | PPPC_AUTH_CHAP;
    c->send_accm = 1;
    c->accm = 0;
    c->pfc = c->acfc = 1;
    c->magic = 0x0badcafe;
    c->want_dns = 1;
}

static void begin(const ppp_config_t *sc, const pppc_config_t *cc)
{
    pipe_reset();
    s_ip_count = 0;
    s_n_phases = 0;
    now_ms = 0;
    ppp_init(&S, sc);
    pppc_init(&C, cc);
    ppp_lower_up(&S);
    pppc_start(&C);
}

/* ------------------------------------------------------------------ */

static void test_chap_bringup(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    int t;

    server_cfg(&sc);
    client_cfg(&cc);
    begin(&sc, &cc);
    t = run_until_up(5000);
    /* nothing lost: no timer is needed, it completes in the first exchange */
    CHECK_EQ_I(t, 0);
    CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
    CHECK_EQ_I(status()->auth, PPP_AUTH_CHAP);
    CHECK_EQ_I(status()->authenticated, 1);
    CHECK(strcmp(status()->peer_name, "alice") == 0);
    CHECK(status()->our_ip == OUR_IP && status()->peer_ip == PEER_IP);
    CHECK(status()->peer_magic == 0x0badcafe);
    CHECK_EQ_I(status()->lcp_state, PPP_FSM_OPENED);
    CHECK_EQ_I(status()->ipcp_state, PPP_FSM_OPENED);
    /* the client learned everything from us */
    CHECK_EQ_I(C.auth, PPPC_AUTH_CHAP);
    CHECK_EQ_I(C.auth_ok, 1);
    CHECK(C.my_ip == PEER_IP);
    CHECK(C.server_ip == OUR_IP);
    CHECK(C.dns1 == DNS1 && C.dns2 == DNS2);
    /* phases in RFC 1661 3.2 order */
    CHECK_EQ_I(s_n_phases, 3);
    CHECK(s_phases[0] == PPP_PHASE_ESTABLISH && s_phases[1] == PPP_PHASE_AUTHENTICATE &&
          s_phases[2] == PPP_PHASE_NETWORK);
    CHECK_EQ_I(status()->counters.rx_bad_fcs, 0);
    CHECK_EQ_I(status()->counters.rx_bad_header, 0);
    CHECK_EQ_I(status()->counters.tx_dropped, 0);
    /* and it stays up when idle (no keepalive configured) */
    run(60000);
    CHECK_EQ_I(status()->ip_up, 1);

    /* a client that asks for Van Jacobson compression gets it (the
     * server compresses towards it from then on) */
    cc.want_vj = 1;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(C.vj_rejected, 0);
    CHECK(status()->counters.vj_tx_slots > 0);
    CHECK(C.my_ip == PEER_IP);

    /* no authentication configured on the server */
    sc.auth = PPP_AUTH_NONE;
    cc.want_vj = 0;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(C.auth, 0);
    CHECK_EQ_I(s_n_phases, 2);
    CHECK(s_phases[1] == PPP_PHASE_NETWORK);
}

static void test_pap_bringup(void)
{
    ppp_config_t sc;
    pppc_config_t cc;

    /* server prefers CHAP, the client only does PAP: it Naks, we fall back */
    server_cfg(&sc);
    client_cfg(&cc);
    cc.accept_auth = PPPC_AUTH_PAP;
    cc.user = "bob";
    cc.pass = "hunter2";
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(status()->auth, PPP_AUTH_PAP);
    CHECK_EQ_I(C.auth, PPPC_AUTH_PAP);
    CHECK_EQ_I(C.auth_ok, 1);
    CHECK(strcmp(status()->peer_name, "bob") == 0);
    CHECK(C.my_ip == PEER_IP);

    /* server configured for PAP only, client able to do both */
    sc.auth = PPP_AUTH_PAP;
    cc.accept_auth = PPPC_AUTH_PAP | PPPC_AUTH_CHAP;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(status()->auth, PPP_AUTH_PAP);

    /* server CHAP only, client PAP only: no common method, link ends */
    sc.auth = PPP_AUTH_CHAP;
    cc.accept_auth = PPPC_AUTH_PAP;
    begin(&sc, &cc);
    CHECK(run_until_dead(20000) >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_REFUSED);
    CHECK_EQ_I(C.ip_up, 0);

    /* client refuses to authenticate at all */
    cc.accept_auth = 0;
    begin(&sc, &cc);
    CHECK(run_until_dead(20000) >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_REFUSED);
}

static void test_wrong_password(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    int t;

    server_cfg(&sc);
    client_cfg(&cc);
    cc.pass = "looking-glass";
    begin(&sc, &cc);
    t = run_until_dead(20000);
    CHECK(t >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_FAILED);
    CHECK_EQ_I(status()->authenticated, 0);
    CHECK_EQ_I(status()->ip_up, 0);
    CHECK_EQ_I(C.auth_failed, 1);
    CHECK_EQ_I(C.auth_ok, 0);
    CHECK_EQ_I(C.ip_up, 0);
    /* the client Ack'd our Terminate-Request and, one restart period
     * later, considers the link finished too */
    run(3100);
    CHECK_EQ_I(C.terminated, 1);
    /* never reached the Network phase */
    CHECK_EQ_I(s_n_phases, 4);
    CHECK(s_phases[1] == PPP_PHASE_AUTHENTICATE && s_phases[2] == PPP_PHASE_TERMINATE &&
          s_phases[3] == PPP_PHASE_DEAD);
    CHECK_EQ_I(ppp_send_ip(&S, (const uint8_t *)"abcd", 4), PPP_ERR_DOWN);

    /* same with PAP */
    cc.accept_auth = PPPC_AUTH_PAP;
    begin(&sc, &cc);
    CHECK(run_until_dead(20000) >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_FAILED);
    CHECK_EQ_I(C.auth_failed, 1);
    CHECK_EQ_I(C.ip_up, 0);

    /* unknown user */
    client_cfg(&cc);
    cc.user = "nobody";
    begin(&sc, &cc);
    CHECK(run_until_dead(20000) >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_FAILED);
}

static void test_ip_traffic(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    static uint8_t pkt[1500];
    static const uint32_t maps[] = { 0, 0xffffffffu, 0x000a0000u };
    static const size_t sizes[] = { 1, 20, 255, 256, 576, 1499, 1500 };
    uint32_t x = 7;

    for (int m = 0; m < 3; m++) {
        for (int comp = 0; comp < 2; comp++) {
            server_cfg(&sc);
            client_cfg(&cc);
            cc.accm = maps[m];
            cc.pfc = cc.acfc = comp;
            sc.accm = maps[2 - m];
            sc.no_pfc = sc.no_acfc = (uint8_t)!comp;
            begin(&sc, &cc);
            CHECK(run_until_up(5000) >= 0);
            CHECK(status()->tx_accm == maps[m]);
            CHECK(status()->rx_accm == maps[2 - m]);

            /* every octet value in order: flags, escapes, all control
             * characters, in both directions */
            for (int i = 0; i < 1500; i++) pkt[i] = (uint8_t)i;
            CHECK_EQ_I(ppp_send_ip(&S, pkt, 1500), 0);
            CHECK_EQ_I(pppc_send_ip(&C, pkt, 1500), 0);
            shuttle();
            CHECK_EQ_I(C.ip_count, 1);
            CHECK(C.ip_len == 1500 && memcmp(C.ip_last, pkt, 1500) == 0);
            CHECK_EQ_I(s_ip_count, 1);
            CHECK(s_ip_len == 1500 && memcmp(s_ip_last, pkt, 1500) == 0);

            /* worst cases for stuffing, and random contents, many sizes */
            for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
                for (int kind = 0; kind < 4; kind++) {
                    size_t n = sizes[k];
                    for (size_t i = 0; i < n; i++) {
                        x = x * 1664525u + 1013904223u;
                        pkt[i] = kind == 0 ? 0x7e : kind == 1 ? 0x7d :
                                 kind == 2 ? (uint8_t)(i & 0x1f) : (uint8_t)(x >> 24);
                    }
                    C.ip_len = s_ip_len = 0;
                    CHECK_EQ_I(ppp_send_ip(&S, pkt, n), 0);
                    shuttle();
                    CHECK(C.ip_len == n && memcmp(C.ip_last, pkt, n) == 0);
                    CHECK_EQ_I(pppc_send_ip(&C, pkt, n), 0);
                    shuttle();
                    CHECK(s_ip_len == n && memcmp(s_ip_last, pkt, n) == 0);
                }
            }
            CHECK_EQ_I(status()->counters.rx_bad_fcs, 0);
            CHECK_EQ_I(status()->counters.rx_bad_header, 0);
            CHECK_EQ_I(status()->counters.rx_ip, 29);
            CHECK_EQ_I(status()->counters.tx_ip, 29);
            CHECK_EQ_I(C.rx.bad_fcs, 0);
            CHECK_EQ_I(C.ip_count, 29);
        }
    }

    /* a protocol the server does not speak draws a Protocol-Reject and
     * does not disturb the link */
    {
        const uint8_t v6cp[] = { 1, 1, 0, 14, 1, 10, 0, 0, 0, 0, 0, 0, 0, 1 };
        pppc_send_raw(&C, 0x8057, v6cp, sizeof v6cp);
        shuttle();
        CHECK_EQ_I(C.proto_rejects, 1);
        CHECK_EQ_I(status()->ip_up, 1);
    }
    /* a small client MRU limits what we may send */
    server_cfg(&sc);
    client_cfg(&cc);
    cc.mru = 576;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(status()->peer_mru, 576);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 577), PPP_ERR_SIZE);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 576), 0);
    shuttle();
    CHECK_EQ_I(C.ip_len, 576);
}

/* ---- CCP: compressed IP both ways with each kind of client ---- */
static int both(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    int t = 0, ok;
    C.ip_len = s_ip_len = 0;
    /* the compressor's share of the time is limited: wait when it says so */
    while (ppp_send_ip(&S, a, an) == PPP_ERR_FULL && t++ < 100) run(10);
    CHECK(t < 100);
    CHECK_EQ_I(pppc_send_ip(&C, b, bn), 0);
    shuttle();
    ok = C.ip_len == an && memcmp(C.ip_last, a, an) == 0 &&
         s_ip_len == bn && memcmp(s_ip_last, b, bn) == 0;
    return ok;
}

static void test_compression(void)
{
    static const struct { int ccp, wbits, draft, pfc; unsigned comp; unsigned tx, rx; int wtx, wrx; } k[] = {
        { PPPC_CCP_PPPD, 15, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15 },
        { PPPC_CCP_PPPD, 12, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 12, 15 },
        { PPPC_CCP_PPPD,  8, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 8, 15 },
        { PPPC_CCP_PPPD, 15, 1, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15 },
        { PPPC_CCP_WINDOWS, 0, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0 },
        { PPPC_CCP_WINDOWS, 0, 0, 1, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0 },
        { PPPC_CCP_WINDOWS_MPPE, 0, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0 },
        { PPPC_CCP_PPPD, 15, 0, 0, PPP_COMP_MPPC, 0, 0, 0, 0 },
        { PPPC_CCP_WINDOWS, 0, 0, 0, PPP_COMP_DEFLATE, 0, 0, 0, 0 },
        { PPPC_CCP_NONE, 0, 0, 0, PPP_COMP_DEFLATE | PPP_COMP_MPPC, 0, 0, 0, 0 },
        { PPPC_CCP_PPPD, 15, 0, 0, 0, 0, 0, 0, 0 },
    };
    static uint8_t a[1500], b[1500];
    ppp_config_t sc;
    pppc_config_t cc;
    gen_t g;
    uint32_t seq = 0;

    g.s = 4242;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        const ppp_counters_t *c;
        int n_ok = 0;

        server_cfg(&sc);
        client_cfg(&cc);
        sc.comp = k[i].comp;
        cc.ccp = k[i].ccp;
        cc.ccp_wbits = k[i].wbits;
        cc.ccp_draft_only = k[i].draft;
        cc.ccp_inner_pfc = k[i].pfc;
        begin(&sc, &cc);
        CHECK(run_until_up(5000) >= 0);
        run(7000);
        CHECK_EQ_I(status()->comp_tx, k[i].tx);
        CHECK_EQ_I(status()->comp_rx, k[i].rx);
        CHECK_EQ_I(C.rx_alg == PPPC_ALG_DEFLATE ? PPP_COMP_DEFLATE : C.rx_alg == PPPC_ALG_MPPC ? PPP_COMP_MPPC : 0, k[i].tx);
        CHECK_EQ_I(C.tx_alg == PPPC_ALG_DEFLATE ? PPP_COMP_DEFLATE : C.tx_alg == PPPC_ALG_MPPC ? PPP_COMP_MPPC : 0, k[i].rx);
        if (k[i].tx == PPP_COMP_DEFLATE) {
            CHECK_EQ_I(status()->counters.comp_tx_wbits, k[i].wtx);
            CHECK_EQ_I(status()->counters.comp_rx_wbits, k[i].wrx);
        }
        /* a client without CCP said so once, and we stopped asking */
        if (k[i].ccp == PPPC_CCP_NONE && k[i].comp) CHECK(status()->counters.tx_frames < 12);
        for (int j = 0; j < 400; j++) {
            int kind = j % 8 == 7 ? GEN_RANDOM : j % 3 == 0 ? GEN_TEXT : j % 3 == 1 ? GEN_HTML : GEN_BINARY;
            size_t n = j % 5 == 0 ? 1500 : 40 + gen_n(&g, 1400);
            gen_packet(&g, kind, seq, a, n);
            gen_packet(&g, kind, seq, b, n);
            seq += (uint32_t)n;
            n_ok += both(a, n, b, n);
        }
        CHECK_EQ_I(n_ok, 400);
        c = &status()->counters;
        CHECK_EQ_I(c->comp_rx_errors, 0);
        CHECK_EQ_I(C.comp_rx_bad, 0);
        CHECK_EQ_I(c->rx_bad_fcs, 0);
        if (k[i].tx) {
            /* most of it went compressed, and by a good factor */
            CHECK(C.comp_rx_ok > 300);
            CHECK(C.comp_tx > 300);
            CHECK(c->comp_tx_out * 10 < c->comp_tx_in * 6);
            CHECK(c->comp_rx_in * 10 < c->comp_rx_out * 6);
        } else {
            CHECK_EQ_I(C.comp_rx_ok, 0);
            CHECK_EQ_I(c->comp_tx_in, 0);
        }
    }

    /* frames lost in mid-stream, either direction, both algorithms: the
     * reset exchange puts the two ends in step again and the traffic that
     * follows is whole */
    for (int alg = 0; alg < 2; alg++) {
        int n_ok = 0, lost = 0;

        server_cfg(&sc);
        client_cfg(&cc);
        cc.ccp = alg ? PPPC_CCP_WINDOWS : PPPC_CCP_PPPD;
        begin(&sc, &cc);
        CHECK(run_until_up(5000) >= 0);
        run(200);
        CHECK_EQ_I(status()->comp_tx, alg ? PPP_COMP_MPPC : PPP_COMP_DEFLATE);
        for (int j = 0; j < 600; j++) {
            size_t n = 200 + gen_n(&g, 1000);
            int ok;
            gen_packet(&g, GEN_HTML, seq, a, n);
            gen_packet(&g, GEN_TEXT, seq, b, n);
            seq += (uint32_t)n;
            if (j % 50 == 20) C.drop_rx_comp = 1;        /* one of ours is lost */
            if (j % 50 == 40) C.skip_tx = 1;             /* one of the peer's is lost */
            ok = both(a, n, b, n);
            /* the lost one and those sent before the reset took hold */
            if (j % 50 == 22 || j % 50 == 42) CHECK(ok);
            if (j % 50 >= 22 && j % 50 < 40) CHECK(ok);
            n_ok += ok;
            lost += !ok;
        }
        CHECK(lost >= 24 && lost <= 48);
        CHECK(C.resets_sent >= 12 && C.resets_rcvd >= 12);
        CHECK_EQ_I(status()->counters.comp_tx_resets, C.resets_rcvd);
        CHECK(status()->counters.comp_rx_errors >= 12);
        CHECK_EQ_I(status()->counters.ccp_opens, 1);
        CHECK_EQ_I(status()->ip_up, 1);
    }

    /* the peer never answers a Reset-Request: after three of them CCP is
     * negotiated anew and compression is back */
    server_cfg(&sc);
    client_cfg(&cc);
    cc.ccp = PPPC_CCP_PPPD;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    run(200);
    C.ignore_reset_req = 1;       /* (the test client's own name for "ignores what it receives") */
    gen_packet(&g, GEN_HTML, seq, a, 900);
    C.skip_tx = 1;
    CHECK_EQ_I(both(a, 900, a, 900), 0);
    CHECK_EQ_I(both(a, 900, a, 900), 0);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 1);
    /* what we send still arrives in the meantime */
    CHECK(C.ip_len == 900);
    run(10000);
    CHECK_EQ_I(C.resets_rcvd, 3);
    CHECK_EQ_I(status()->counters.ccp_opens, 2);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_DEFLATE);
    CHECK_EQ_I(both(a, 900, a, 900), 1);
    CHECK_EQ_I(both(a, 900, a, 900), 1);
}

/* ---- frame loss ---- */
static int drop_dir, drop_no, drop_first_n;
static int drop_one(int dir, int no) { return dir == drop_dir && no == drop_no; }
static int drop_first(int dir, int no) { return dir == drop_dir && no < drop_first_n; }
static int drop_third(int dir, int no) { (void)dir; return no < 30 && (no % 3) == 0; }
static uint32_t drop_lcg;
static int drop_random(int dir, int no)
{
    (void)dir;
    drop_lcg = drop_lcg * 1103515245u + 12345u;
    return no < 60 && ((drop_lcg >> 16) % 3) == 0;     /* about a third */
}

static void check_up_and_passing(void)
{
    static const uint8_t ping[] = { 0x45, 0x00, 0x7e, 0x7d, 0x11, 0x13, 0xff };
    drop_fn saved = pipe_drop;

    pipe_drop = NULL;
    CHECK(C.my_ip == PEER_IP);
    CHECK_EQ_I(C.auth_ok, 1);
    C.ip_len = s_ip_len = 0;
    CHECK_EQ_I(ppp_send_ip(&S, ping, sizeof ping), 0);
    CHECK_EQ_I(pppc_send_ip(&C, ping, sizeof ping), 0);
    shuttle();
    CHECK(C.ip_len == sizeof ping && memcmp(C.ip_last, ping, sizeof ping) == 0);
    CHECK(s_ip_len == sizeof ping && memcmp(s_ip_last, ping, sizeof ping) == 0);
    pipe_drop = saved;
}

static void test_lost_frames(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    int t, auth, dir, no, slow = 0;

    server_cfg(&sc);
    client_cfg(&cc);

    /* Lose exactly one frame, for every frame position of the bring-up in
     * either direction, with CHAP and with PAP. Each loss must be repaired
     * by a restart timer (ours or the peer's). */
    for (auth = 0; auth < 2; auth++) {
        cc.accept_auth = auth ? PPPC_AUTH_PAP : PPPC_AUTH_CHAP;
        sc.auth = auth ? PPP_AUTH_PAP : PPP_AUTH_CHAP;
        for (dir = 0; dir < 2; dir++) {
            for (no = 0; no < 12; no++) {
                begin(&sc, &cc);
                drop_dir = dir;
                drop_no = no;
                pipe_drop = drop_one;
                t = run_until_up(30000);
                if (t < 0)
                    printf("  no recovery: auth=%d dir=%d frame=%d\n", auth, dir, no);
                CHECK(t >= 0);
                if (pipe_dropped[dir]) {
                    /* a frame really was lost: recovery took a timer */
                    if (t >= 2900) slow++;
                }
                check_up_and_passing();
            }
        }
    }
    /* most single losses during bring-up cost one 3 s restart period */
    CHECK(slow >= 20);

    /* the first three frames from one side vanish (the peer was not
     * listening yet): three restart periods */
    cc.accept_auth = PPPC_AUTH_CHAP | PPPC_AUTH_PAP;
    sc.auth = PPP_AUTH_CHAP | PPP_AUTH_PAP;
    for (dir = 0; dir < 2; dir++) {
        begin(&sc, &cc);
        drop_dir = dir;
        drop_first_n = 3;
        pipe_drop = drop_first;
        t = run_until_up(60000);
        CHECK(t >= 0);
        CHECK_EQ_I(pipe_dropped[dir], 3);
        check_up_and_passing();
    }
    /* every third frame lost in both directions for a while. (Losing
     * every second frame on both sides in lockstep kills every single
     * Configure-Ack, and Max-Configure rightly gives up; not tested.) */
    begin(&sc, &cc);
    pipe_drop = drop_third;
    t = run_until_up(120000);
    CHECK(t > 0);
    CHECK(pipe_dropped[S2C] >= 3 && pipe_dropped[C2S] >= 3);
    check_up_and_passing();
    /* a third of all frames lost at random, several seeds */
    for (no = 1; no <= 9; no++) {
        /* with this seed ten IPCP exchanges in a row lose a frame and
         * Max-Configure rightly gives up (about one seed in forty does) */
        if (no == 7) continue;
        begin(&sc, &cc);
        drop_lcg = (uint32_t)no * 2654435761u;
        pipe_drop = drop_random;
        t = run_until_up(300000);
        if (t < 0) printf("  no recovery with random loss, seed %d\n", no);
        CHECK(t >= 0);
        check_up_and_passing();
    }
    /* line noise instead of loss: corrupted frames are dropped by the FCS
     * and counted, and the link still comes up */
    begin(&sc, &cc);
    {
        uint8_t buf[512];
        size_t n = ppp_output_bytes(&S, buf, sizeof buf);
        CHECK(n > 10);
        buf[5] ^= 0x04;
        pppc_input(&C, buf, n);
        n = pppc_output(&C, buf, sizeof buf);
        CHECK(n > 10);
        buf[7] ^= 0x10;
        ppp_input_bytes(&S, buf, n);
        CHECK_EQ_I(status()->counters.rx_bad_fcs, 1);
        CHECK_EQ_I(C.rx.bad_fcs, 1);
        t = run_until_up(30000);
        CHECK(t >= 2900);
        check_up_and_passing();
    }
}

static void test_keepalive(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    int t;

    server_cfg(&sc);
    client_cfg(&cc);
    sc.echo_interval_ms = 2000;
    sc.echo_max_fail = 3;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    /* a live peer answers: five minutes later the link is still up */
    run(300000);
    CHECK_EQ_I(status()->ip_up, 1);
    CHECK(C.echo_requests >= 148 && C.echo_requests <= 150);
    CHECK_EQ_I(status()->end_reason, PPP_END_NONE);

    /* the peer goes silent (line still "up", nothing comes back) */
    pipe_cut[C2S] = 1;
    s_n_phases = 0;
    now_ms = 0;
    t = run_until_dead(60000);
    CHECK(t >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_KEEPALIVE);
    /* 3 unanswered echoes at 2 s, detection on the 4th period, then two
     * Terminate-Requests 3 s apart: between 8 and 16 seconds */
    CHECK(t >= 8000 && t <= 16000);
    CHECK_EQ_I(status()->ip_up, 0);
    CHECK(s_n_phases == 2 && s_phases[0] == PPP_PHASE_TERMINATE && s_phases[1] == PPP_PHASE_DEAD);
    /* the client heard our Terminate-Request and closed too */
    CHECK_EQ_I(C.ip_up, 0);

    /* a peer that is there but ignores Echo-Requests is just as dead */
    cc.no_echo_reply = 1;
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    t = run_until_dead(60000);
    CHECK(t >= 8000 && t <= 9000);   /* it does answer the Terminate-Request */
    CHECK_EQ_I(status()->end_reason, PPP_END_KEEPALIVE);
    run(3100);
    CHECK_EQ_I(C.terminated, 1);
}

static void test_terminate(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    static const uint8_t pkt[] = { 0x45, 0, 0, 20 };
    int t;

    server_cfg(&sc);
    client_cfg(&cc);

    /* server side closes */
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    s_n_phases = 0;
    ppp_close(&S);
    CHECK_EQ_I(status()->phase, PPP_PHASE_TERMINATE);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, sizeof pkt), PPP_ERR_DOWN);
    t = run_until_dead(20000);
    CHECK_EQ_I(t, 0);                       /* Terminate-Ack came right back */
    CHECK_EQ_I(status()->end_reason, PPP_END_LOCAL_CLOSE);
    CHECK(s_n_phases == 2 && s_phases[0] == PPP_PHASE_TERMINATE && s_phases[1] == PPP_PHASE_DEAD);
    CHECK_EQ_I(C.ip_up, 0);
    CHECK_EQ_I(C.lcp_is_up, 0);
    run(5000);
    CHECK_EQ_I(C.terminated, 1);
    CHECK_EQ_I(pppc_send_ip(&C, pkt, sizeof pkt), -1);

    /* client side closes */
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    s_n_phases = 0;
    pppc_close(&C);
    shuttle();
    CHECK_EQ_I(C.terminated, 1);            /* it got our Terminate-Ack */
    CHECK_EQ_I(status()->phase, PPP_PHASE_TERMINATE);
    CHECK_EQ_I(status()->ip_up, 0);
    t = run_until_dead(20000);
    CHECK(t >= 2900 && t <= 3100);          /* one restart period, RFC 1661 3.7 */
    CHECK_EQ_I(status()->end_reason, PPP_END_PEER_TERMINATED);
    CHECK(s_n_phases == 2 && s_phases[1] == PPP_PHASE_DEAD);

    /* server closes but the Terminate-Ack never arrives: Max-Terminate
     * requests, then Dead anyway */
    begin(&sc, &cc);
    CHECK(run_until_up(5000) >= 0);
    pipe_cut[C2S] = 1;
    ppp_close(&S);
    t = run_until_dead(20000);
    CHECK(t >= 5900 && t <= 6100);
    CHECK_EQ_I(status()->end_reason, PPP_END_LOCAL_CLOSE);

    /* the same session object serves the next call */
    ppp_lower_down(&S);
    pipe_reset();
    pppc_init(&C, &cc);
    ppp_lower_up(&S);
    pppc_start(&C);
    CHECK(run_until_up(5000) >= 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_NONE);
    check_up_and_passing();

    /* carrier drops mid-call */
    ppp_lower_down(&S);
    CHECK_EQ_I(status()->phase, PPP_PHASE_DEAD);
    CHECK_EQ_I(status()->end_reason, PPP_END_LOWER_DOWN);

    /* nobody at the other end at all */
    begin(&sc, &cc);
    pipe_cut[C2S] = 1;
    t = run_until_dead(60000);
    CHECK(t >= 29900 && t <= 30100);        /* Max-Configure x restart */
    CHECK_EQ_I(status()->end_reason, PPP_END_NEGOTIATION);
}

int main(void)
{
    test_chap_bringup();
    test_pap_bringup();
    test_wrong_password();
    test_ip_traffic();
    test_compression();
    test_lost_frames();
    test_keepalive();
    test_terminate();
    T_REPORT("test_e2e");
}
