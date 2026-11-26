/* Complete simulated calls: the answering DCE (dce.h: modem + V.42 + PPP
 * server) against the calling test peer (test/caller.h: calling modem +
 * V.42 originator + PPP client) through the channel simulator, sample by
 * sample. Run with the argument "quick" for a shorter subset; a further
 * argument (or a first one other than "quick") picks scenarios by their
 * letters, for example "hk". */
#include <string.h>
#include "test.h"
#include "harness.h"

#define OUR_IP   HARNESS_OUR_IP
#define PEER_IP  HARNESS_PEER_IP
#define DNS1     HARNESS_DNS1

static int quick;

static void show_rates(const char *what, const long r[2], int bitrate)
{
    printf("    %-34s down %4ld octets/s (%3ld%%)   up %4ld octets/s (%3ld%%)\n", what,
           r[DN], r[DN] * 800 / bitrate, r[UP], r[UP] * 800 / bitrate);
}

/* ------------------------------------------------------------------ (a) */
static void test_v22bis_lapm(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    long r[2];
    int t, n = quick ? 10 : 20, nb = quick ? 30 : 50;

    printf("(a) V.8 + V.22bis 2400 + LAPM + V.42bis + CHAP + IPCP\n");
    server_cfg(&sc); client_cfg(&cc);
    cc.v42.lapm.v44_tx = cc.v42.lapm.v44_rx = 0;       /* a caller from before V.44 */
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 30000);
    CHECK(t > 0);
    s = status();
    printf("    IP up %.2f s after answering (modem connected, V.42 and PPP done)\n", t / 1000.0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->end_reason, DCE_END_NONE);
    CHECK_EQ_I(s->modem_connected, 1);
    CHECK_EQ_I(s->used_v8, 1);
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(s->bitrate, 2400);
    CHECK_EQ_I(s->v42_state, V42EC_CONNECTED);
    CHECK_EQ_I(s->error_control, 1);
    CHECK_EQ_I(s->comp_tx, 1);
    CHECK_EQ_I(s->comp_rx, 1);
    CHECK_EQ_I(s->comp_alg, V42EC_COMP_V42BIS);
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_NETWORK);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(strcmp(s->peer_name, "alice") == 0);
    CHECK(s->our_ip == OUR_IP && s->peer_ip == PEER_IP);
    CHECK_EQ_I(C.ppp.auth, PPPC_AUTH_CHAP);
    CHECK(C.ppp.my_ip == PEER_IP && C.ppp.server_ip == OUR_IP && C.ppp.dns1 == DNS1);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_CONNECTED);
    CHECK(cv.comp_tx && cv.comp_rx);

    /* packets of assorted sizes, verified octet for octet on arrival */
    CHECK(traffic(RANDOM, n, 0, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[DN].n_bad, 0);
    show_rates("incompressible, down only:", r, 2400);
    CHECK(r[DN] > 270);                    /* synchronous: better than 240 cps */
    CHECK(traffic(RANDOM, 0, n, 120000, r));
    CHECK_EQ_I(F[UP].n_rx, n); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates("incompressible, up only:", r, 2400);
    CHECK(r[UP] > 270);
    /* more than PPP's transmit buffer holds, both ways at once: the senders
     * are told "full" over and over and nothing accepted may get lost */
    CHECK(traffic(RANDOM, nb, nb, 300000, r));
    CHECK_EQ_I(F[DN].n_rx, nb); CHECK_EQ_I(F[UP].n_rx, nb);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates("incompressible, both ways at once:", r, 2400);
    printf("    (%ld + %ld octets; senders held off: down %ld times, up %ld times)\n",
           F[DN].bytes_rx, F[UP].bytes_rx, n_busy[DN], n_busy[UP]);
    CHECK(n_busy[DN] > 100 && n_busy[UP] > 100);
    CHECK(r[DN] > 270 && r[UP] > 270);
    CHECK(traffic(TEXT, n, n, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates("text (V.42bis at work), both ways:", r, 2400);
    CHECK(r[DN] > 600 && r[UP] > 600);

    s = status();
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->ppp_counters.rx_bad_fcs, 0);
    CHECK_EQ_I(s->ppp_counters.tx_dropped, 0);
    CHECK_EQ_I(s->ppp_counters.rx_ip, 2 * n + nb);
    CHECK_EQ_I(s->ppp_counters.tx_ip, 2 * n + nb);
    CHECK_EQ_I(C.ppp.rx.bad_fcs, 0);
    CHECK(s->octets_down > 0 && s->octets_up > 0 && s->line_bits_tx > 0 && s->line_bits_rx > 0);
    printf("    LAPM: %lu I frames sent, %lu received, %lu retransmitted, %lu bad frames, %lu T401 expiries\n",
           s->lapm_tx_i, s->lapm_rx_i, s->lapm_retx, s->lapm_rx_bad, s->lapm_t401_expiries);
    /* a clean line: nothing is sent twice and the acknowledgement timer,
     * which the DCE scales to the line rate, never runs out */
    CHECK_EQ_I(s->lapm_retx, 0);
    CHECK_EQ_I(s->lapm_t401_expiries, 0);
}

/* ------------------------------------------------------------------ (b) */
static void test_noisy_line(void)
{
    static const line_t noisy = { -46.0, 500, 3, 4000, 0.2 };
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    long r[2];
    int t, n = quick ? 10 : 20;

    printf("(b) the same over a noisy line (steady noise %.0f dBFS plus a %d ms burst every %d ms)\n",
           noisy.noise_dbfs, noisy.burst_ms, noisy.burst_every_ms);
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &noisy);
    t = run_until(p_ip_up, 30000);
    CHECK(t > 0);
    CHECK_EQ_I(status()->error_control, 1);
    bursts_on = 1;
    CHECK(traffic(RANDOM, n, n, 400000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n);
    CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates("incompressible, both ways at once:", r, 2400);
    CHECK(traffic(TEXT, n, n, 400000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n);
    CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates("text, both ways at once:", r, 2400);
    bursts_on = 0;

    s = status();
    v42ec_get_status(&C.v42, &cv);
    printf("    %ld bursts; server LAPM: %lu bad frames, %lu retransmitted; caller LAPM: %lu bad frames, %lu retransmitted\n",
           n_bursts, s->lapm_rx_bad, s->lapm_retx, cv.rx_frames_bad, cv.tx_i_retx);
    /* the line really did damage frames in both directions ... */
    CHECK(s->lapm_rx_bad > 0);
    CHECK(cv.rx_frames_bad > 0);
    CHECK(s->lapm_retx > 0);
    CHECK(cv.tx_i_retx > 0);
    /* ... and PPP never saw any of it */
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->ppp_counters.rx_bad_fcs, 0);
    CHECK_EQ_I(s->ppp_counters.rx_runts + s->ppp_counters.rx_aborts + s->ppp_counters.rx_bad_header, 0);
    CHECK_EQ_I(C.ppp.rx.bad_fcs, 0);
    CHECK_EQ_I(s->ppp_counters.rx_ip, 2 * n);
}

/* ------------------------------------------------------------------ (c) */
static void test_no_error_control(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t, n = 10;

    printf("(c) caller without V.42: start-stop fallback (through the linear dce_tick)\n");
    server_cfg(&sc); client_cfg(&cc);
    cc.use_v42 = 0;
    harness_linear = 1;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 40000);
    CHECK(t > 0);
    s = status();
    printf("    IP up %.2f s after answering\n", t / 1000.0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->bitrate, 2400);
    CHECK_EQ_I(s->v42_state, V42EC_ASYNC);
    CHECK_EQ_I(s->error_control, 0);
    CHECK_EQ_I(s->comp_tx, 0);
    CHECK_EQ_I(s->comp_rx, 0);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(s->peer_ip == PEER_IP);
    CHECK(traffic(RANDOM, n, n, 200000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n);
    CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates("incompressible, both ways at once:", r, 2400);
    CHECK(r[DN] > 200 && r[DN] <= 240);    /* ten line bits per octet */
    s = status();
    CHECK_EQ_I(s->async_overruns, 0);
    CHECK_EQ_I(s->ppp_counters.rx_bad_fcs, 0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    harness_linear = 0;
}

/* every layer reports down, the transmitter is silent, nothing is accepted */
static void check_all_down(void)
{
    dce_status_t *s = status();
    int16_t tx = 123;
    uint8_t oct = 0x12;

    CHECK_EQ_I(dce_tick_pcm(&D, 0x33, &oct), 0);
    CHECK_EQ_I(oct, D.cfg.alaw ? 0xD5 : 0xFF);     /* G.711 silence */
    static const uint8_t pkt[20] = { 0x45 };

    CHECK_EQ_I(s->state, DCE_ENDED);
    CHECK_EQ_I(s->modem_connected, 0);
    CHECK_EQ_I(s->v42_state, V42EC_DISCONNECTED);
    CHECK_EQ_I(s->error_control, 0);
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_DEAD);
    CHECK_EQ_I(s->ip_up, 0);
    CHECK_EQ_I(dce_tick(&D, 1000, &tx), 0);
    CHECK_EQ_I(tx, 0);
    CHECK_EQ_I(dce_send_ip(&D, pkt, sizeof pkt), PPP_ERR_DOWN);
}

/* ------------------------------------------------------------------ (r) */
/* A call that carries octets and no PPP (dce.h, raw_rx): what a game
 * console's modem sends comes out as it is, what is written goes to it. */
static uint8_t raw_up[4096], raw_dn[4096]; static size_t raw_up_n, raw_dn_n, raw_cli_at, raw_cli_n;
static void raw_srv_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (raw_up_n + n <= sizeof raw_up) { memcpy(raw_up + raw_up_n, b, n); raw_up_n += n; } }
static void raw_cli_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (raw_dn_n + n <= sizeof raw_dn) { memcpy(raw_dn + raw_dn_n, b, n); raw_dn_n += n; } }
static size_t raw_cli_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = 0; (void)ctx;
    while (n < max && raw_cli_at < raw_cli_n) { b[n++] = (uint8_t)(raw_cli_at * 13 + 5); raw_cli_at++; }
    return n;
}
static int p_raw_online(void) { return status()->state == DCE_ONLINE; }
static int p_raw_done(void) { return raw_up_n >= 700 && raw_dn_n >= 3000; }

