/* Interop against the system ppp package (host only, not part of
 * `make test`; nothing here is shipped).
 *
 * Part 1, pppdump: a complete session between our server and the test
 * client is written in pppd's "record" format and handed to pppdump -p,
 * an independent implementation of the RFC 1662 deframer and FCS check.
 * Every frame must come out without an FCS complaint and with exactly the
 * content our own decoder sees.
 *
 * Part 2, pppd: the real pppd binary runs as the calling peer against our
 * server. pppd cannot open /dev/ppp without root, so it runs inside an
 * unprivileged user namespace with test/pppd_shim.c preloaded in place of
 * the kernel side. Its LCP, CHAP/PAP and IPCP state machines are the real
 * ones. Packets travel over a SOCK_SEQPACKET pair; this harness does the
 * HDLC framing towards our server (so pppd-side framing is not tested
 * here; that is what part 1 is for, in the transmit direction).
 *
 * Usage: interop_pppd <build-dir>   (needs <build-dir>/pppd_shim.so) */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "test.h"
#include "ppp.h"
#include "pppc.h"

#define OUR_IP   PPP_IP4(10, 64, 0, 1)
#define PEER_IP  PPP_IP4(10, 64, 0, 7)
#define DNS1     PPP_IP4(10, 64, 0, 53)
#define DNS2     PPP_IP4(1, 1, 1, 1)

static const ppp_user_t users[] = { { "alice", "wonderland" }, { "bob", "hunter2" } };
static const char *build_dir = "build";

static ppp_t S;
static ppp_status_t st;
static ppp_status_t *status(void)
{
    ppp_get_status(&S, &st);
    return &st;
}

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
    c->seed = (uint32_t)time(NULL) ^ ((uint32_t)getpid() << 16);
}

/* ================= part 1: pppdump ================= */

static pppc_t C;
static FILE *rec;
#define MAXPK 400
static struct { int dir; size_t n; uint8_t d[1510]; } mine[MAXPK];
static int n_mine;
static ppp_hdlc_rx_t dec[2];

static void record(int dir, const uint8_t *b, size_t n)
{
    /* record format: tag 1 = sent, 2 = received; 16-bit length; data */
    fputc(dir == 0 ? 1 : 2, rec);
    fputc((int)(n >> 8), rec);
    fputc((int)(n & 0xff), rec);
    fwrite(b, 1, n, rec);
    for (size_t i = 0; i < n; i++) {
        if (ppp_hdlc_rx_byte(&dec[dir], b[i]) && n_mine < MAXPK) {
            mine[n_mine].dir = dir;
            mine[n_mine].n = dec[dir].len;
            memcpy(mine[n_mine].d, dec[dir].buf, dec[dir].len);
            n_mine++;
        }
    }
}

