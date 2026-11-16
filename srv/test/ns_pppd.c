/* The real daemon against the real pppd: PPP compression with a peer that
 * was not written here.
 *
 *   sudo unshare -n -m ns_pppd PATH-TO-v90d SCRATCH-DIR
 *
 * pppd needs the kernel's PPP driver (/dev/ppp, and ppp_deflate for the
 * compression), which only root in the initial user namespace may open;
 * so, unlike ns_call, this runs as root, in a network and a mount
 * namespace of its own (see the Makefile, which leaves the test out where
 * that cannot be had). Nothing of the machine's own PPP set-up is touched:
 * /etc/ppp and /run are covered with empty file systems first, so that
 * pppd reads no options or secrets of the system, runs none of its
 * scripts and writes its pid file nowhere that matters.
 *
 * The picture: this program starts v90d with a socket pair as its line
 * and plays the caller's telephone and modem (a V.90 call, LAPM) as
 * ns_call does. Where ns_call has its own PPP client, the octets of the
 * data connection here go to pppd, which runs in a second network
 * namespace: the far end of the telephone line. A file is then fetched and
 * another one sent over TCP between the two namespaces, through pppd's
 * ppp0, the kernel's Deflate, the modems, the daemon's PPP and its TUN
 * device, and compared octet by octet.
 *
 * Simulated time runs four times as fast as the clock at most: pppd and
 * the kernel's TCP keep their own time. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <linux/capability.h>
#include "test.h"
#include "config.h"
#include "caller.h"
#include "channel.h"
#include "gen.h"

#define FS 8000
#define BLK 80
#define SPEED 4                 /* simulated seconds per second of the clock, at most */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PPP_LOCAL IP4(10, 64, 64, 1)
#define PPP_PEER  IP4(10, 64, 64, 2)
#define FILE_PORT 8099

static caller_t C;
static channel_t CH;
static int line_fd, hook, modem_on, dead;
static uint8_t prev_tx[BLK], blk_rx[BLK];
static int blk_n;
static double c_tx;
static long sim_blocks;
static struct timespec t_start;

/* ---- the octets between the modem and pppd ---- */
typedef struct { uint8_t b[65536]; size_t n; } q_t;
static q_t to_modem, to_pppd;
static int pppd_fd = -1;
static unsigned long oct_to_pppd, oct_from_pppd;

static void raw_rx(void *ctx, const uint8_t *b, size_t n)
{
    (void)ctx;
    if (n > sizeof to_pppd.b - to_pppd.n) n = sizeof to_pppd.b - to_pppd.n;
    memcpy(to_pppd.b + to_pppd.n, b, n);
    to_pppd.n += n;
}

static size_t raw_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = to_modem.n < max ? to_modem.n : max;
    (void)ctx;
    memcpy(b, to_modem.b, n);
    memmove(to_modem.b, to_modem.b + n, to_modem.n - n);
    to_modem.n -= n;
    return n;
}

/* ---- the file server: what the caller fetches and what it sends ---- */
static uint8_t down_file[400000], up_file[100000];
static size_t down_len, up_len;
static int lsock = -1, csock = -1;
static size_t sv_sent, sv_got;
static int sv_bad, sv_done;

static void make_files(int n, size_t down_text, size_t down_random, size_t up)
{
    gen_t g;
    g.s = 0x51a7u + (uint32_t)n * 977u;
    gen_fill(&g, GEN_HTML, down_file, down_text);
    gen_fill(&g, GEN_RANDOM, down_file + down_text, down_random);
    down_len = down_text + down_random;
    gen_fill(&g, GEN_TEXT, up_file, up);
    up_len = up;
}