static void test_raw(void)
{
    dce_cfg_t sc; caller_cfg_t cc; dce_status_t *s; static uint8_t dn[3000]; size_t i, sent = 0; int t, bad = 0;

    printf("(r) octets instead of PPP: a console's modem, no error control\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx;
    cc.use_v42 = 0; cc.raw_rx = raw_cli_rx; cc.raw_tx = raw_cli_tx;
    raw_up_n = raw_dn_n = raw_cli_at = raw_cli_n = 0;
    CHECK_EQ_I(dce_raw_write(&D, dn, 10), 0);          /* nobody there */
    call_begin(&sc, &cc, &line_clean);
    CHECK_EQ_I(dce_raw_write(&D, dn, 10), 0);          /* not yet */
    t = run_until(p_raw_online, 40000);
    CHECK(t > 0);
    s = status();
    CHECK_EQ_I(s->v42_state, V42EC_ASYNC);
    CHECK_EQ_I(s->error_control, 0);
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_DEAD);
    CHECK_EQ_I(s->ip_up, 0);
    run_ms(500);
    CHECK_EQ_I(raw_up_n, 0); CHECK_EQ_I(raw_dn_n, 0);  /* nothing of our own goes out: no PPP */
    for (i = 0; i < sizeof dn; i++) dn[i] = (uint8_t)(i * 7 + 1);
    raw_cli_n = 700;
    for (t = 0; t < 200000 && !p_raw_done(); t++) {
        if (sent < sizeof dn) sent += dce_raw_write(&D, dn + sent, sizeof dn - sent);
        step();
        if ((t & 7) == 7 && status()->state != DCE_ONLINE) break;
    }
    CHECK_EQ_I(sent, sizeof dn);
    CHECK_EQ_I(raw_up_n, 700); CHECK_EQ_I(raw_dn_n, 3000);
    for (i = 0; i < raw_up_n; i++) if (raw_up[i] != (uint8_t)(i * 13 + 5)) bad++;
    for (i = 0; i < raw_dn_n; i++) if (raw_dn[i] != dn[i]) bad++;
    CHECK_EQ_I(bad, 0);
    s = status();
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->octets_up, 700); CHECK_EQ_I(s->octets_down, 3000);
    /* ended from our side */
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
    CHECK_EQ_I(status()->end_reason, DCE_END_LOCAL);
    CHECK_EQ_I(dce_raw_write(&D, dn, 10), 0);
}

/* The same with error control left on (raw_ec): a terminal program calling
 * a bulletin board through us (srv/bbs.h). LAPM is negotiated as for a PPP
 * call, the octets ride on it, PPP stays down. */
static void test_raw_ec(void)
{
    dce_cfg_t sc; caller_cfg_t cc; dce_status_t *s; static uint8_t dn[3000]; size_t i, sent = 0; int t, bad = 0;

    printf("(s) octets instead of PPP, with error control: a terminal program\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx; sc.raw_ec = 1;
    cc.raw_rx = raw_cli_rx; cc.raw_tx = raw_cli_tx;
    raw_up_n = raw_dn_n = raw_cli_at = raw_cli_n = 0;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_raw_online, 40000);
    CHECK(t > 0);
    s = status();
    CHECK_EQ_I(s->v42_state, V42EC_CONNECTED);
    CHECK_EQ_I(s->error_control, 1);
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_DEAD);
    for (i = 0; i < sizeof dn; i++) dn[i] = (uint8_t)(i * 7 + 1);
    raw_cli_n = 700;
    for (t = 0; t < 200000 && !p_raw_done(); t++) {
        if (sent < sizeof dn) sent += dce_raw_write(&D, dn + sent, sizeof dn - sent);
        step();
        if ((t & 7) == 7 && status()->state != DCE_ONLINE) break;
    }
    CHECK_EQ_I(sent, sizeof dn);
    CHECK_EQ_I(raw_up_n, 700); CHECK_EQ_I(raw_dn_n, 3000);
    for (i = 0; i < raw_up_n; i++) if (raw_up[i] != (uint8_t)(i * 13 + 5)) bad++;
    for (i = 0; i < raw_dn_n; i++) if (raw_dn[i] != dn[i]) bad++;
    CHECK_EQ_I(bad, 0);
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
    CHECK_EQ_I(status()->end_reason, DCE_END_LOCAL);
}

/* ------------------------------------------------------------------ (t) */
/* Text first, then PPP on the same call (dce_start_ppp): a menu goes out,
 * the caller answers "1", the owner switches; the caller's PPP client then
 * brings IP up over the same V.42 link. */
static const uint8_t *seq_tx; static size_t seq_n, seq_at;
static size_t seq_cli_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = 0; (void)ctx;
    while (n < max && seq_at < seq_n) b[n++] = seq_tx[seq_at++];
    return n;
}
static int p_menu_seen(void) { return raw_dn_n >= 10; }
static int p_one_seen(void) { return raw_up_n >= 1; }

static void test_text_then_ppp(void)
{
    static const uint8_t menu[] = "1) Start PPP\r\n", one[] = "1";
    dce_cfg_t sc; caller_cfg_t cc; size_t sent = 0; int t;

    printf("(t) text first, then PPP on the same call (dce_start_ppp)\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx; sc.raw_ec = 1;
    cc.raw_rx = raw_cli_rx; cc.raw_tx = seq_cli_tx;
    raw_up_n = raw_dn_n = 0; seq_tx = one; seq_n = 0; seq_at = 0;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_raw_online, 40000) > 0);
    CHECK_EQ_I(status()->raw, 1);
    CHECK_EQ_I(dce_start_ppp(&D), 0);               /* with nothing queued it switches at once */
    CHECK_EQ_I(status()->raw, 0);
    /* again from the start, this time with the menu */
    call_begin(&sc, &cc, &line_clean);
    raw_up_n = raw_dn_n = 0; seq_at = 0; seq_n = 0;
    CHECK(run_until(p_raw_online, 40000) > 0);
    while (sent < sizeof menu - 1) sent += dce_raw_write(&D, menu + sent, sizeof menu - 1 - sent);
    CHECK(run_until(p_menu_seen, 10000) >= 0);
    CHECK(memcmp(raw_dn, menu, sizeof menu - 1) == 0);
    seq_n = 1;                                      /* the caller types 1 */
    CHECK(run_until(p_one_seen, 10000) >= 0);
    CHECK_EQ_I(raw_up_n, 1); CHECK_EQ_I(raw_up[0], '1');
    CHECK_EQ_I(status()->ppp_phase, PPP_PHASE_DEAD);
    CHECK_EQ_I(dce_start_ppp(&D), 0);
    CHECK_EQ_I(dce_raw_write(&D, menu, 4), 0);      /* no more text once PPP is asked for */
    caller_to_ppp(&C);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    CHECK_EQ_I(status()->raw, 0);
    CHECK_EQ_I(status()->authenticated, 1);
    CHECK_EQ_I(status()->error_control, 1);         /* the same V.42 link */
    CHECK_EQ_I(raw_up_n, 1);                        /* nothing of PPP's reached raw_rx */
    CHECK_EQ_I(dce_start_ppp(&D), -1);              /* PPP already */
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
    CHECK_EQ_I(status()->end_reason, DCE_END_LOCAL);
}

/* ------------------------------------------------------------------ (u) */
/* ppp_detect: a Dial-Up Networking caller on a number that offers a menu
 * goes straight to PPP; the owner's raw_rx sees nothing of it. */
static void test_detect_dun(void)
{
    dce_cfg_t sc; caller_cfg_t cc; int t;

    printf("(u) PPP detected on an octet-mode call (ppp_detect)\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx; sc.raw_ec = 1; sc.ppp_detect = 1;
    raw_up_n = raw_dn_n = 0;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    CHECK_EQ_I(raw_up_n, 0);
    CHECK_EQ_I(status()->raw, 0);
    CHECK_EQ_I(status()->authenticated, 1);
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
}

/* ------------------------------------------------------------------ (v) */
/* ppp_detect with someone at a terminal: every octet reaches raw_rx as it
 * was sent, a "~" first included, and a lone "~" within DCE_DETECT_MS. */
static int p_two_seen(void) { return raw_up_n >= 2; }
static void test_detect_terminal(void)
{
    static const uint8_t tilde_x[] = "~x", tilde[] = "~";
    dce_cfg_t sc; caller_cfg_t cc; int t;

    printf("(v) a terminal user on an octet-mode call with ppp_detect\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx; sc.raw_ec = 1; sc.ppp_detect = 1;
    cc.raw_rx = raw_cli_rx; cc.raw_tx = seq_cli_tx;
    raw_up_n = raw_dn_n = 0; seq_tx = tilde_x; seq_n = 2; seq_at = 0;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_two_seen, 40000) > 0);
    CHECK_EQ_I(raw_up_n, 2);
    CHECK(memcmp(raw_up, "~x", 2) == 0);
    CHECK_EQ_I(status()->raw, 1);
    CHECK_EQ_I(status()->ppp_phase, PPP_PHASE_DEAD);
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);

    raw_up_n = raw_dn_n = 0; seq_tx = tilde; seq_n = 1; seq_at = 0;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_raw_online, 40000) > 0);
    CHECK(run_until(p_one_seen, 1500) < 0);         /* held while it may be PPP... */
    t = run_until(p_one_seen, 2000);                /* ...but not for long */
    CHECK(t >= 0);
    CHECK_EQ_I(raw_up_n, 1); CHECK_EQ_I(raw_up[0], '~');
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
}

/* ------------------------------------------------------------------ (w) */
/* ppp_detect, the watch: a PPP program that sends something else first (a
 * chat script's CR after CONNECT) is taken for a terminal, then its first
 * frame takes the call to PPP; with the watch off (dce_ppp_watch) it does
 * not, and its frames are the owner's like any octets. */