static void shuttle(void)
{
    uint8_t buf[300];
    size_t n;
    int busy;
    do {
        busy = 0;
        while ((n = ppp_output_bytes(&S, buf, sizeof buf)) > 0) {
            record(0, buf, n);
            pppc_input(&C, buf, n);
            busy = 1;
        }
        while ((n = pppc_output(&C, buf, sizeof buf)) > 0) {
            record(1, buf, n);
            ppp_input_bytes(&S, buf, n);
            busy = 1;
        }
    } while (busy);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static void part1_pppdump(void)
{
    ppp_config_t sc;
    pppc_config_t cc;
    char path[300], cmd[700], line[400];
    static uint8_t pkt[1500];
    FILE *p;
    int n_theirs = 0, bad = 0, cur = -1, mismatch = 0;
    size_t cur_n = 0;
    static uint8_t cur_d[1600];
    static const uint32_t maps[] = { 0, 0xffffffffu, 0x000a0000u };

    snprintf(path, sizeof path, "%s/interop_session.rec", build_dir);
    rec = fopen(path, "wb");
    CHECK(rec != NULL);
    if (!rec) return;
    fputc(7, rec); fputc(0, rec); fputc(0, rec); fputc(0, rec); fputc(1, rec);
    ppp_hdlc_rx_init(&dec[0], PPP_HDLC_BUF);
    ppp_hdlc_rx_init(&dec[1], PPP_HDLC_BUF);
    dec[0].rx_accm = dec[1].rx_accm = 0;

    for (int m = 0; m < 3; m++) {
        server_cfg(&sc);
        sc.accm = maps[m];
        memset(&cc, 0, sizeof cc);
        cc.user = "alice"; cc.pass = "wonderland";
        cc.accept_auth = m == 1 ? PPPC_AUTH_PAP : PPPC_AUTH_CHAP;
        cc.send_accm = 1; cc.accm = maps[(m + 1) % 3];
        cc.pfc = cc.acfc = (m != 2);
        sc.no_pfc = sc.no_acfc = (m == 2);
        cc.magic = 0x1badcafe; cc.want_dns = 1; cc.want_vj = 1;
        ppp_init(&S, &sc);
        pppc_init(&C, &cc);
        ppp_lower_up(&S);
        pppc_start(&C);
        shuttle();
        CHECK_EQ_I(status()->ip_up, 1);
        for (int k = 0; k < 6; k++) {
            size_t len = k == 0 ? 1500 : (size_t)(40 + 291 * k);
            for (size_t i = 0; i < len; i++)
                pkt[i] = k == 1 ? 0x7e : k == 2 ? 0x7d : (uint8_t)(i * (k + 1));
            CHECK_EQ_I(ppp_send_ip(&S, pkt, len), 0);
            shuttle();
            CHECK_EQ_I(pppc_send_ip(&C, pkt, len), 0);
            shuttle();
        }
        ppp_close(&S);
        shuttle();
    }
    fclose(rec);
    CHECK(n_mine > 60 && n_mine < MAXPK);

    /* -m 1510: do not let pppdump call our full-size frames over-length */
    snprintf(cmd, sizeof cmd, "pppdump -p -m 1510 %s 2>&1", path);
    p = popen(cmd, "r");
    CHECK(p != NULL);
    if (!p) return;
    /* output: "sent  hh hh ... (16 per line)  ascii", continuation lines
     * indented; anything else is a note such as "BAD FCS" */
    for (;;) {
        char *got = fgets(line, sizeof line, p);
        int is_pkt = got && (!strncmp(line, "sent  ", 6) || !strncmp(line, "rcvd  ", 6));
        int is_cont = got && !strncmp(line, "      ", 6) && hexval(line[6]) >= 0 &&
                      hexval(line[7]) >= 0 && cur >= 0;
        if (cur >= 0 && !is_cont) {
            /* packet complete: compare with ours */
            if (n_theirs < n_mine) {
                if (mine[n_theirs].dir != cur || mine[n_theirs].n != cur_n ||
                    memcmp(mine[n_theirs].d, cur_d, cur_n) != 0)
                    mismatch++;
            }
            n_theirs++;
            cur = -1;
        }
        if (!got)
            break;
        if (is_pkt) {
            cur = line[0] == 's' ? 0 : 1;
            cur_n = 0;
        }
        if (is_pkt || is_cont) {
            for (int col = 6; col < 6 + 48 && cur_n < sizeof cur_d; col += 3) {
                int h = hexval(line[col]), l = h >= 0 ? hexval(line[col + 1]) : -1;
                if (h < 0 || l < 0) break;
                cur_d[cur_n++] = (uint8_t)(h * 16 + l);
            }
        } else if (strncmp(line, "start", 5) && strncmp(line, "time", 4) &&
                   strncmp(line, "end", 3)) {
            bad++;
            printf("  pppdump says: %s", line);
        }
    }
    pclose(p);
    printf("pppdump: %d packets decoded, ours %d, %d complaints, %d content mismatches\n",
           n_theirs, n_mine, bad, mismatch);
    CHECK_EQ_I(n_theirs, n_mine);
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(mismatch, 0);
}

/* ================= part 2: pppd ================= */

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000);
}

static int sock = -1;
static pid_t pppd_pid = -1;
static ppp_hdlc_rx_t sdec;
static int carrier;
static int n_from_pppd, n_to_pppd, pppd_echo_requests, pppd_echo_replies;