static void serve(void)
{
    ssize_t r;

    if (csock < 0) {
        if (lsock < 0) return;
        csock = accept4(lsock, NULL, NULL, SOCK_NONBLOCK);
        if (csock < 0) return;
        sv_sent = sv_got = 0; sv_bad = sv_done = 0;
    }
    if (sv_sent < down_len) {
        r = write(csock, down_file + sv_sent, down_len - sv_sent);
        if (r > 0) sv_sent += (size_t)r;
    }
    if (!sv_done) {
        uint8_t b[4096];
        r = read(csock, b, sizeof b);
        if (r > 0) {
            if (sv_got + (size_t)r > up_len || memcmp(b, up_file + sv_got, (size_t)r) != 0) sv_bad = 1;
            sv_got += (size_t)r;
        } else if (r == 0) {
            /* the caller has sent everything: tell it what arrived */
            uint8_t ack = (uint8_t)(!sv_bad && sv_got == up_len && sv_sent == down_len ? 'K' : 'E');
            if (write(csock, &ack, 1) != 1) sv_bad = 1;
            sv_done = 1;
        }
    }
}

static int xfer(int fd, uint8_t *b, size_t n, int wr)
{
    size_t d = 0;
    while (d < n) {
        ssize_t r = wr ? write(fd, b + d, n - d) : read(fd, b + d, n - d);
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        d += (size_t)r;
    }
    return 0;
}

/* once per block of 10 ms: pppd's octets, the file server, the clock */
static void block_work(void)
{
    struct timespec now;
    long ahead_us;
    ssize_t r;

    if (pppd_fd >= 0) {
        r = read(pppd_fd, to_modem.b + to_modem.n, sizeof to_modem.b - to_modem.n);
        if (r > 0) { to_modem.n += (size_t)r; oct_from_pppd += (unsigned long)r; }
        if (to_pppd.n) {
            r = write(pppd_fd, to_pppd.b, to_pppd.n);
            if (r > 0) {
                memmove(to_pppd.b, to_pppd.b + r, to_pppd.n - (size_t)r);
                to_pppd.n -= (size_t)r; oct_to_pppd += (unsigned long)r;
            }
        }
    }
    serve();
    sim_blocks++;
    clock_gettime(CLOCK_MONOTONIC, &now);
    ahead_us = sim_blocks * (10000 / SPEED) -
               ((now.tv_sec - t_start.tv_sec) * 1000000L + (now.tv_nsec - t_start.tv_nsec) / 1000);
    if (ahead_us > 2000) {
        struct timespec d;
        d.tv_sec = ahead_us / 1000000; d.tv_nsec = (ahead_us % 1000000) * 1000;
        nanosleep(&d, NULL);
    }
}

/* one sample; the daemon's answer to a block arrives one block later */
static void step(double tone)
{
    double crx; uint8_t srv_rx, s_tx = prev_tx[blk_n]; int16_t x;

    channel_tick(&CH, s_tx, hook ? c_tx : 0.0, &crx, &srv_rx);
    blk_rx[blk_n] = hook ? srv_rx : 0xFF;
    x = (int16_t)lround(crx);
    c_tx = modem_on ? caller_tick_pcm(&C, x, s_tx) : tone;
    if (++blk_n == BLK) {
        uint8_t h = (uint8_t)hook;
        blk_n = 0;
        if (dead) return;
        if (xfer(line_fd, &h, 1, 1) || xfer(line_fd, blk_rx, BLK, 1) || xfer(line_fd, prev_tx, BLK, 0)) {
            dead = 1; printf("  the daemon is gone\n");
        }
        block_work();
    }
}
static void run_ms(int ms) { long i; for (i = 0; i < (long)ms * 8 && !dead; i++) step(0); }

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

static int http(const char *req, char *resp, size_t cap)
{
    struct sockaddr_in sa; int fd = socket(AF_INET, SOCK_STREAM, 0); size_t got = 0; long i;
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(8080); sa.sin_addr.s_addr = htonl(0x7F000001);
    resp[0] = 0;
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { if (fd >= 0) close(fd); return -1; }
    if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) { close(fd); return -1; }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    for (i = 0; i < 3L * FS && !dead; i++) {
        if (i % BLK == 0) {
            ssize_t r = read(fd, resp + got, cap - 1 - got);
            if (r == 0) break;
            if (r > 0) got += (size_t)r;
        }
        step(0);
    }
    resp[got] = 0;
    close(fd);
    return (int)got;
}