static int p_cr_seen(void) { return raw_up_n >= 1; }
static int p_many_seen(void) { return raw_up_n >= 40; }
static void test_detect_watch(void)
{
    static const uint8_t cr[] = "\r";
    dce_cfg_t sc; caller_cfg_t cc; int t;

    printf("(w) a PPP program that sends a CR first (ppp_detect, the watch)\n");
    server_cfg(&sc); client_cfg(&cc);
    sc.raw_rx = raw_srv_rx; sc.raw_ec = 1; sc.ppp_detect = 1;
    cc.raw_rx = raw_cli_rx; cc.raw_tx = seq_cli_tx;
    raw_up_n = raw_dn_n = 0; seq_tx = cr; seq_n = 1; seq_at = 0;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_cr_seen, 40000) > 0);
    CHECK_EQ_I(raw_up[0], '\r');                     /* a terminal's start: the owner's */
    CHECK_EQ_I(status()->raw, 1);
    caller_to_ppp(&C);
    t = run_until(p_ip_up, 60000);
    printf("    CR, then PPP: IP up %.2f s later; %d octets reached raw_rx\n", t / 1000.0, (int)raw_up_n);
    CHECK(t > 0);
    CHECK_EQ_I(status()->raw, 0);
    CHECK_EQ_I(status()->authenticated, 1);
    CHECK(raw_up_n <= 5);                           /* the CR and at most the frame's start */
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);

    /* the watch off: the same caller's frames stay octets */
    raw_up_n = raw_dn_n = 0; seq_at = 0;
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_cr_seen, 40000) > 0);
    dce_ppp_watch(&D, 0);
    caller_to_ppp(&C);
    CHECK(run_until(p_many_seen, 20000) > 0);
    CHECK_EQ_I(status()->raw, 1);
    CHECK_EQ_I(status()->ppp_phase, PPP_PHASE_DEAD);
    /* on again: its next frame (a retry) takes it to PPP */
    dce_ppp_watch(&D, 1);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    CHECK_EQ_I(status()->raw, 0);
    dce_hangup(&D);
    CHECK(run_until(p_ended, 10000) >= 0);
}

/* ------------------------------------------------------------------ (d) */
static void test_wrong_password(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    int t;

    printf("(d) wrong password\n");
    server_cfg(&sc); client_cfg(&cc);
    cc.ppp.pass = "not-the-password";
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ended, 60000);
    CHECK(t > 0);
    s = status();
    printf("    call ended after %.2f s: %s (PPP end %d, V.42 reason %d)\n", t / 1000.0,
           dce_end_name(s->end_reason), (int)s->ppp_end, s->v42_reason);
    CHECK_EQ_I(s->end_reason, DCE_END_AUTH_FAILED);
    CHECK_EQ_I(s->ppp_end, PPP_END_AUTH_FAILED);
    CHECK_EQ_I(s->authenticated, 0);
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    check_all_down();
    /* the ending was orderly: the client was told why before we hung up,
     * and its error control saw our release */
    CHECK_EQ_I(C.ppp.auth_failed, 1);
    CHECK_EQ_I(C.ppp.ip_up, 0);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_DISCONNECTED);
    CHECK_EQ_I(cv.reason, V42LAPM_R_PEER_DISC);
    CHECK_EQ_I(s->v42_reason, V42LAPM_R_LOCAL);
}

/* ------------------------------------------------------------------ (e) */
static void test_caller_hangs_up(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    printf("(e) caller hangs up in mid-session\n");
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    CHECK(traffic(TEXT, 3, 3, 30000, r));
    /* more data on its way down when the line goes dead */
    traffic(RANDOM, 6, 0, 1500, r);
    caller_hangup(&C);
    t = run_until(p_ended, 5000);
    CHECK(t > 0);
    s = status();
    printf("    server noticed %d ms after the carrier went: %s\n", t, dce_end_name(s->end_reason));
    CHECK(t < 1000);
    CHECK_EQ_I(s->end_reason, DCE_END_CARRIER_LOST);
    CHECK_EQ_I(s->ppp_end, PPP_END_LOWER_DOWN);
    check_all_down();
    /* what the call was is still on record */
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(s->bitrate, 2400);
    CHECK(strcmp(s->peer_name, "alice") == 0);
    CHECK(s->peer_ip == PEER_IP);
    CHECK(s->online_ms > 0 && s->call_ms > s->online_ms);
}

/* ------------------------------------------------------------------ (f) */
static void v21_call(int caller_v42)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    server_cfg(&sc); client_cfg(&cc);
    cc.modes = V8_MOD_V21;
    cc.use_v42 = caller_v42;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 90000);
    CHECK(t > 0);
    s = status();
    printf("    caller %s V.42: IP up %.2f s after answering\n", caller_v42 ? "with" : "without", t / 1000.0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->modulation, ANS_MOD_V21);
    CHECK_EQ_I(s->bitrate, 300);
    CHECK_EQ_I(s->v42_state, V42EC_ASYNC);
    CHECK_EQ_I(s->error_control, 0);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(s->peer_ip == PEER_IP);
    CHECK(traffic(RANDOM, 5, 5, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, 5); CHECK_EQ_I(F[UP].n_rx, 5);
    CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates("incompressible, both ways at once:", r, 300);
    CHECK_EQ_I(status()->state, DCE_ONLINE);
}

static void test_v21(void)
{
    printf("(f) V.21 300 bit/s\n");
    v21_call(0);
    if (!quick) v21_call(1);
}

/* ------------------------------------------------------------------ (g) */
static int p_cli_terminated(void) { return C.ppp.terminated; }
/* one retrain done at both ends, both in data mode again */
static int want_rt = 1;
static int p_retrained(void)
{
    return v90m__answer(D.modem)->retrains == want_rt && !v90m__answer(D.modem)->retraining && C.retrains == want_rt && C.stage == CALLER_ST_P34
        && C.p34.tx_ready && C.p34.rx_ready;
}

static void test_other_endings(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    long r[2];
    int t;

    printf("(g) other ways a call ends\n");

    /* the PPP client closes the link */
    server_cfg(&sc); client_cfg(&cc);
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    pppc_close(&C.ppp);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    PPP client closes: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    CHECK_EQ_I(s->end_reason, DCE_END_PEER_CLOSED);
    CHECK_EQ_I(s->ppp_end, PPP_END_PEER_TERMINATED);
    CHECK_EQ_I(C.ppp.terminated, 1);          /* it got our Terminate-Ack */
    check_all_down();

    /* we hang up */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    dce_hangup(&D);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    dce_hangup: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    CHECK_EQ_I(s->end_reason, DCE_END_LOCAL);
    CHECK_EQ_I(s->ppp_end, PPP_END_LOCAL_CLOSE);
    CHECK(run_until(p_cli_terminated, 10000) >= 0); /* the client was told */
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.reason, V42LAPM_R_PEER_DISC);
    check_all_down();

    /* dce_hangup while the modems are still training */
    call_begin(&sc, &cc, &line_clean);
    run_ms(1500);
    CHECK_EQ_I(status()->state, DCE_TRAINING);
    dce_hangup(&D);
    t = run_until(p_ended, 1000);
    CHECK(t >= 0 && t < 100);
    CHECK_EQ_I(status()->end_reason, DCE_END_LOCAL);
    check_all_down();

    /* the calling modem releases the error-corrected connection */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    v42ec_release(&C.v42);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    caller's LAPM releases: ended after %d ms: %s (V.42 reason %d)\n", t,
           dce_end_name(s->end_reason), s->v42_reason);
    CHECK_EQ_I(s->end_reason, DCE_END_V42_RELEASED);
    CHECK_EQ_I(s->v42_reason, V42LAPM_R_PEER_DISC);
    CHECK_EQ_I(s->ppp_end, PPP_END_LOWER_DOWN);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_DISCONNECTED);  /* our UA got out before the silence */
    CHECK_EQ_I(cv.reason, V42LAPM_R_LOCAL);
    check_all_down();

    /* the caller's error control goes deaf and dumb with the carrier still
     * up: our LAPM runs out of retransmissions */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 30000) > 0);
    C.v42_mute = 1;                            /* caller.c: line bits become mark */
    traffic(RANDOM, 4, 0, 100, r);
    t = run_until(p_ended, 120000);
    CHECK(t > 0);
    s = status();
    printf("    caller's LAPM stops answering: ended after %.1f s: %s (V.42 reason %d)\n", t / 1000.0,
           dce_end_name(s->end_reason), s->v42_reason);
    CHECK_EQ_I(s->end_reason, DCE_END_V42_FAILED);
    CHECK_EQ_I(s->v42_reason, V42LAPM_R_N400);
    check_all_down();

    /* nobody there at all */
    if (!quick) {
        call_begin(&sc, &cc, &line_no_echo);
        caller_hangup(&C);
        t = run_until(p_ended, 120000);
        CHECK(t > 0);
        s = status();
        printf("    silent line: ended after %.1f s: %s\n", t / 1000.0, dce_end_name(s->end_reason));
        CHECK_EQ_I(s->end_reason, DCE_END_NO_CARRIER);
        CHECK_EQ_I(s->modulation, ANS_MOD_NONE);
        check_all_down();
    }
}

/* =============================================================== V.90 */
static void show_rates2(const char *what, const long r[2], const dce_status_t *s)
{
    printf("    %-34s down %4ld octets/s (%3ld%%)   up %4ld octets/s (%3ld%%)\n", what,
           r[DN], r[DN] * 800 / s->bitrate, r[UP], r[UP] * 800 / s->rx_bitrate);
}

/* server with V.90 enabled, V.90 calling modem */
static void v90_cfgs(dce_cfg_t *sc, caller_cfg_t *cc)
{
    server_cfg(sc); client_cfg(cc);
    sc->modulations |= V90M_MOD_V90;
    cc->v90 = 1;
}

static int p_modem_up(void) { return status()->modem_connected; }