static int start_pppd(const char *label, const char *const *extra)
{
    int sv[2], master;
    char shim[400], fdenv[32], logpath[400];
    const char *argv[64];
    int n = 0;
    char *pts;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0)
        return -1;
    /* pppd insists on a serial device; give it a pty it never really uses */
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0)
        return -1;
    pts = ptsname(master);
    snprintf(shim, sizeof shim, "%s/pppd_shim.so", build_dir);
    snprintf(logpath, sizeof logpath, "%s/interop_pppd_%s.log", build_dir, label);
    snprintf(fdenv, sizeof fdenv, "%d", sv[1]);

    argv[n++] = "unshare"; argv[n++] = "-Urn";
    argv[n++] = "pppd"; argv[n++] = pts; argv[n++] = "115200";
    argv[n++] = "nodetach"; argv[n++] = "debug"; argv[n++] = "logfd"; argv[n++] = "2";
    argv[n++] = "noauth"; argv[n++] = "local"; argv[n++] = "nocrtscts"; argv[n++] = "nolock";
    argv[n++] = "nodefaultroute"; argv[n++] = "noipdefault"; argv[n++] = "usepeerdns";
    argv[n++] = "noccp"; argv[n++] = "noipv6"; argv[n++] = "nopersist";
    argv[n++] = "ip-up-script"; argv[n++] = "/bin/true";
    argv[n++] = "ip-down-script"; argv[n++] = "/bin/true";
    while (*extra && n < 62) argv[n++] = *extra++;
    argv[n] = NULL;

    pppd_pid = fork();
    if (pppd_pid < 0)
        return -1;
    if (pppd_pid == 0) {
        int lf = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        int nul = open("/dev/null", O_RDONLY);
        close(sv[0]);
        dup2(nul, 0);
        dup2(lf, 1);
        dup2(lf, 2);
        setenv("PPP_SHIM_FD", fdenv, 1);
        setenv("LD_PRELOAD", shim, 1);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(sv[1]);
    sock = sv[0];
    ppp_hdlc_rx_init(&sdec, PPP_HDLC_BUF);
    sdec.rx_accm = 0;
    carrier = 0;
    n_from_pppd = n_to_pppd = pppd_echo_requests = pppd_echo_replies = 0;
    return master;
}

/* one pass: packets from pppd into the server, time, server output to pppd */
static void pump(uint32_t *last)
{
    struct pollfd pfd = { sock, POLLIN, 0 };
    uint8_t pk[1600], wire[3300];
    uint32_t t;
    size_t n;

    if (poll(&pfd, 1, 10) > 0 && (pfd.revents & POLLIN)) {
        ssize_t r = recv(sock, pk + 2, sizeof pk - 2, MSG_DONTWAIT);
        if (r >= 2) {
            /* pppd hands the "kernel" a packet starting at the protocol
             * field; frame it as the kernel would */
            if (!carrier) {
                carrier = 1;
                ppp_lower_up(&S);
            }
            n_from_pppd++;
            if (pk[2] == 0xc0 && pk[3] == 0x21 && r >= 3) {
                if (pk[4] == 9) pppd_echo_requests++;
                if (pk[4] == 10) pppd_echo_replies++;
            }
            pk[0] = 0xff;
            pk[1] = 0x03;
            n = ppp_hdlc_encode(wire, sizeof wire, pk, (size_t)r + 2, NULL, 0, 0xffffffffu, 1);
            ppp_input_bytes(&S, wire, n);
        }
    }
    t = now_ms();
    if (carrier && t != *last)
        ppp_tick(&S, t - *last);
    *last = t;
    while ((n = ppp_output_bytes(&S, wire, sizeof wire)) > 0) {
        for (size_t i = 0; i < n; i++) {
            uint16_t proto;
            size_t off;
            if (!ppp_hdlc_rx_byte(&sdec, wire[i]))
                continue;
            if (ppp_hdlc_parse(sdec.buf, sdec.len, 1, 1, &proto, &off) != 0)
                continue;
            pk[0] = (uint8_t)(proto >> 8);
            pk[1] = (uint8_t)proto;
            memcpy(pk + 2, sdec.buf + off, sdec.len - off);
            if (send(sock, pk, 2 + sdec.len - off, MSG_DONTWAIT | MSG_NOSIGNAL) > 0)
                n_to_pppd++;
        }
    }
}

/* pump until cond() holds or the timeout; returns ms taken or -1 */
static int pump_until(int (*cond)(void), uint32_t timeout_ms)
{
    uint32_t t0 = now_ms(), last = t0;
    for (;;) {
        pump(&last);
        if (cond && cond())
            return (int)(now_ms() - t0);
        if (now_ms() - t0 >= timeout_ms)
            return -1;
    }
}