static unsigned long json_num(const char *json, const char *key)
{
    char k[64]; const char *p;
    snprintf(k, sizeof k, "\"%s\":", key);
    p = strstr(json, k);
    return p ? strtoul(p + strlen(k), NULL, 10) : 0;
}

static void lo_up(void)
{
    struct ifreq ifr; int s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "lo");
    ioctl(s, SIOCGIFFLAGS, &ifr); ifr.ifr_flags |= IFF_UP;
    ioctl(s, SIOCSIFFLAGS, &ifr);
    close(s);
}

static uint32_t if_address(const char *name)
{
    struct ifreq ifr; int s = socket(AF_INET, SOCK_DGRAM, 0); uint32_t a = 0;
    memset(&ifr, 0, sizeof ifr); strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFADDR, &ifr) == 0) a = ntohl(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr);
    close(s);
    return a;
}

/* ---- the far end: its own network namespace, pppd, and the file client ---- */
typedef struct { int ip_up, connected, down_ok, up_ok; unsigned long got; char ack; } far_t;

#define MAX_ARGS 40
static void far_end(int ppp_fd, int res_fd, const char *log, const char *opts)
{
    static uint8_t buf[sizeof down_file];
    static char optbuf[200];
    const char *argv[MAX_ARGS]; int na = 0, i, fd, s;
    far_t r; pid_t pd; struct sockaddr_in sa; struct timeval tv; char *tok;
    size_t got = 0, sent = 0;

    memset(&r, 0, sizeof r);
    if (unshare(CLONE_NEWNET) != 0) { if (write(res_fd, &r, sizeof r) < 0) _exit(3); _exit(3); }
    lo_up();
    pd = fork();
    if (pd == 0) {
        fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(ppp_fd, 0); dup2(ppp_fd, 1);
        if (fd >= 0) dup2(fd, 2);
        for (i = 3; i < 64; i++) close(i);
        argv[na++] = "pppd";
        /* the link: our descriptors, no modem lines, stay in the foreground */
        argv[na++] = "notty"; argv[na++] = "local"; argv[na++] = "nodetach";
        argv[na++] = "debug"; argv[na++] = "logfd"; argv[na++] = "2";
        /* the caller of a dial-up server: does not ask who the server is,
         * gives its name and password when asked, takes the address it is given */
        argv[na++] = "noauth"; argv[na++] = "user"; argv[na++] = "alice";
        argv[na++] = "password"; argv[na++] = "wonderland";
        argv[na++] = "noipdefault"; argv[na++] = "nodefaultroute"; argv[na++] = "noipv6";
        /* compression: whatever pppd does by itself, unless the test says */
        snprintf(optbuf, sizeof optbuf, "%s", opts);
        for (tok = strtok(optbuf, " "); tok && na < MAX_ARGS - 1; tok = strtok(NULL, " ")) argv[na++] = tok;
        argv[na] = NULL;
        execvp("pppd", (char *const *)argv);
        _exit(127);
    }
    close(ppp_fd);
    /* wait for ppp0 to have the address the server hands out */
    for (i = 0; i < 600 && !r.ip_up; i++) {
        if (waitpid(pd, NULL, WNOHANG) == pd) { pd = -1; break; }
        if (if_address("ppp0") == PPP_PEER) r.ip_up = 1;
        else usleep(50000);
    }
    if (r.ip_up) {
        s = socket(AF_INET, SOCK_STREAM, 0);
        tv.tv_sec = 120; tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(FILE_PORT); sa.sin_addr.s_addr = htonl(PPP_LOCAL);
        if (connect(s, (struct sockaddr *)&sa, sizeof sa) == 0) {
            r.connected = 1;
            /* fetch, then send, then hear what the other side got */
            while (got < down_len) {
                ssize_t k = read(s, buf + got, down_len - got);
                if (k <= 0) break;
                got += (size_t)k;
            }
            r.got = (unsigned long)got;
            r.down_ok = got == down_len && memcmp(buf, down_file, down_len) == 0;
            while (sent < up_len) {
                ssize_t k = write(s, up_file + sent, up_len - sent);
                if (k <= 0) break;
                sent += (size_t)k;
            }
            shutdown(s, SHUT_WR);
            if (read(s, &r.ack, 1) == 1) r.up_ok = r.ack == 'K';
        }
        close(s);
    }
    if (write(res_fd, &r, sizeof r) != (ssize_t)sizeof r) _exit(4);
    /* leave the link up for a moment (the other side reads its counters),
     * then pppd closes it as a user would */
    usleep(1500000);
    if (pd > 0) { kill(pd, SIGTERM); waitpid(pd, NULL, 0); }
    _exit(0);
}