static void check_v90_online(const dce_status_t *s)
{
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->end_reason, DCE_END_NONE);
    CHECK_EQ_I(s->modem_connected, 1);
    CHECK_EQ_I(s->used_v8, 1);
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(s->bitrate, 50666);                 /* what the test caller's CP asks for */
    CHECK(s->rx_bitrate >= 19200 && s->rx_bitrate <= 33600);
    CHECK_EQ_I(s->rx_bitrate, C.tx_rate);
    CHECK_EQ_I(s->bitrate, C.rx_rate);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(s->our_ip == OUR_IP && s->peer_ip == PEER_IP);
}

/* ------------------------------------------------------------------ (h) */
static void test_v90_lapm(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    long r[2];
    unsigned long bt, br; uint32_t ms0;
    int t, tm, n = quick ? 30 : 200, nb = quick ? 20 : 150, nu;

    printf("(h) V.8 + V.90 + LAPM + V.44 + CHAP + IPCP\n");
    v90_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    tm = run_until(p_modem_up, 40000);
    CHECK(tm > 0);
    t = run_until(p_ip_up, 20000);
    CHECK(t >= 0);
    s = status();
    printf("    modem connected %.2f s after answering at %d bit/s down, %d bit/s up; IP up %.2f s later (%.2f s)\n",
           tm / 1000.0, s->bitrate, s->rx_bitrate, t / 1000.0, (tm + t) / 1000.0);
    check_v90_online(s);
    CHECK_EQ_I(s->v42_state, V42EC_CONNECTED);
    CHECK_EQ_I(s->error_control, 1);
    CHECK_EQ_I(s->comp_tx, 1);
    CHECK_EQ_I(s->comp_rx, 1);
    CHECK_EQ_I(s->comp_alg, V42EC_COMP_V44);           /* both ends have it: preferred */
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_NETWORK);
    CHECK(strcmp(s->peer_name, "alice") == 0);
    CHECK_EQ_I(C.ppp.auth, PPPC_AUTH_CHAP);
    CHECK(C.ppp.my_ip == PEER_IP && C.ppp.server_ip == OUR_IP && C.ppp.dns1 == DNS1);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_CONNECTED);
    CHECK(cv.comp_tx && cv.comp_rx);
    /* V.42 and PPP together must not take long at this speed */
    CHECK(t < 3000);

    /* the line bit counters follow the two different rates */
    bt = (unsigned long)s->line_bits_tx; br = (unsigned long)s->line_bits_rx; ms0 = s->call_ms;
    run_ms(2000);
    s = status();
    printf("    idle line, %u ms: %lu bits sent, %lu received\n", (unsigned)(s->call_ms - ms0),
           (unsigned long)s->line_bits_tx - bt, (unsigned long)s->line_bits_rx - br);
    CHECK_NEAR((double)((unsigned long)s->line_bits_tx - bt), s->bitrate * 2.0, s->bitrate * 0.01);
    CHECK_NEAR((double)((unsigned long)s->line_bits_rx - br), s->rx_bitrate * 2.0, s->rx_bitrate * 0.01);

    /* packets of assorted sizes, verified octet for octet on arrival */
    CHECK(traffic(RANDOM, n, 0, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[DN].n_bad, 0);
    show_rates2("incompressible, down only:", r, s);
    CHECK(r[DN] * 800 / s->bitrate >= 90);
    CHECK(traffic(RANDOM, 0, n, 120000, r));
    CHECK_EQ_I(F[UP].n_rx, n); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates2("incompressible, up only:", r, s);
    /* upstream some frames are damaged on the line and sent again
     * (test_findings.c, 6): LAPM hides it at a small cost */
    CHECK(r[UP] * 800 / s->rx_bitrate >= 80);
    /* both ways at once, each side offered as much as its rate carries in
     * the same time, so that both directions are loaded all the way */
    {
        int nd = n;
        nu = (int)((long)n * s->rx_bitrate / s->bitrate);
        n_busy[DN] = n_busy[UP] = 0;
        CHECK(traffic(RANDOM, nd, nu, 120000, r));
        CHECK_EQ_I(F[DN].n_rx, nd); CHECK_EQ_I(F[UP].n_rx, nu);
        CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
        show_rates2("incompressible, both ways at once:", r, s);
        printf("    (%ld + %ld octets; senders held off: down %ld times, up %ld times)\n",
               F[DN].bytes_rx, F[UP].bytes_rx, n_busy[DN], n_busy[UP]);
        /* (the quick run is too short to fill the caller's PPP buffer) */
        CHECK(n_busy[DN] > 100 && (quick || n_busy[UP] > 100));
        CHECK(r[DN] * 800 / s->bitrate >= 88);
        CHECK(r[UP] * 800 / s->rx_bitrate >= 80);
    }
    CHECK(traffic(TEXT, nb, nb, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, nb); CHECK_EQ_I(F[UP].n_rx, nb);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates2("text (V.44 at work), both ways:", r, s);
    CHECK(r[DN] * 8 > s->bitrate * 2 && r[UP] * 8 > s->rx_bitrate * 2);

    s = status();
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->ppp_counters.rx_bad_fcs, 0);
    CHECK_EQ_I(s->ppp_counters.tx_dropped, 0);
    CHECK_EQ_I(C.ppp.rx.bad_fcs, 0);
    CHECK(s->octets_down > 0 && s->octets_up > 0);
    printf("    LAPM: %lu I frames sent, %lu received, %lu retransmitted, %lu bad frames, %lu T401 expiries\n",
           s->lapm_tx_i, s->lapm_rx_i, s->lapm_retx, s->lapm_rx_bad, s->lapm_t401_expiries);
    v42ec_get_status(&C.v42, &cv);
    printf("    caller's LAPM: %lu I frames sent, %lu retransmitted, %lu bad frames, %lu T401 expiries\n",
           cv.tx_i, cv.tx_i_retx, cv.rx_frames_bad, cv.t401_expiries);
    /* downstream nothing is damaged, nothing is sent twice, and the
     * acknowledgement timer never runs out; what the upstream receiver
     * damages (lapm_rx_bad, the caller's retransmissions) is reported
     * above and held against the modem in test_findings.c */
    CHECK_EQ_I(s->lapm_retx, 0);
    CHECK_EQ_I(s->lapm_t401_expiries, 0);
    CHECK_EQ_I(cv.rx_frames_bad, 0);
    CHECK_EQ_I(s->ppp_counters.rx_ip, n + nu + nb);
    CHECK_EQ_I(s->ppp_counters.tx_ip, 2 * n + nb);
}

/* ------------------------------------------------------------------ (i) */
static void test_v90_no_error_control(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t, n = quick ? 20 : 100;

    printf("(i) V.90 caller without V.42: start-stop fallback over the synchronous stream\n");
    v90_cfgs(&sc, &cc);
    cc.use_v42 = 0;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    s = status();
    printf("    IP up %.2f s after answering\n", t / 1000.0);
    check_v90_online(s);
    CHECK_EQ_I(s->v42_state, V42EC_ASYNC);
    CHECK_EQ_I(s->error_control, 0);
    CHECK_EQ_I(s->comp_tx, 0);
    CHECK_EQ_I(s->comp_rx, 0);
    /* Downstream the test caller reads our octets as sent, so nothing may
     * go wrong there. Upstream the V.34 receiver makes occasional bit
     * errors even on this clean line (test_findings.c, 6), and without
     * error control each one costs a PPP frame: those packets are missing,
     * which is how an uncorrected link behaves. What does arrive must be
     * right. */
    traffic_lossy = 1;
    CHECK(traffic(RANDOM, n, 0, 200000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[DN].n_lost, 0);
    show_rates2("incompressible, down only:", r, s);
    /* ten line bits per octet, less PPP's own framing */
    CHECK(r[DN] * 10 <= s->bitrate && r[DN] * 1000 / s->bitrate >= 95);
    CHECK(traffic(RANDOM, 0, n, 200000, r));
    CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates2("incompressible, up only:", r, s);
    printf("    (up: %d of %d packets arrived, %d lost to line errors)\n", F[UP].n_rx, n, F[UP].n_lost);
    CHECK(F[UP].n_rx >= n * 8 / 10);
    CHECK(r[UP] * 10 <= s->rx_bitrate && r[UP] * 1000 / s->rx_bitrate >= 70);
    CHECK(traffic(RANDOM, n, n / 2, 200000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[DN].n_bad, 0); CHECK_EQ_I(F[DN].n_lost, 0);
    CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates2("incompressible, both ways at once:", r, s);
    printf("    (up: %d of %d packets arrived, %d lost to line errors)\n", F[UP].n_rx, n / 2, F[UP].n_lost);
    CHECK(F[UP].n_rx >= n / 2 * 8 / 10);
    CHECK(r[DN] * 10 <= s->bitrate && r[DN] * 1000 / s->bitrate >= 95);
    traffic_lossy = 0;
    s = status();
    printf("    server: %lu start-stop framing errors, %lu overruns, %lu PPP frames with a bad FCS\n",
           s->async_framing, s->async_overruns, (unsigned long)s->ppp_counters.rx_bad_fcs);
    CHECK_EQ_I(s->async_overruns, 0);
    CHECK_EQ_I(C.ppp.rx.bad_fcs, 0);               /* downstream: clean */
    CHECK_EQ_I(s->ppp_counters.tx_dropped, 0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
}

/* ------------------------------------------------------------------ (j) */
static int p_cli_p34(void) { return C.stage == CALLER_ST_P34; }

static void test_v90_caller_hangs_up(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    printf("(j) V.90: caller hangs up\n");
    v90_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    CHECK(traffic(TEXT, 3, 3, 30000, r));
    /* more data on its way down when the line goes dead */
    traffic(RANDOM, 40, 0, 1500, r);
    caller_hangup(&C);
    t = run_until(p_ended, 5000);
    CHECK(t > 0);
    s = status();
    printf("    in mid-session: server noticed %d ms after the carrier went: %s\n", t, dce_end_name(s->end_reason));
    CHECK(t < 1000);
    CHECK_EQ_I(s->end_reason, DCE_END_CARRIER_LOST);
    CHECK_EQ_I(s->ppp_end, PPP_END_LOWER_DOWN);
    check_all_down();
    /* what the call was is still on record */
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(s->bitrate, 50666);
    CHECK(s->rx_bitrate >= 19200);
    CHECK(strcmp(s->peer_name, "alice") == 0);
    CHECK(s->peer_ip == PEER_IP);
    CHECK(s->online_ms > 0 && s->call_ms > s->online_ms);

    if (quick) return;
    /* during the V.90 training (Phase 3): that is a failed start-up */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_cli_p34, 30000) > 0);
    run_ms(1000);
    CHECK_EQ_I(status()->state, DCE_TRAINING);
    CHECK_EQ_I(v90m__answer(D.modem)->phase, ANS_P3);
    caller_hangup(&C);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    during Phase 3: ended after %.1f s: %s (modem error %d)\n", t / 1000.0,
           dce_end_name(s->end_reason), v90m__answer(D.modem)->p34.error);
    CHECK_EQ_I(s->end_reason, DCE_END_NO_CARRIER);
    CHECK_EQ_I(s->modulation, ANS_MOD_NONE);
    CHECK_EQ_I(s->online_ms, 0);
    check_all_down();
}