static int cond_ip_up(void) { return status()->ip_up; }
static int cond_dead(void) { return carrier && status()->phase == PPP_PHASE_DEAD; }
static int pppd_status = -1;
static int cond_pppd_exited(void)
{
    int ws;
    if (pppd_pid > 0 && waitpid(pppd_pid, &ws, WNOHANG) == pppd_pid) {
        pppd_status = WIFEXITED(ws) ? WEXITSTATUS(ws) : 1000 + WTERMSIG(ws);
        pppd_pid = -1;
    }
    return pppd_pid < 0;
}

static void finish_pppd(int master)
{
    if (pump_until(cond_pppd_exited, 8000) < 0 && pppd_pid > 0) {
        kill(pppd_pid, SIGKILL);
        waitpid(pppd_pid, NULL, 0);
        pppd_pid = -1;
        pppd_status = -2;
    }
    close(sock);
    close(master);
}

static void show_log(const char *label, const char *pattern)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "grep -E '%s' %s/interop_pppd_%s.log | sed 's/^/    pppd| /'",
             pattern, build_dir, label);
    fflush(stdout);
    if (system(cmd) != 0)
        printf("    (no matching pppd log lines)\n");
}

static int log_has(const char *label, const char *text)
{
    char cmd[600];
    snprintf(cmd, sizeof cmd, "grep -qF '%s' %s/interop_pppd_%s.log", text, build_dir, label);
    return system(cmd) == 0;
}