/* lines of pppd's log that tell what was negotiated */
static void show_log(const char *log, int max)
{
    FILE *f = fopen(log, "r"); char line[600]; int n = 0;
    if (!f) return;
    while (fgets(line, sizeof line, f) && n < max) {
        if (strstr(line, "CCP") || strstr(line, "ompression") || strstr(line, "ProtRej")) {
            line[strcspn(line, "\n")] = 0;
            if (strlen(line) > 150) strcpy(line + 147, "...");
            printf("    pppd: %s\n", line);
            n++;
        }
    }
    fclose(f);
}

static int log_has(const char *log, const char *needle)
{
    static char buf[200000]; int fd = open(log, O_RDONLY); ssize_t n;
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

typedef struct {
    const char *what, *opts;        /* pppd options on top of the basic ones */
    size_t down_text, down_random, up;
    const char *tx, *rx;            /* what the daemon's status must say */
    const char *in_log;             /* and pppd's log, NULL = nothing asked */
} call_t;

static void one_call(int n, const call_t *k, const char *log)
{
    static char resp[40000];
    caller_cfg_t cc; int sp[2], rp[2], ms = 0, st, waited; pid_t far; far_t r; long i;
    unsigned long ti, to, ri, ro;

    make_files(n, k->down_text, k->down_random, k->up);
    printf("  call %d: pppd %s\n", n + 1, k->what);
    hook = 1; run_ms(500);
    dial("5551234");
    run_ms(3000 + 2000 + 300);
    caller_cfg_default(&cc);
    cc.v90 = 1; cc.raw_rx = raw_rx; cc.raw_tx = raw_tx;
    to_modem.n = to_pppd.n = 0; oct_to_pppd = oct_from_pppd = 0;
    caller_init(&C, &cc);
    modem_on = 1;
    for (i = 0; i < 40000L * 8 && !dead && !C.ppp_on; i++) step(0);
    CHECK(C.ppp_on);
    if (!C.ppp_on) { modem_on = 0; hook = 0; run_ms(1000); return; }

    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), 0);
    CHECK_EQ_I(pipe(rp), 0);
    far = fork();
    if (far == 0) {
        close(sp[0]); close(rp[0]); close(line_fd); if (lsock >= 0) close(lsock);
        far_end(sp[1], rp[1], log, k->opts);
    }
    close(sp[1]); close(rp[1]);
    pppd_fd = sp[0];
    fcntl(pppd_fd, F_SETFL, O_NONBLOCK);
    fcntl(rp[0], F_SETFL, O_NONBLOCK);

    /* the modem works while the far end does its part: up to ten minutes */
    memset(&r, 0, sizeof r);
    waited = 0;
    for (i = 0; i < 600000L * 8 && !dead && !waited; i++) {
        step(0);
        if (i % BLK == 0 && read(rp[0], &r, sizeof r) == (ssize_t)sizeof r) waited = 1;
        if (i % (BLK * 100) == 0 && !waited && waitpid(far, &st, WNOHANG) == far) { far = -1; break; }
        ms = (int)(i / 8);
    }
    CHECK(waited);
    CHECK_EQ_I(r.ip_up, 1);
    CHECK_EQ_I(r.connected, 1);
    if (!r.down_ok) printf("    fetched %lu of %lu octets\n", r.got, (unsigned long)down_len);
    CHECK_EQ_I(r.down_ok, 1);
    CHECK_EQ_I(r.up_ok, 1);
    CHECK_EQ_I(sv_bad, 0);
    CHECK_EQ_I(sv_got, up_len);

    /* the daemon's view, while the call is still up (its status page is
     * brought up to date once a second) */
    run_ms(1200);
    CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    ti = json_num(resp, "comp_tx_in"); to = json_num(resp, "comp_tx_out");
    ri = json_num(resp, "comp_rx_in"); ro = json_num(resp, "comp_rx_out");
    {
        char want[100];
        snprintf(want, sizeof want, "\"comp_tx\":\"%s\",\"comp_rx\":\"%s\"", k->tx, k->rx);
        if (!strstr(resp, want)) {
            const char *p = strstr(resp, "\"comp_tx\"");
            printf("    status says %.60s, expected %s\n", p ? p : "(nothing)", want);
        }
        CHECK(strstr(resp, want) != NULL);
    }
    CHECK(strstr(resp, "\"peer_name\":\"alice\"") != NULL);
    printf("    %lu octets fetched and %lu sent over TCP in %.1f s of line time, both files whole\n",
           (unsigned long)down_len, (unsigned long)up_len, ms / 1000.0);
    printf("    daemon: to caller %s, %lu octets of IP sent as %lu", k->tx, ti, to);
    if (to) printf(" (%lu.%02lu to 1)", ti / to, ti * 100 / to % 100);
    printf("; from caller %s, %lu received as %lu", k->rx, ro, ri);
    if (ri) printf(" (%lu.%02lu to 1)", ro / ri, ro * 100 / ri % 100);
    printf("\n    modem: %lu octets to pppd, %lu from it\n", oct_to_pppd, oct_from_pppd);
    if (strcmp(k->tx, "none") != 0) {
        /* the text part of the download shrank; all of it went through the compressor */
        CHECK(ti >= down_len);
        CHECK(to < ti);
        if (k->down_text > 4 * k->down_random) CHECK(to * 2 < ti);
    } else {
        CHECK_EQ_I(ti, 0);
    }
    if (strcmp(k->rx, "none") != 0) CHECK(ri * 2 < ro && ro >= up_len);
    else CHECK_EQ_I(ri, 0);
    /* no decompressor of ours lost step, and none of pppd's (it would have asked) */
    CHECK(strstr(resp, "\"rx_errors\":0") != NULL);

    /* pppd ends the call: Terminate-Request, then the modem hangs up */
    for (i = 0; i < 20000L * 8 && !dead && far > 0; i++) {
        step(0);
        if (i % (BLK * 20) == 0 && waitpid(far, &st, WNOHANG) == far) far = -1;
    }
    CHECK(far == -1);
    if (far > 0) { kill(far, SIGKILL); waitpid(far, &st, 0); }
    close(pppd_fd); pppd_fd = -1; close(rp[0]);
    if (csock >= 0) { close(csock); csock = -1; }
    show_log(log, 14);
    if (k->in_log) {
        if (!log_has(log, k->in_log)) printf("    pppd's log lacks \"%s\"\n", k->in_log);
        CHECK(log_has(log, k->in_log));
    }
    caller_hangup(&C);
    modem_on = 0; hook = 0; c_tx = 0;
    run_ms(1500);
}