/* ------------------------------------------------------------------ (k) */
static void test_v90_endings(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    int t;

    printf("(k) V.90: wrong password and other orderly endings\n");
    v90_cfgs(&sc, &cc);
    cc.ppp.pass = "not-the-password";
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ended, 60000);
    CHECK(t > 0);
    s = status();
    printf("    wrong password: call ended after %.2f s (%u ms after the modem connected): %s (PPP end %d, V.42 reason %d)\n",
           t / 1000.0, (unsigned)(s->call_ms - s->connect_ms), dce_end_name(s->end_reason),
           (int)s->ppp_end, s->v42_reason);
    CHECK_EQ_I(s->end_reason, DCE_END_AUTH_FAILED);
    CHECK_EQ_I(s->ppp_end, PPP_END_AUTH_FAILED);
    CHECK_EQ_I(s->authenticated, 0);
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    check_all_down();
    /* the ending was orderly: the client was told why before we hung up,
     * and its error control saw our release */
    CHECK_EQ_I(C.ppp.auth_failed, 1);
    CHECK_EQ_I(C.ppp.ip_up, 0);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_DISCONNECTED);
    CHECK_EQ_I(cv.reason, V42LAPM_R_PEER_DISC);
    CHECK_EQ_I(s->v42_reason, V42LAPM_R_LOCAL);
    /* and quick: not a matter of the closing sequence's time limits */
    CHECK(s->call_ms - s->connect_ms < 3000);
    if (quick) return;

    /* we hang up */
    v90_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    dce_hangup(&D);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    dce_hangup: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    CHECK(t < 1000);
    CHECK_EQ_I(s->end_reason, DCE_END_LOCAL);
    CHECK_EQ_I(s->ppp_end, PPP_END_LOCAL_CLOSE);
    CHECK(run_until(p_cli_terminated, 10000) >= 0); /* the client was told */
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.reason, V42LAPM_R_PEER_DISC);
    check_all_down();
    /* and so was its modem: a V.90 cleardown (MP with drn = 0), not just silence */
    CHECK_EQ_I(C.p34.clear_seen, 1);

    /* the calling modem clears down (its user hung up): V.90 9.7 */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    CHECK_EQ_I(p34a_renegotiate(&C.p34, 0, 1), 0);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    caller's modem clears down: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    CHECK(t < 1000);
    CHECK_EQ_I(s->end_reason, DCE_END_CLEARDOWN);
    check_all_down();

    /* the calling modem renegotiates the rate in the middle of the session:
     * the call goes on, at the new rate, and still carries traffic */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    {
        long r[2];
        int before = status()->bitrate;
        CHECK_EQ_I(p34a_renegotiate(&C.p34, C.p34.cfg.Kd - 2, 0), 0);
        run_ms(2000);
        s = status();
        printf("    caller renegotiates: %d -> %d bit/s down, state %d\n", before, s->bitrate, (int)s->state);
        CHECK_EQ_I(C.p34.renegs, 1);
        CHECK_EQ_I(s->state, DCE_ONLINE);
        CHECK_EQ_I(s->bitrate, before - 2 * 8000 / 6);
        {
            /* the stop is kept for the log: a renegotiation, its rates */
            dce_stop_t k;
            CHECK_EQ_I((int)dce_last_stop(&D, &k), 1);
            CHECK_EQ_I(k.retrain, 0);
            CHECK_EQ_I(k.cause, V90M_CAUSE_FAR);           /* the caller began it */
            CHECK_EQ_I(k.tx_before, before); CHECK_EQ_I(k.tx_after, before - 2 * 8000 / 6);
            CHECK(k.rx_before > 0 && k.rx_after > 0);
            CHECK(k.dur_ms > 0 && k.dur_ms < 2000);
            CHECK(k.since_ip_ms >= 0 && (uint32_t)k.since_ip_ms <= k.at_ms);
        }
        CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
        CHECK_EQ_I(status()->state, DCE_ONLINE);
    }

    /* the calling modem retrains (V.90 9.5.2.1): Phase 2 from the tones,
     * Phases 3 and 4 again; the session above must survive it */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    {
        long r[2];
        dce_stop_t k;
        CHECK_EQ_I((int)dce_last_stop(&D, &k), 0);     /* none yet in this call */
        CHECK_EQ_I(k.since_ip_ms, -1);
        caller_retrain(&C);
        t = run_until(p_retrained, 30000);
        run_ms(200);
        CHECK_EQ_I((int)dce_last_stop(&D, &k), 1);
        CHECK_EQ_I(k.retrain, 1);
        CHECK_EQ_I(k.cause, V90M_CAUSE_FAR);
        CHECK(k.dur_ms > 0 && k.since_ip_ms >= 0);
        s = status();
        printf("    caller retrains: data again after %d ms, state %d, %d bit/s down, %d up\n",
               t, (int)s->state, s->bitrate, s->rx_bitrate);
        CHECK(t > 0);
        CHECK_EQ_I(v90m__answer(D.modem)->retrains, 1);
        CHECK_EQ_I(s->state, DCE_ONLINE);
        CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
        CHECK_EQ_I(status()->state, DCE_ONLINE);
    }

    /* we retrain (9.5.1.1) */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    {
        long r[2];
        dce_stop_t k;
        CHECK_EQ_I(answer_retrain(v90m__answer(D.modem)), 0);
        t = run_until(p_retrained, 30000);
        printf("    we retrain: data again after %d ms\n", t);
        CHECK(t > 0);
        run_ms(200);
        CHECK_EQ_I((int)dce_last_stop(&D, &k), 1);
        CHECK_EQ_I(k.retrain, 1);
        CHECK(k.cause != V90M_CAUSE_FAR && k.cause != V90M_CAUSE_NONE);   /* ours */
        CHECK_EQ_I(C.retrains, 1);
        CHECK_EQ_I(status()->state, DCE_ONLINE);
        CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
        CHECK_EQ_I(status()->state, DCE_ONLINE);
    }

    /* a caller that retrains over and over is let go: the fourth in a row */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    for (want_rt = 1; want_rt <= 3; want_rt++) {
        caller_retrain(&C);
        CHECK(run_until(p_retrained, 30000) > 0);
    }
    want_rt = 1;
    CHECK_EQ_I(status()->state, DCE_ONLINE);
    caller_retrain(&C);
    t = run_until(p_ended, 5000);
    s = status();
    printf("    fourth retrain in a row: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    CHECK(t > 0);
    CHECK_EQ_I(s->end_reason, DCE_END_RETRAIN_FAILED);
    check_all_down();

    /* the calling modem releases the error-corrected connection: our UA
     * has to make it through the V.90 transmitter before we go silent */
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 60000) > 0);
    v42ec_release(&C.v42);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    caller's LAPM releases: ended after %d ms: %s (V.42 reason %d)\n", t,
           dce_end_name(s->end_reason), s->v42_reason);
    CHECK(t < 1000);
    CHECK_EQ_I(s->end_reason, DCE_END_V42_RELEASED);
    CHECK_EQ_I(s->v42_reason, V42LAPM_R_PEER_DISC);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_DISCONNECTED);
    CHECK_EQ_I(cv.reason, V42LAPM_R_LOCAL);
    check_all_down();
}

/* ------------------------------------------------------------------ (l) */
typedef struct { int t_ip; long r[2]; dce_status_t st; } v90_run_t;