static int part2_pppd(void)
{
    ppp_config_t sc;
    int master, t;

    /* ---- CHAP, echo both ways, we close ---- */
    {
        static const char *const args[] = { "user", "alice", "password", "wonderland",
            "lcp-echo-interval", "1", "lcp-echo-failure", "3", NULL };
        printf("-- pppd run 1: CHAP-MD5, keepalive both ways, server closes\n");
        server_cfg(&sc);
        sc.echo_interval_ms = 1000;
        ppp_init(&S, &sc);
        master = start_pppd("chap", args);
        if (master < 0) { printf("cannot start pppd\n"); return -1; }
        t = pump_until(cond_ip_up, 10000);
        if (t < 0 && !carrier) {
            /* pppd never spoke: it could not run at all in this setup */
            finish_pppd(master);
            printf("pppd produced no packets (exit status %d). Its log:\n", pppd_status);
            show_log("chap", ".");
            return -1;
        }
        CHECK(t >= 0);
        printf("   IP up after %d ms; %d packets from pppd, %d to pppd\n", t, n_from_pppd, n_to_pppd);
        CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
        CHECK_EQ_I(status()->auth, PPP_AUTH_CHAP);
        CHECK_EQ_I(status()->authenticated, 1);
        CHECK(strcmp(status()->peer_name, "alice") == 0);
        CHECK(status()->peer_ip == PEER_IP);
        CHECK_EQ_I(status()->ipcp_state, PPP_FSM_OPENED);
        CHECK(status()->tx_accm == 0);          /* pppd's default asyncmap */
        /* stay up 5 s: pppd's echoes are answered by us (or it would hang
         * up after 3), ours by pppd (or we would) */
        CHECK(pump_until(NULL, 5000) < 0);
        CHECK_EQ_I(status()->ip_up, 1);
        CHECK_EQ_I(status()->end_reason, PPP_END_NONE);
        CHECK(pppd_echo_requests >= 3);
        CHECK(pppd_echo_replies >= 3);
        printf("   after 5 s: still up; pppd sent %d Echo-Requests and %d Echo-Replies\n",
               pppd_echo_requests, pppd_echo_replies);
        ppp_close(&S);
        t = pump_until(cond_dead, 8000);
        CHECK(t >= 0 && t < 2000);              /* pppd Ack'd our Terminate */
        CHECK_EQ_I(status()->end_reason, PPP_END_LOCAL_CLOSE);
        finish_pppd(master);
        printf("   closed in %d ms; pppd exit status %d\n", t, pppd_status);
        CHECK(log_has("chap", "CHAP authentication succeeded"));
        CHECK(log_has("chap", "local  IP address 10.64.0.7"));
        CHECK(log_has("chap", "remote IP address 10.64.0.1"));
        CHECK(log_has("chap", "primary   DNS address 10.64.0.53"));
        CHECK(log_has("chap", "secondary DNS address 1.1.1.1"));
        show_log("chap", "CHAP|IP address|DNS address|terminated|Terminat|rcvd \\[LCP Conf|sent \\[LCP Conf|IPCP Conf");
    }
    /* ---- PAP by fallback, pppd closes ---- */
    {
        static const char *const args[] = { "user", "bob", "password", "hunter2",
            "refuse-chap", "refuse-mschap", "refuse-mschap-v2", "refuse-eap", NULL };
        printf("-- pppd run 2: pppd refuses CHAP, fallback to PAP, pppd closes (SIGTERM)\n");
        server_cfg(&sc);
        ppp_init(&S, &sc);
        master = start_pppd("pap", args);
        if (master < 0) return -1;
        t = pump_until(cond_ip_up, 10000);
        CHECK(t >= 0);
        printf("   IP up after %d ms\n", t);
        CHECK_EQ_I(status()->auth, PPP_AUTH_PAP);
        CHECK(strcmp(status()->peer_name, "bob") == 0);
        CHECK(status()->peer_ip == PEER_IP);
        if (pppd_pid > 0) {
            /* unshare execs pppd directly, so this signal reaches pppd */
            kill(pppd_pid, SIGTERM);
        }
        t = pump_until(cond_dead, 10000);
        CHECK(t >= 0);
        CHECK_EQ_I(status()->end_reason, PPP_END_PEER_TERMINATED);
        finish_pppd(master);
        printf("   peer terminated, Dead after %d ms; pppd exit status %d\n", t, pppd_status);
        CHECK_EQ_I(pppd_status, 5);             /* pppd: "terminated by signal" */
        CHECK(log_has("pap", "PAP authentication succeeded"));
        CHECK(log_has("pap", "local  IP address 10.64.0.7"));
        show_log("pap", "PAP|auth|IP address|Term");
    }
    /* ---- wrong password ---- */
    {
        static const char *const args[] = { "user", "alice", "password", "queen-of-hearts", NULL };
        printf("-- pppd run 3: wrong CHAP secret\n");
        server_cfg(&sc);
        ppp_init(&S, &sc);
        master = start_pppd("badpw", args);
        if (master < 0) return -1;
        t = pump_until(cond_dead, 15000);
        CHECK(t >= 0);
        CHECK_EQ_I(status()->end_reason, PPP_END_AUTH_FAILED);
        CHECK_EQ_I(status()->authenticated, 0);
        CHECK_EQ_I(status()->ip_up, 0);
        finish_pppd(master);
        printf("   Dead after %d ms; pppd exit status %d\n", t, pppd_status);
        CHECK_EQ_I(pppd_status, 19);            /* "failed to authenticate ourselves to the peer" */
        CHECK(log_has("badpw", "CHAP authentication failed"));
        show_log("badpw", "CHAP|auth|Term");
    }
    /* ---- no authentication, Van Jacobson requested by pppd ---- */
    {
        static const char *const args[] = { "vj-max-slots", "16", NULL };
        printf("-- pppd run 4: server without authentication; pppd asks for VJ compression\n");
        server_cfg(&sc);
        sc.auth = PPP_AUTH_NONE;
        ppp_init(&S, &sc);
        master = start_pppd("noauth", args);
        if (master < 0) return -1;
        t = pump_until(cond_ip_up, 10000);
        CHECK(t >= 0);
        printf("   IP up after %d ms\n", t);
        CHECK_EQ_I(status()->auth, PPP_AUTH_NONE);
        CHECK(status()->peer_ip == PEER_IP);
        ppp_close(&S);
        CHECK(pump_until(cond_dead, 8000) >= 0);
        finish_pppd(master);
        CHECK(log_has("noauth", "rcvd [IPCP ConfRej"));
        CHECK(log_has("noauth", "<compress VJ"));
        show_log("noauth", "IPCP");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        build_dir = argv[1];
    if (system("which pppdump >/dev/null 2>&1") == 0)
        part1_pppdump();
    else
        printf("pppdump not found: framing cross-check SKIPPED\n");
    if (system("which pppd >/dev/null 2>&1") != 0 || system("unshare -Urn true 2>/dev/null") != 0)
        printf("pppd or unprivileged user namespaces not available: pppd run SKIPPED\n");
    else if (part2_pppd() != 0)
        printf("pppd run could not be carried out (see above)\n");
    T_REPORT("interop_pppd");
}