int main(int argc, char **argv)
{
    /* What pppd 2.5.1 does by itself (first call) was read off its log and
     * is pinned here; the other calls turn its options one way and another. */
    static const call_t calls[] = {
        { "with its own defaults", "", 300000, 40000, 60000, "deflate", "deflate", "Deflate (15) compression enabled" },
        { "deflate 12,10 (it decompresses with 4 KiB, compresses with 1 KiB)", "deflate 12,10", 80000, 0, 30000, "deflate", "deflate", "Deflate (12/10) compression enabled" },
        { "nodeflate (BSD-Compress is all it offers)", "nodeflate", 40000, 0, 10000, "none", "none", "rcvd [CCP ConfRej id=0x1 <bsd v1 15>]" },
        { "nobsdcomp nodeflatedraft (RFC 1979 number only)", "nobsdcomp nodeflatedraft", 80000, 0, 30000, "deflate", "deflate", "Deflate (15) compression enabled" },
        { "noccp", "noccp", 40000, 0, 10000, "none", "none", "sent [LCP ProtRej" },
    };
    static const double one[1] = { 1.0 };
    static char conf[300], log[300], err[CFG_ERR_MAX];
    channel_cfg cc; cfg_t c; int sv[2], st, on = 1; pid_t pid; struct sockaddr_in sa;
    size_t i;

    if (argc < 3) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (geteuid() != 0) { printf("  ns_pppd: not root, nothing tried\n"); return 0; }
    /* nothing of the machine's PPP set-up: see the top of the file */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0 ||
        mount("none", "/etc/ppp", "tmpfs", 0, NULL) != 0 ||
        mount("none", "/run", "tmpfs", 0, NULL) != 0) {
        printf("  ns_pppd: cannot cover /etc/ppp and /run (%s): not a mount namespace of our own? Nothing tried\n", strerror(errno));
        return 0;
    }
    snprintf(conf, sizeof conf, "%s/nsp_v90d.conf", argv[2]);
    snprintf(log, sizeof log, "%s/nsp_pppd.log", argv[2]);
    lo_up();

    cfg_default(&c);
    strcpy(c.line_number, "5551234"); c.rings = 1;
    c.auth = CFG_AUTH_CHAP; c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland");
    c.status_port = 8080;
    CHECK_EQ_I(c.compression, 1);
    CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);

    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pid = fork();
    if (pid == 0) {
        /* root here, but no real-time priority and no locked memory on
         * somebody's workstation: the daemon carries on without them */
        prctl(PR_CAPBSET_DROP, CAP_SYS_NICE, 0, 0, 0);
        prctl(PR_CAPBSET_DROP, CAP_IPC_LOCK, 0, 0, 0);
        close(sv[0]);                           /* it may well be descriptor 3 */
        if (sv[1] != 3) { dup2(sv[1], 3); close(sv[1]); }
        execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "v90t0", "-A", "", "-N", "/nonexistent", (char *)NULL);
        _exit(127);
    }
    close(sv[1]); line_fd = sv[0];
    signal(SIGPIPE, SIG_IGN);

    lsock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(FILE_PORT); sa.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK_EQ_I(bind(lsock, (struct sockaddr *)&sa, sizeof sa), 0);
    CHECK_EQ_I(listen(lsock, 2), 0);

    memset(&cc, 0, sizeof cc);
    cc.ds_noise_dbfs = cc.us_noise_dbfs = -60; cc.ds_taps = one; cc.ds_ntaps = 1; cc.us_taps = one; cc.us_ntaps = 1;
    cc.echo_gain = 0.15; cc.echo_delay = 21; cc.full_scale = 32124.0; cc.seed = 3;
    channel_init(&CH, &cc);
    memset(prev_tx, 0xFF, sizeof prev_tx);
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    run_ms(2500);
    CHECK_EQ_I(if_address("v90t0"), PPP_LOCAL);
    for (i = 0; i < sizeof calls / sizeof calls[0] && !dead; i++)
        one_call((int)i, &calls[i], log);

    CHECK_EQ_I(dead, 0);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    unlink(conf); unlink(log);
    T_REPORT("ns_pppd");
}