static void v90_short_call(v90_run_t *o)
{
    dce_cfg_t sc; caller_cfg_t cc;

    v90_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    o->t_ip = run_until(p_ip_up, 60000);
    CHECK(o->t_ip > 0);
    CHECK(traffic(RANDOM, 20, 20, 60000, o->r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    CHECK(traffic(TEXT, 20, 20, 60000, o->r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    o->st = *status();
    check_v90_online(&o->st);
    CHECK_EQ_I(o->st.error_control, 1);
}

static void test_back_to_back(void)
{
    static v90_run_t ref, again;
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    printf("(l) V.22bis then V.90 then V.22bis on the same dce_t\n");
    /* reference: the V.90 call on an object full of rubbish */
    memset(&D, 0xA5, sizeof D);
    v90_short_call(&ref);

    /* a V.22bis call, ended by us, ... */
    server_cfg(&sc); client_cfg(&cc);
    sc.modulations |= V90M_MOD_V90;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 30000);
    CHECK(t > 0);
    s = status();
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(s->bitrate, 2400); CHECK_EQ_I(s->rx_bitrate, 2400);
    CHECK(traffic(TEXT, 5, 5, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    dce_hangup(&D);
    CHECK(run_until(p_ended, 30000) > 0);
    CHECK_EQ_I(status()->end_reason, DCE_END_LOCAL);
    printf("    V.22bis call: IP up after %.2f s, ended by dce_hangup\n", t / 1000.0);

    /* ... then the V.90 call again on the same object: with a simulation
     * that repeats exactly, nothing about it may differ from the reference */
    v90_short_call(&again);
    printf("    V.90 call: IP up after %.2f s (reference %.2f s), text down %ld up %ld octets/s (reference %ld, %ld)\n",
           again.t_ip / 1000.0, ref.t_ip / 1000.0, again.r[DN], again.r[UP], ref.r[DN], ref.r[UP]);
    CHECK_EQ_I(again.t_ip, ref.t_ip);
    CHECK_EQ_I(again.r[DN], ref.r[DN]); CHECK_EQ_I(again.r[UP], ref.r[UP]);
    CHECK_EQ_I(again.st.call_ms, ref.st.call_ms);
    CHECK_EQ_I(again.st.lapm_tx_i, ref.st.lapm_tx_i);
    CHECK_EQ_I(again.st.octets_down, ref.st.octets_down);
    CHECK_EQ_I(again.st.octets_up, ref.st.octets_up);
    CHECK(again.st.line_bits_tx == ref.st.line_bits_tx && again.st.line_bits_rx == ref.st.line_bits_rx);
    CHECK(memcmp(&again.st, &ref.st, sizeof ref.st) == 0);
    caller_hangup(&C);
    CHECK(run_until(p_ended, 5000) > 0);
    CHECK_EQ_I(status()->end_reason, DCE_END_CARRIER_LOST);

    /* and a V.22bis call after the V.90 one */
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 30000);
    CHECK(t > 0);
    s = status();
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(s->bitrate, 2400); CHECK_EQ_I(s->rx_bitrate, 2400);
    CHECK_EQ_I(s->error_control, 1);
    CHECK(traffic(RANDOM, 10, 10, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    CHECK(r[DN] > 250 && r[UP] > 250);
    printf("    V.22bis call after it: IP up after %.2f s, down %ld up %ld octets/s\n", t / 1000.0, r[DN], r[UP]);
}

/* ------------------------------------------------------------------ (m) */
static void test_v90_disabled(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t, n = 10;

    printf("(m) V.90 caller, V.90 disabled in our configuration: V.22bis\n");
    v90_cfgs(&sc, &cc);
    sc.modulations &= ~V90M_MOD_V90;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 40000);
    CHECK(t > 0);
    s = status();
    printf("    IP up %.2f s after answering at %d bit/s\n", t / 1000.0, s->bitrate);
    CHECK(C.v8.got_jm && !C.v8.jm.has_pcm);        /* our JM did not offer PCM */
    CHECK_EQ_I(C.stage, CALLER_ST_PEER);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->used_v8, 1);
    CHECK_EQ_I(s->modulation, ANS_MOD_V22BIS);
    CHECK_EQ_I(s->bitrate, 2400);
    CHECK_EQ_I(s->rx_bitrate, 2400);
    CHECK_EQ_I(s->error_control, 1);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(traffic(RANDOM, n, n, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates("incompressible, both ways at once:", r, 2400);
    CHECK(r[DN] > 250 && r[UP] > 250);
    CHECK_EQ_I(status()->state, DCE_ONLINE);
}

/* ------------------------------------------------------------------ (n) */
static void test_v90_alaw(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    printf("(n) V.90 with an A-law codec\n");
    v90_cfgs(&sc, &cc);
    sc.alaw = 1; cc.alaw = 1;
    harness_alaw = 1;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    s = status();
    printf("    IP up %.2f s after answering at %d bit/s down, %d bit/s up\n", t / 1000.0, s->bitrate, s->rx_bitrate);
    if (t > 0) {
        check_v90_online(s);
        CHECK(traffic(RANDOM, 30, 15, 60000, r));
        CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
        show_rates2("incompressible, both ways at once:", r, s);
        printf("    (frames damaged upstream: %lu; rate lowered %d times, now %d bit/s up; receiver error %d, limit %d, at connect SNR %d)\n", status()->lapm_rx_bad, v90m__answer(D.modem)->p34.rate_drops, v90m__answer(D.modem)->p34.up_rate, (int)v90m__answer(D.modem)->p34.up.rx.mse, (int)v90m__answer(D.modem)->p34.up.mse_limit, (int)v90m__answer(D.modem)->p34.snr_db10);
        CHECK(r[DN] * 800 / s->bitrate >= 88);
        caller_hangup(&C);
        CHECK(run_until(p_ended, 5000) > 0);
        CHECK_EQ_I(status()->end_reason, DCE_END_CARRIER_LOST);
        check_all_down();
    }
    harness_alaw = 0;
}

/* =============================================================== V.92 */
/* server with V.90 and V.92 enabled, V.92 calling modem (sim/v92call) */
static void v92_cfgs(dce_cfg_t *sc, caller_cfg_t *cc)
{
    server_cfg(sc); client_cfg(cc);
    sc->modulations |= V90M_MOD_V90 | V90M_MOD_V92;
    sc->max_up = 48000;
    cc->v92 = 1;
}

static int want_rt92 = 1;
static int p_retrained92(void)
{
    return v90m__answer(D.modem)->retrains == want_rt92 && !v90m__answer(D.modem)->retraining && C.vc.retrains == want_rt92
        && !C.vc.retraining && v92call_data(&C.vc) && status()->modem_connected;
}

static void check_v92_online(const dce_status_t *s)
{
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->end_reason, DCE_END_NONE);
    CHECK_EQ_I(s->modem_connected, 1);
    CHECK_EQ_I(s->used_v8, 1);
    CHECK_EQ_I(s->modulation, ANS_MOD_V92);
    CHECK_EQ_I(C.vc.stage, VCL_PCM);
    CHECK(s->rx_bitrate >= 24000 && s->rx_bitrate <= 48000);   /* V.92 5.2 */
    CHECK_EQ_I(s->rx_bitrate, C.tx_rate);
    CHECK_EQ_I(s->bitrate, C.rx_rate);
    CHECK_EQ_I(s->authenticated, 1);
    CHECK(s->our_ip == OUR_IP && s->peer_ip == PEER_IP);
}

/* ------------------------------------------------------------------ (o) */
static void test_v92_lapm(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    v42ec_status_t cv;
    long r[2];
    unsigned long bt, br; uint32_t ms0;
    int t, tm, n = quick ? 30 : 150, nb = quick ? 20 : 100, nu, up0;

    printf("(o) V.8 + V.92 (PCM upstream) + LAPM + V.44 + CHAP + IPCP\n");
    v92_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    tm = run_until(p_modem_up, 60000);
    CHECK(tm > 0);
    t = run_until(p_ip_up, 20000);
    CHECK(t >= 0);
    s = status();
    printf("    modem connected %.2f s after answering at %d bit/s down, %d bit/s up; IP up %.2f s later (%.2f s)\n",
           tm / 1000.0, s->bitrate, s->rx_bitrate, t / 1000.0, (tm + t) / 1000.0);
    check_v92_online(s);
    /* V.92 9.3.1: the caller sent no ODP, and it is LAPM all the same */
    CHECK_EQ_I(C.odp_bypass, 1);
    CHECK_EQ_I(answer_odp_bypass(v90m__answer(D.modem)), 1);
    CHECK_EQ_I(s->v42_state, V42EC_CONNECTED);
    CHECK_EQ_I(s->error_control, 1);
    CHECK_EQ_I(s->comp_tx, 1);
    CHECK_EQ_I(s->comp_rx, 1);
    CHECK_EQ_I(s->comp_alg, V42EC_COMP_V44);
    CHECK_EQ_I(s->ppp_phase, PPP_PHASE_NETWORK);
    CHECK(strcmp(s->peer_name, "alice") == 0);
    CHECK(C.ppp.my_ip == PEER_IP && C.ppp.server_ip == OUR_IP && C.ppp.dns1 == DNS1);
    v42ec_get_status(&C.v42, &cv);
    CHECK_EQ_I(cv.state, V42EC_CONNECTED);
    CHECK(t < 3000);
    up0 = s->rx_bitrate;

    /* the line bit counters follow the two different rates */
    bt = (unsigned long)s->line_bits_tx; br = (unsigned long)s->line_bits_rx; ms0 = s->call_ms;
    run_ms(2000);
    s = status();
    printf("    idle line, %u ms: %lu bits sent, %lu received\n", (unsigned)(s->call_ms - ms0),
           (unsigned long)s->line_bits_tx - bt, (unsigned long)s->line_bits_rx - br);
    CHECK_NEAR((double)((unsigned long)s->line_bits_tx - bt), s->bitrate * 2.0, s->bitrate * 0.01);
    CHECK_NEAR((double)((unsigned long)s->line_bits_rx - br), s->rx_bitrate * 2.0, s->rx_bitrate * 0.01);

    CHECK(traffic(RANDOM, n, 0, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[DN].n_bad, 0);
    show_rates2("incompressible, down only:", r, s);
    CHECK(r[DN] * 800 / s->bitrate >= 90);
    CHECK(traffic(RANDOM, 0, n, 120000, r));
    CHECK_EQ_I(F[UP].n_rx, n); CHECK_EQ_I(F[UP].n_bad, 0);
    show_rates2("incompressible, up only:", r, s);
    CHECK(r[UP] * 800 / s->rx_bitrate >= 88);
    nu = (int)((long)n * s->rx_bitrate / s->bitrate);
    CHECK(traffic(RANDOM, n, nu, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, nu);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates2("incompressible, both ways at once:", r, s);
    CHECK(r[DN] * 800 / s->bitrate >= 88);
    CHECK(r[UP] * 800 / s->rx_bitrate >= 85);
    CHECK(traffic(TEXT, nb, nb, 120000, r));
    CHECK_EQ_I(F[DN].n_rx, nb); CHECK_EQ_I(F[UP].n_rx, nb);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    show_rates2("text (V.44 at work), both ways:", r, s);
    CHECK(r[DN] * 8 > s->bitrate * 2 && r[UP] * 8 > s->rx_bitrate * 2);

    s = status();
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->ppp_counters.rx_bad_fcs, 0);
    CHECK_EQ_I(C.ppp.rx.bad_fcs, 0);
    v42ec_get_status(&C.v42, &cv);
    printf("    LAPM: %lu I frames sent, %lu received, %lu retransmitted, %lu bad frames, %lu T401 expiries\n",
           s->lapm_tx_i, s->lapm_rx_i, s->lapm_retx, s->lapm_rx_bad, s->lapm_t401_expiries);
    printf("    caller's LAPM: %lu I frames sent, %lu retransmitted, %lu bad frames, %lu T401 expiries\n",
           cv.tx_i, cv.tx_i_retx, cv.rx_frames_bad, cv.t401_expiries);
    /* a clean line: nothing damaged in either direction, nothing sent twice */
    CHECK_EQ_I(s->lapm_retx, 0);
    CHECK_EQ_I(s->lapm_rx_bad, 0);
    CHECK_EQ_I(s->lapm_t401_expiries, 0);
    CHECK_EQ_I(cv.rx_frames_bad, 0);
    CHECK_EQ_I(cv.tx_i_retx, 0);

    /* the caller renegotiates the rate (V.92 9.8.2): the call goes on */
    CHECK_EQ_I(v92call_renegotiate(&C.vc, 0), 0);
    run_ms(3000);
    s = status();
    printf("    caller renegotiates: %d bit/s down, %d up, state %d\n", s->bitrate, s->rx_bitrate, (int)s->state);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->modulation, ANS_MOD_V92);
    CHECK_EQ_I(v90m__answer(D.modem)->retrains, 0);
    CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);

    /* our receiver asks for less (as its bad frame watch does): a rate
     * renegotiation of ours that changes the upstream rate alone, and the
     * status follows it */
    v90m__answer(D.modem)->u.v92.cfg.max_up_rate = 30000;
    CHECK_EQ_I(v92d_renegotiate(&v90m__answer(D.modem)->u.v92), 0);
    run_ms(9000);       /* the CPd design is paced for the unit's processor (V92_DESIGN_PACE) */
    s = status();
    printf("    we renegotiate with a ceiling of 30000: %d bit/s down, %d up, state %d\n", s->bitrate, s->rx_bitrate, (int)s->state);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->bitrate, C.rx_rate);
    CHECK(s->rx_bitrate < up0 && s->rx_bitrate >= 24000 && s->rx_bitrate <= 30000);
    CHECK_EQ_I(s->rx_bitrate, C.tx_rate);
    CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);

    /* the caller retrains (V.92 9.7.2): Phase 2 and the V.92 Phases 3 and 4
     * again, and the session survives it */
    caller_retrain(&C);
    t = run_until(p_retrained92, 40000);
    s = status();
    printf("    caller retrains: data again after %d ms, state %d, %d bit/s down, %d up\n",
           t, (int)s->state, s->bitrate, s->rx_bitrate);
    CHECK(t > 0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->modulation, ANS_MOD_V92);
    CHECK_EQ_I(s->rx_bitrate, up0);
    CHECK_EQ_I(C.vc.pcm_starts, 2);
    CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);

    /* we retrain (9.7.1) */
    want_rt92 = 2;
    CHECK_EQ_I(answer_retrain(v90m__answer(D.modem)), 0);
    t = run_until(p_retrained92, 40000);
    s = status();
    printf("    we retrain: data again after %d ms, state %d, %d bit/s down, %d up\n",
           t, (int)s->state, s->bitrate, s->rx_bitrate);
    want_rt92 = 1;
    CHECK(t > 0);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->modulation, ANS_MOD_V92);
    CHECK_EQ_I(C.vc.pcm_starts, 3);
    CHECK(traffic(TEXT, 20, 20, 60000, r) > 0);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);

    /* the caller's line goes dead */
    caller_hangup(&C);
    v92call_hangup(&C.vc);
    t = run_until(p_ended, 5000);
    CHECK(t > 0);
    printf("    caller gone: ended after %d ms: %s\n", t, dce_end_name(status()->end_reason));
    CHECK_EQ_I(status()->end_reason, DCE_END_CARRIER_LOST);
    CHECK_EQ_I(status()->modulation, ANS_MOD_V92);  /* kept for the call log */
    check_all_down();
}

/* ------------------------------------------------------------------ (p) */
static void test_v92_other(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    printf("(p) V.92: A-law, endings, falling back to V.90\n");
    /* A-law codec */
    v92_cfgs(&sc, &cc);
    sc.alaw = 1; cc.alaw = 1;
    harness_alaw = 1;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 80000);
    CHECK(t > 0);
    s = status();
    printf("    A-law: IP up %.2f s after answering at %d bit/s down, %d bit/s up\n", t / 1000.0, s->bitrate, s->rx_bitrate);
    if (t > 0) {
        check_v92_online(s);
        CHECK(traffic(RANDOM, 30, 20, 60000, r));
        CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
        show_rates2("incompressible, both ways at once:", r, s);
        /* we hang up: the PPP and LAPM closing, then the V.92 cleardown (9.11) */
        dce_hangup(&D);
        t = run_until(p_ended, 30000);
        CHECK(t > 0);
        s = status();
        printf("    dce_hangup: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
        CHECK(t < 1000);
        CHECK_EQ_I(s->end_reason, DCE_END_LOCAL);
        check_all_down();
    }
    harness_alaw = 0;

    /* the calling modem clears down */
    v92_cfgs(&sc, &cc);
    call_begin(&sc, &cc, &line_clean);
    CHECK(run_until(p_ip_up, 80000) > 0);
    CHECK_EQ_I(v92call_renegotiate(&C.vc, 1), 0);
    t = run_until(p_ended, 30000);
    CHECK(t > 0);
    s = status();
    printf("    caller's modem clears down: ended after %d ms: %s\n", t, dce_end_name(s->end_reason));
    /* the whole rate renegotiation sequence comes before the CP with drn = 0
     * (V.92 9.11), our CPd design included, which is paced for the unit's
     * processor (V92_DESIGN_PACE): seconds, where V.90 needs one MP */
    CHECK(t < 8000);
    CHECK_EQ_I(s->end_reason, DCE_END_CLEARDOWN);
    check_all_down();

    /* the V.92 start-up fails (the caller's Phase 3 is not heard): a retrain
     * without PCM upstream, and the call is a V.90 call (V.92 9.5.1.2) */
    v92_cfgs(&sc, &cc);
    cc.v92_mute = 1;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 120000);
    CHECK(t > 0);
    s = status();
    printf("    failed V.92 start-up: IP up %.2f s after answering, %d bit/s down, %d up, modulation %d\n",
           t / 1000.0, s->bitrate, s->rx_bitrate, s->modulation);
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(C.vc.stage, VCL_V90);
    CHECK_EQ_I(C.vc.pcm_starts, 1);
    CHECK_EQ_I(C.vc.v90_starts, 1);
    CHECK(s->rx_bitrate <= 33600);
    CHECK_EQ_I(s->rx_bitrate, C.tx_rate);
    CHECK_EQ_I(s->error_control, 1);
    CHECK(traffic(RANDOM, 30, 15, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    CHECK_EQ_I(status()->state, DCE_ONLINE);

    /* a V.92 modem with PCM upstream switched off: V.90, and the ODP bypass
     * of 9.3.1 still applies (it follows from INFO0, not from INFO1a) */
    v92_cfgs(&sc, &cc);
    cc.v92 = 2;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 80000);
    CHECK(t > 0);
    s = status();
    printf("    V.92 caller, PCM upstream off: IP up %.2f s after answering, %d bit/s down, %d up\n",
           t / 1000.0, s->bitrate, s->rx_bitrate);
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(C.odp_bypass, 1);
    CHECK_EQ_I(s->error_control, 1);
    CHECK(traffic(RANDOM, 20, 10, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);

    /* V.92 not enabled here: the same caller gets V.90 and sends its ODP */
    v92_cfgs(&sc, &cc);
    sc.modulations &= ~V90M_MOD_V92;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 80000);
    CHECK(t > 0);
    s = status();
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(C.vc.d_v92, 0);
    CHECK_EQ_I(C.odp_bypass, 0);
    CHECK_EQ_I(answer_odp_bypass(v90m__answer(D.modem)), 0);
    CHECK_EQ_I(s->error_control, 1);

    /* callers without V.92 against the V.92-enabled server: the V.90 modem
     * of the tests above, and a V.34 modem */
    v90_cfgs(&sc, &cc);
    sc.modulations |= V90M_MOD_V92; sc.max_up = 48000;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    s = status();
    printf("    V.90 caller, V.92 enabled: IP up %.2f s after answering, %d bit/s down, %d up\n", t / 1000.0, s->bitrate, s->rx_bitrate);
    CHECK_EQ_I(s->modulation, ANS_MOD_V90);
    CHECK_EQ_I(s->bitrate, 50666);
    CHECK(s->rx_bitrate >= 19200 && s->rx_bitrate <= 33600);
    CHECK_EQ_I(answer_odp_bypass(v90m__answer(D.modem)), 0);
    CHECK_EQ_I(s->error_control, 1);
    CHECK(traffic(RANDOM, 20, 10, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);

    server_cfg(&sc); client_cfg(&cc);
    sc.modulations |= V90M_MOD_V34 | V90M_MOD_V90 | V90M_MOD_V92; sc.max_up = 48000;
    cc.v34 = 1;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    s = status();
    printf("    V.34 caller, V.92 enabled: IP up %.2f s after answering, %d bit/s down, %d up\n", t / 1000.0, s->bitrate, s->rx_bitrate);
    CHECK_EQ_I(s->modulation, ANS_MOD_V34);
    CHECK(s->rx_bitrate <= 33600);
    CHECK_EQ_I(s->error_control, 1);
    CHECK(traffic(RANDOM, 20, 10, 60000, r));
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
}

/* ------------------------------------------------------------------ (q) */
/* PPP compression (CCP): Deflate with a caller like pppd, MPPC with one
 * like Windows, over V.90 with and without the modem's own compression,
 * and over a line without error control where frames get lost. */
static const char *alg_name(unsigned a)
{
    return a == PPP_COMP_DEFLATE ? "Deflate" : a == PPP_COMP_MPPC ? "MPPC" : "none";
}

static long ccp_call(int ppp_comp, int link_comp, int client, int n, unsigned want, const char *what)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long r[2];
    int t;

    v90_cfgs(&sc, &cc);
    sc.ppp.comp = ppp_comp ? (PPP_COMP_DEFLATE | PPP_COMP_MPPC) : 0;
    if (!link_comp) {
        sc.v42.lapm.v42bis_tx = sc.v42.lapm.v42bis_rx = 0;
        sc.v42.lapm.v44_tx = sc.v42.lapm.v44_rx = 0;
    }
    cc.ppp.ccp = client;
    call_begin(&sc, &cc, &line_clean);
    t = run_until(p_ip_up, 60000);
    CHECK(t > 0);
    run_ms(500);                    /* CCP follows IPCP by a frame or two */
    s = status();
    CHECK_EQ_I(s->state, DCE_ONLINE);
    CHECK_EQ_I(s->comp_tx, link_comp);
    CHECK_EQ_I(s->ppp_comp_tx, want);
    CHECK_EQ_I(s->ppp_comp_rx, want);
    CHECK(traffic(HTML, n, n / 4, 400000, r));
    CHECK_EQ_I(F[DN].n_rx, n); CHECK_EQ_I(F[UP].n_rx, n / 4);
    CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    s = status();
    printf("    %-44s down %6ld octets/s (%4ld%% of the line rate)", what, r[DN], r[DN] * 800 / s->bitrate);
    if (want)
        printf("  PPP %s: %lu -> %lu octets",
               alg_name(s->ppp_counters.comp_tx), (unsigned long)s->ppp_counters.comp_tx_in,
               (unsigned long)s->ppp_counters.comp_tx_out);
    printf("\n");
    CHECK_EQ_I(s->ppp_counters.comp_rx_errors, 0);
    CHECK_EQ_I(C.ppp.comp_rx_bad, 0);
    CHECK_EQ_I(s->ppp_counters.tx_dropped, 0);
    if (want) {
        CHECK(s->ppp_counters.comp_tx_out * 2 < s->ppp_counters.comp_tx_in);
        CHECK(s->ppp_counters.comp_rx_in * 2 < s->ppp_counters.comp_rx_out);
        CHECK(C.ppp.comp_rx_ok >= n * 9 / 10);
    } else {
        CHECK_EQ_I(C.ppp.comp_rx_ok, 0);
    }
    CHECK_EQ_I(s->state, DCE_ONLINE);
    return r[DN];
}

static void test_ppp_compression(void)
{
    dce_cfg_t sc; caller_cfg_t cc;
    dce_status_t *s;
    long both, ppp_only, link_only, none, mppc, mppc_only, r[2];
    int n = quick ? 60 : 240, t, alg;

    printf("(q) PPP compression (CCP) over V.90 + LAPM, a download of markup-like text\n");
    none      = ccp_call(0, 0, PPPC_CCP_PPPD, n, 0, "no compression at all:");
    link_only = ccp_call(0, 1, PPPC_CCP_PPPD, n, 0, "V.44 only (PPP compression off):");
    ppp_only  = ccp_call(1, 0, PPPC_CCP_PPPD, n, PPP_COMP_DEFLATE, "Deflate only (V.42bis/V.44 off):");
    both      = ccp_call(1, 1, PPPC_CCP_PPPD, n, PPP_COMP_DEFLATE, "Deflate, and V.44 underneath:");
    mppc_only = ccp_call(1, 0, PPPC_CCP_WINDOWS, n, PPP_COMP_MPPC, "MPPC only (V.42bis/V.44 off):");
    mppc      = ccp_call(1, 1, PPPC_CCP_WINDOWS_MPPE, n, PPP_COMP_MPPC, "MPPC, and V.44 underneath:");
    CHECK(none * 8 <= status()->bitrate);
    /* each kind of compression is worth having on its own */
    CHECK(link_only > none * 2 && ppp_only > none * 2 && mppc_only > none * 2);
    /* Deflate with its 32 KiB window does better than the modem's
     * dictionary on this kind of data */
    CHECK(ppp_only > link_only);
    /* and the two together are not worse than PPP compression alone: the
     * link compressor has little left to find and must not cost anything
     * (3% allowed for where the last packet happens to end) */
    CHECK(both * 100 >= ppp_only * 97);
    CHECK(mppc * 100 >= mppc_only * 97);
    printf("    both on against link compression off: Deflate %+ld%%, MPPC %+ld%%\n",
           (both - ppp_only) * 100 / ppp_only, (mppc - mppc_only) * 100 / mppc_only);

    /* No error control (V.22bis, start-stop) on a line with noise bursts:
     * a burst costs the frame in flight, in either direction. The
     * decompressor that sees the gap asks for a reset and gets it; what
     * arrives is right and the call goes on compressed. */
    for (alg = 0; alg < 3; alg++) {
        static const line_t bursty = { -50.0, 15000, 3, 4000, 0.2 };
        int nn = quick ? 30 : 80;
        unsigned want = alg == 0 ? PPP_COMP_DEFLATE : alg == 1 ? PPP_COMP_MPPC : PPP_COMP_NONE;
        long t0;

        server_cfg(&sc); client_cfg(&cc);
        cc.use_v42 = 0;
        cc.ppp.ccp = alg ? PPPC_CCP_WINDOWS : PPPC_CCP_PPPD;
        if (alg == 2) sc.ppp.comp = 0;      /* for comparison: the same line uncompressed */
        /* a caller that keeps a short queue, as we do on such a line
         * (Linux queues three packets on a ppp interface) */
        cc.tx_busy_mark = 600;
        call_begin(&sc, &cc, &bursty);
        t = run_until(p_ip_up, 60000);
        CHECK(t > 0);
        run_ms(3000);
        s = status();
        CHECK_EQ_I(s->error_control, 0);
        CHECK_EQ_I(s->ppp_comp_rx, want);
        CHECK_EQ_I(s->ppp_comp_tx, want);
        bursts_on = 1;
        traffic_lossy = 40;
        /* a full frame takes more than six seconds here, and uncompressed
         * the transmit buffer holds 34 s of this line */
        traffic_idle_ms = 45000;
        t0 = now;
        CHECK(traffic(HTML, nn, nn, 1800000, r));
        t0 = ((F[DN].t_last > F[UP].t_last ? F[DN].t_last : F[UP].t_last) - t0) / FS;
        bursts_on = 0;
        traffic_lossy = 0;
        traffic_idle_ms = 3000;
        s = status();
        printf("    V.22bis without error control, noise bursts, %s: down %d of %d packets arrived, up %d of %d, in %ld s"
               " (%ld octets/s delivered down, %ld up)\n",
               alg_name(want), F[DN].n_rx, nn, F[UP].n_rx, nn, t0,
               F[DN].bytes_rx / (t0 ? t0 : 1), F[UP].bytes_rx / (t0 ? t0 : 1));
        if (want)
            printf("      server: %lu frames with a bad FCS, %lu decompressor errors (Reset-Requests), %lu Reset-Requests answered;"
                   " caller: %d and %d; CCP opened %lu time(s)\n",
                   (unsigned long)s->ppp_counters.rx_bad_fcs, (unsigned long)s->ppp_counters.comp_rx_errors,
                   (unsigned long)s->ppp_counters.comp_tx_resets, C.ppp.comp_rx_bad, C.ppp.resets_rcvd,
                   (unsigned long)s->ppp_counters.ccp_opens);
        CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
        CHECK(F[DN].n_rx >= nn / 2 && F[UP].n_rx >= nn / 2);
        if (want) {
            /* frames were lost both ways and both decompressors recovered */
            CHECK(s->ppp_counters.comp_rx_errors >= 1 && C.ppp.comp_rx_bad >= 1);
            CHECK(s->ppp_counters.comp_tx_resets >= 1);
        }
        CHECK_EQ_I(s->state, DCE_ONLINE);
        /* still compressing at the end, both ways */
        CHECK_EQ_I(s->ppp_comp_rx, want);
        CHECK(traffic(HTML, 4, 4, 120000, r));
        CHECK_EQ_I(F[DN].n_rx, 4); CHECK_EQ_I(F[UP].n_rx, 4);
        CHECK_EQ_I(F[DN].n_bad + F[UP].n_bad, 0);
    }
}

int main(int argc, char **argv)
{
    const char *only = argc > 2 ? argv[2] : argc > 1 && strcmp(argv[1], "quick") != 0 ? argv[1] : "";
#define RUN(letter, fn) do { if (!*only || strchr(only, letter)) fn(); } while (0)
    quick = argc > 1 && strcmp(argv[1], "quick") == 0;
    RUN('a', test_v22bis_lapm);
    RUN('b', test_noisy_line);
    RUN('c', test_no_error_control);
    RUN('r', test_raw);
    RUN('s', test_raw_ec);
    RUN('t', test_text_then_ppp);
    RUN('u', test_detect_dun);
    RUN('v', test_detect_terminal);
    RUN('w', test_detect_watch);
    RUN('d', test_wrong_password);
    RUN('e', test_caller_hangs_up);
    RUN('f', test_v21);
    RUN('g', test_other_endings);
    RUN('h', test_v90_lapm);
    if (!quick) {
        RUN('i', test_v90_no_error_control);
        RUN('j', test_v90_caller_hangs_up);
        RUN('k', test_v90_endings);
        RUN('l', test_back_to_back);
    }
    RUN('m', test_v90_disabled);
    if (!quick) RUN('n', test_v90_alaw);
    RUN('o', test_v92_lapm);
    if (!quick) RUN('p', test_v92_other);
    RUN('q', test_ppp_compression);
    T_REPORT(quick ? "call (quick)" : "call");
}
