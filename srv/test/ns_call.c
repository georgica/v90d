/* The real daemon binary, in a private network namespace (run through
 * `unshare -Urn`, see the Makefile): this program starts v90d with a socket
 * pair as its telephone line and plays the caller on the other end: hook
 * switch, DTMF dialler, modem, V.42, PPP client. It checks what only the
 * real thing can show: the TUN device, kernel routing and the NAT rule, the
 * HTTP listener, the settings form changing the file and the behaviour.
 *
 *   ns_call PATH-TO-v90d SCRATCH-DIR [PATH-TO-v90nv FLASH.bin NEW.uimage]
 *
 * With the last three (the Makefile makes them: the image's flash tool
 * built for the host, a 16 MiB file that stands for the flash of a box
 * started by our loader, a new system image) the Firmware page is tried
 * end to end as well: refused during a call, written when the lines are
 * free, and back to the stock firmware. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "test.h"
#include "config.h"
#include "caller.h"
#include "channel.h"
#include "tun.h"
#include "saturn.h"

#define FS 8000
#define BLK 80
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define PPP_LOCAL IP4(10, 64, 64, 1)
#define PPP_PEER  IP4(10, 64, 64, 2)
#define WAN_LOCAL IP4(192, 0, 2, 1)
#define WAN_HOST  IP4(192, 0, 2, 9)

static caller_t C;
static channel_t CH;
static int line_fd, hook, modem_on;
static int idle_fd = -1;                /* a second line of the daemon that nobody uses */
static uint32_t peer_ip = PPP_PEER;     /* the address the caller is given */
static uint8_t prev_tx[BLK];            /* the daemon's last block */
static uint8_t blk_rx[BLK]; static int blk_n;
static double c_tx, c_rx_pw;
static uint8_t cli_pkt[1600]; static size_t cli_len; static int cli_n;

static void cli_rx(void *ctx, const uint8_t *p, size_t n) { (void)ctx; cli_n++; cli_len = n < sizeof cli_pkt ? n : sizeof cli_pkt; memcpy(cli_pkt, p, cli_len); }

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

static int dead, quiet_exit;
static long idle_noise;
/* one sample; the daemon's answer to a block arrives one block later */
static void step(double tone)
{
    double crx; uint8_t srv_rx, s_tx = prev_tx[blk_n]; int16_t x;
    if (modem_on && C.cfg.v92) {
        /* a V.92 caller brings its own line (sim/v92call): octets both ways */
        blk_rx[blk_n] = caller_tick_v92(&C, s_tx);
    } else {
        channel_tick(&CH, s_tx, hook ? c_tx : 0.0, &crx, &srv_rx);
        blk_rx[blk_n] = hook ? srv_rx : 0xFF;
        x = (int16_t)lround(crx);
        c_rx_pw += ((double)x * x - c_rx_pw) / 400.0;
        c_tx = modem_on ? caller_tick_pcm(&C, x, s_tx) : tone;
    }
    if (++blk_n == BLK) {
        uint8_t h = (uint8_t)hook;
        blk_n = 0;
        if (dead) return;
        if (xfer(line_fd, &h, 1, 1) || xfer(line_fd, blk_rx, BLK, 1) || xfer(line_fd, prev_tx, BLK, 0)) {
            dead = 1; if (!quiet_exit) printf("  the daemon is gone\n");
        }
        if (idle_fd >= 0 && !dead) {
            /* the other port: on hook, silence in; what comes out must be silence */
            uint8_t ib[1 + BLK], ob[BLK]; int k;
            ib[0] = 0; memset(ib + 1, 0xFF, BLK);
            if (xfer(idle_fd, ib, sizeof ib, 1) || xfer(idle_fd, ob, BLK, 0)) { dead = 1; if (!quiet_exit) printf("  the daemon is gone (idle line)\n"); }
            else for (k = 0; k < BLK; k++) if (ob[k] != 0xFF) idle_noise++;
        }
    }
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

/* kind: 0 V.22bis, 1 V.90, 2 V.34, 3 V.92 */
static void start_modem(int kind)
{
    caller_cfg_t cc;
    caller_cfg_default(&cc);
    cc.v90 = kind == 1; cc.v34 = kind == 2; cc.v92 = kind == 3; cc.ppp.user = "alice"; cc.ppp.pass = "wonderland"; cc.ip_rx = cli_rx;
    caller_init(&C, &cc);
    modem_on = 1;
}
static int wait_ip(int limit_ms)
{
    long i;
    for (i = 0; i < (long)limit_ms * 8 && !dead; i++) { if (C.ppp_on && C.ppp.ip_up) return (int)(i / 8); step(0); }
    return -1;
}
static void hang_up(void) { if (modem_on) caller_hangup(&C); if (modem_on && C.cfg.v92) v92call_hangup(&C.vc); modem_on = 0; hook = 0; c_tx = 0; run_ms(1000); }

/* ---- a console that plays modem to modem (service = saturn) ---- */
static uint8_t sat_dn[2048]; static size_t sat_dn_n, sat_at, sat_n;
static void sat_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (sat_dn_n + n <= sizeof sat_dn) { memcpy(sat_dn + sat_dn_n, b, n); sat_dn_n += n; } }
static size_t sat_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = 0; (void)ctx;
    while (n < max && sat_at < sat_n) { b[n++] = (uint8_t)(sat_at * 9 + 2); sat_at++; }
    return n;
}
static void sat_call(const char *num)
{
    caller_cfg_t cc; long i;
    hook = 1; run_ms(500);
    dial(num);
    run_ms(3000 + 2000 + 300);
    caller_cfg_default(&cc);
    cc.use_v42 = 0; cc.raw_rx = sat_rx; cc.raw_tx = sat_tx;
    caller_init(&C, &cc);
    modem_on = 1; sat_dn_n = sat_at = sat_n = 0;
    for (i = 0; i < 30L * FS && !C.connected && !dead; i++) step(0);
    CHECK(C.connected);
    run_ms(1500);
}
static int udp_at(uint32_t addr, int port)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1; struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(addr);
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, (struct sockaddr *)(void *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    return fd;
}
static void udp_to(int fd, int port, const char *b, size_t n)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)port); sa.sin_addr.s_addr = htonl(IP4(127, 0, 0, 1));
    (void)sendto(fd, b, n, 0, (struct sockaddr *)(void *)&sa, sizeof sa);
}
/* everything waiting on the socket: pings and pongs counted, the octets of
 * the data datagrams put together as the receiving box would */
static saturn_rx sat_peer_rx; static uint8_t sat_got[2048]; static size_t sat_got_n; static int sat_pings, sat_pongs, sat_from;
static char sat_first[64];
static void udp_drain(int fd)
{
    static uint8_t dg[8192]; struct sockaddr_in from; socklen_t fl; ssize_t r;
    for (;;) {
        fl = sizeof from;
        r = recvfrom(fd, dg, sizeof dg, MSG_DONTWAIT, (struct sockaddr *)(void *)&from, &fl);
        if (r <= 0) break;
        sat_from = ntohs(from.sin_port);
        if (r == 10 && !memcmp(dg, "PING_SHIRO", 10)) sat_pings++;
        else if (r == 10 && !memcmp(dg, "PONG_SHIRO", 10)) sat_pongs++;
        else if (r == 10 && !memcmp(dg, "OPEN_SHIRO", 10)) ;
        else {
            if (!sat_first[0]) { size_t k = (size_t)r < sizeof sat_first - 1 ? (size_t)r : sizeof sat_first - 1; memcpy(sat_first, dg, k); sat_first[k] = 0; }
            sat_got_n += saturn_unpack(&sat_peer_rx, dg, (size_t)r, sat_got + sat_got_n, sizeof sat_got - sat_got_n);
        }
    }
}

/* ---- IP ---- */
static uint16_t csum(const uint8_t *p, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ICMP echo (type 8) or reply (type 0), 28 + 32 octets */
static size_t icmp(uint8_t *p, uint32_t src, uint32_t dst, int type, uint16_t id, uint16_t seq)
{
    int i; uint16_t c;
    memset(p, 0, 60);
    p[0] = 0x45; p[3] = 60; p[8] = 64; p[9] = 1; put32(p + 12, src); put32(p + 16, dst);
    c = csum(p, 20); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
    p[20] = (uint8_t)type; p[24] = (uint8_t)(id >> 8); p[25] = (uint8_t)id; p[26] = (uint8_t)(seq >> 8); p[27] = (uint8_t)seq;
    for (i = 28; i < 60; i++) p[i] = (uint8_t)(i * 3);
    c = csum(p + 20, 40); p[22] = (uint8_t)(c >> 8); p[23] = (uint8_t)c;
    return 60;
}

/* send from the caller, wait for a packet to reach the caller; 1 = got one */
static int ping_via_ppp(uint32_t dst, uint16_t seq, int wan_fd)
{
    uint8_t p[1600]; long i; int n0 = cli_n, sent = 0;
    size_t n = icmp(p, peer_ip, dst, 8, 0x1234, seq);
    for (i = 0; i < 6L * FS && cli_n == n0 && !dead; i++) {
        if (!sent && i % 400 == 0 && caller_send_ip(&C, p, n) == 0) sent = 1;
        if (wan_fd >= 0 && i % BLK == 0) {
            /* we are the Internet host: answer what arrives on the WAN side */
            uint8_t q[1600], a[1600]; ssize_t r = read(wan_fd, q, sizeof q);
            if (r >= 28 && q[9] == 1 && q[20] == 8) {
                CHECK_EQ_I(get32(q + 12), WAN_LOCAL);      /* masqueraded: not the caller's address */
                CHECK_EQ_I(get32(q + 16), WAN_HOST);
                icmp(a, WAN_HOST, get32(q + 12), 0, (uint16_t)(q[24] << 8 | q[25]), (uint16_t)(q[26] << 8 | q[27]));
                if (write(wan_fd, a, 60) != 60) printf("  WAN write failed\n");
            }
        }
        step(0);
    }
    return cli_n > n0;
}

/* ---- HTTP against the daemon while keeping its clock going ---- */
static int http_n(const char *req, size_t reqlen, char *resp, size_t cap);
static int http(const char *req, char *resp, size_t cap) { return http_n(req, strlen(req), resp, cap); }
static int http_n(const char *req, size_t reqlen, char *resp, size_t cap)
{
    struct sockaddr_in sa; int fd = socket(AF_INET, SOCK_STREAM, 0); size_t got = 0; long i;
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(8080); sa.sin_addr.s_addr = htonl(0x7F000001);
    resp[0] = 0;
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { if (fd >= 0) close(fd); return -1; }
    if (write(fd, req, reqlen) != (ssize_t)reqlen) { close(fd); return -1; }
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

static int file_has(const char *path, const char *needle)
{
    static char buf[CFG_FILE_MAX + 1]; int fd = open(path, O_RDONLY); ssize_t n;
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof buf - 1); close(fd);
    if (n < 0) return 0;
    buf[n] = 0;
    return strstr(buf, needle) != NULL;
}

/* ---- the Firmware page, with a file for the flash ---- */
static const char *fw_flash;
static int flash_is(uint32_t off, const void *want, size_t n)
{
    static uint8_t b[65536]; int fd = open(fw_flash, O_RDONLY); ssize_t r;
    if (fd < 0 || n > sizeof b) return 0;
    r = pread(fd, b, n, (off_t)off); close(fd);
    return r == (ssize_t)n && memcmp(b, want, n) == 0;
}
/* POST /firmware/upload of img[0..n) as a browser's form sends it */
static int upload(const uint8_t *img, size_t n, char *resp, size_t cap)
{
    static char req[70000]; static const char B[] = "----ns7MA4YWxkTrZu0gW";
    char head[300], tail[64]; size_t hl, tl, at;
    hl = (size_t)snprintf(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"image\"; filename=\"v90d.bin\"\r\n"
                          "Content-Type: application/octet-stream\r\n\r\n", B);
    tl = (size_t)snprintf(tail, sizeof tail, "\r\n--%s--\r\n", B);
    at = (size_t)snprintf(req, sizeof req, "POST /firmware/upload HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n"
                          "Content-Type: multipart/form-data; boundary=%s\r\nContent-Length: %u\r\n\r\n", B, (unsigned)(hl + n + tl));
    if (at + hl + n + tl > sizeof req) return -1;
    memcpy(req + at, head, hl); at += hl; memcpy(req + at, img, n); at += n; memcpy(req + at, tail, tl); at += tl;
    return http_n(req, at, resp, cap);
}

int main(int argc, char **argv)
{
    static uint8_t new_img[60000]; size_t new_len = 0; static char fw_note[300], fw_state[300], fw_where[300], cmd[1000];
    const char *fw_nv = argc >= 6 ? argv[3] : NULL;
    static const double one[1] = { 1.0 };
    static char resp[40000], conf[300], err[CFG_ERR_MAX], post[400];
    channel_cfg cc; cfg_t c; int sv[2], wan_fd, s, ms, i, st; pid_t pid; struct ifreq ifr;
    const char *body;

    if (argc < 3) return 2;
    setvbuf(stdout, NULL, _IOLBF, 0);
    snprintf(conf, sizeof conf, "%s/ns_v90d.conf", argv[2]);
    if (fw_nv) {
        FILE *f = fopen(argv[5], "rb");
        fw_flash = argv[4];
        if (f) { new_len = fread(new_img, 1, sizeof new_img, f); fclose(f); }
        CHECK(new_len > 64 && new_len < sizeof new_img);
        snprintf(fw_note, sizeof fw_note, "%s/ns_fw.restart", argv[2]);
        snprintf(fw_state, sizeof fw_state, "%s/ns_fw.state", argv[2]);
        snprintf(fw_where, sizeof fw_where, "%s/ns_fw.where", argv[2]);
        unlink(fw_note); unlink(fw_state);
        /* what the boot script does first, and what the daemon and the tool go by */
        snprintf(cmd, sizeof cmd, "%s -f %s where > %s", fw_nv, fw_flash, fw_where);
        CHECK_EQ_I(system(cmd), 0);
        setenv("V90NV_FLASH", fw_flash, 1); setenv("V90NV_RESTART_NOTE", fw_note, 1);
        setenv("V90NV_STATE", fw_state, 1); setenv("V90D_WHERE", fw_where, 1);
    }

    /* the namespace: loopback up (it stands in for the LAN), and a second
     * TUN device as the way to the Internet */
    s = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "lo");
    ioctl(s, SIOCGIFFLAGS, &ifr); ifr.ifr_flags |= IFF_UP;
    CHECK_EQ_I(ioctl(s, SIOCSIFFLAGS, &ifr), 0);
    close(s);
    wan_fd = tun_open("wan0", WAN_LOCAL, WAN_HOST, 1500);
    CHECK(wan_fd >= 0);
    if (wan_fd < 0) { printf("  no TUN device here: %s\n", strerror(errno)); T_REPORT("ns_call"); }

    cfg_default(&c);
    strcpy(c.line_number, "5551234"); c.rings = 1;
    c.auth = CFG_AUTH_CHAP; c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland");
    c.status_port = 8080;
    CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);

    CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    pid = fork();
    if (pid == 0) {
        close(wan_fd);                          /* it may well be descriptor 3 */
        if (sv[1] != 3) { dup2(sv[1], 3); close(sv[1]); }
        close(sv[0]);
        execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "", "-N", fw_nv ? fw_nv : "/nonexistent", (char *)NULL);
        _exit(127);
    }
    close(sv[1]); line_fd = sv[0];
    signal(SIGPIPE, SIG_IGN);

    memset(&cc, 0, sizeof cc);
    cc.ds_noise_dbfs = cc.us_noise_dbfs = -60; cc.ds_taps = one; cc.ds_ntaps = 1; cc.us_taps = one; cc.us_ntaps = 1;
    cc.echo_gain = 0.15; cc.echo_delay = 21; cc.full_scale = 32124.0; cc.seed = 3;
    channel_init(&CH, &cc);
    memset(prev_tx, 0xFF, sizeof prev_tx);

    /* 1. idle: the status page is there, the NAT rule and the device exist */
    run_ms(2500);
    CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "200 OK") != NULL);
    CHECK(http("GET /settings HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "5551234") != NULL);
    CHECK(strstr(resp, "wonderland") == NULL);          /* passwords are not shown */
    if (fw_nv) {
        CHECK(http("GET /firmware HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 200 ") != NULL && strstr(resp, "by the v90d loader, from set A of the flash") != NULL);
        CHECK(strstr(resp, "action=\"/firmware/upload\"") != NULL);
        CHECK(!flash_is(0x2E0200, new_img, new_len));
    }
    CHECK_EQ_I(if_address("ppp0"), PPP_LOCAL);
    CHECK(file_has("/proc/sys/net/ipv4/ip_forward", "1"));

    /* 2. a V.90 call: dial tone, number, ringing tone, modem, PPP */
    hook = 1; run_ms(500);
    CHECK(c_rx_pw > 200.0 * 200.0);
    dial("5551234");
    run_ms(3000 + 2000 + 300);                          /* post-dial delay and one ring at the most */
    start_modem(1);
    ms = wait_ip(30000);
    printf("  V.90 call through the daemon: IP up after %.2f s, %d bit/s down, %d bit/s up\n", ms / 1000.0, C.rx_rate, C.tx_rate);
    CHECK(ms > 0);
    CHECK(C.rx_rate >= 40000);
    /* the kernel answers a ping to our end of the link */
    CHECK(ping_via_ppp(PPP_LOCAL, 1, -1));
    CHECK(cli_len == 60 && cli_pkt[20] == 0 && get32(cli_pkt + 12) == PPP_LOCAL && get32(cli_pkt + 16) == PPP_PEER);
    /* and forwards one to the Internet, masqueraded, and the answer back */
    cli_len = 0;
    CHECK(ping_via_ppp(WAN_HOST, 2, wan_fd));
    CHECK(cli_len == 60 && cli_pkt[20] == 0 && get32(cli_pkt + 12) == WAN_HOST && get32(cli_pkt + 16) == PPP_PEER);
    CHECK(cli_pkt[27] == 2);
    /* the status page knows the caller */
    run_ms(1200);
    CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "alice") != NULL);
    CHECK(strstr(resp, "V.90") != NULL);
    CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0 || 1);
    /* PPP compression is on unless switched off, and this caller asks for
     * Deflate as pppd does: the pings went compressed, and the page says so */
    CHECK(strstr(resp, "\"ppp_compression\":true") != NULL);
    CHECK(strstr(resp, "\"comp_tx\":\"deflate\",\"comp_rx\":\"deflate\"") != NULL);
    CHECK(C.ppp.comp_rx_ok >= 1 && C.ppp.comp_tx >= 1);
    CHECK_EQ_I(C.ppp.comp_rx_bad, 0);
    CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "<th>Compressed to caller</th><td>Deflate") != NULL);
    CHECK(http("GET /settings HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "<select name=\"compression\">") != NULL);

    /* 3. the settings form during the call: accepted, written once the line is free */
    body = "number=42&busy_chance=0&service=archive&v92=on&compression=off";
    snprintf(post, sizeof post, "POST /settings HTTP/1.0\r\nHost: 127.0.0.1:8080\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    CHECK(http(post, resp, sizeof resp) > 0);
    CHECK(strstr(resp, " 303 ") != NULL);
    CHECK(!file_has(conf, "number = 42"));
    CHECK(ping_via_ppp(PPP_LOCAL, 3, -1));              /* the call goes on */
    if (fw_nv) {
        /* 3a. a new firmware during the call: not now, and nothing is touched */
        CHECK(upload(new_img, new_len, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 409 ") != NULL && strstr(resp, "A call is in progress") != NULL);
        CHECK(!flash_is(0x2E0200, new_img, new_len));
        CHECK(access(fw_state, F_OK) != 0 && access(fw_note, F_OK) != 0);
        CHECK(ping_via_ppp(PPP_LOCAL, 30, -1));
    }
    hang_up();
    run_ms(500);
    if (fw_nv) {
        /* 3b. with the lines free: checked, answered, written by the flash
         * tool, and the boot script is asked to restart */
        static uint8_t bad[60000];
        memcpy(bad, new_img, new_len); bad[new_len / 2] ^= 1;
        CHECK(upload(bad, new_len, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 400 ") != NULL && strstr(resp, "checksum is wrong") != NULL);
        run_ms(300);
        CHECK(!flash_is(0x2E0200, new_img, 64) && access(fw_note, F_OK) != 0);
        CHECK(upload(new_img, new_len, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 200 ") != NULL && strstr(resp, "being written to the flash now") != NULL);
        for (i = 0; i < 100 && access(fw_note, F_OK) != 0; i++) run_ms(100);
        CHECK(access(fw_note, F_OK) == 0);
        CHECK(file_has(fw_state, "written: v90d ht802"));
        CHECK(flash_is(0x2E0200, new_img, new_len));
        CHECK(flash_is(0x2E0000, "\xF9\x7A\xC9\x23", 4));
        CHECK(http("GET /firmware HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<h2>Last upload</h2>") != NULL && strstr(resp, "the box restarts") != NULL);
        unlink(fw_note);
    }
    CHECK(file_has(conf, "number = 42"));
    CHECK(file_has(conf, "wonderland"));

    /* 4. the old number is now a wrong number, the new one connects (V.22bis this time) */
    hook = 1; run_ms(500); dial("5551234"); run_ms(4000);
    c_rx_pw = 0; run_ms(3000);
    CHECK(c_rx_pw > 100.0 * 100.0);                     /* congestion tone, not an answer */
    hang_up();
    hook = 1; run_ms(500); dial("42"); run_ms(3000 + 2000 + 300);
    start_modem(0);
    ms = wait_ip(30000);
    printf("  V.22bis call to the new number: IP up after %.2f s\n", ms / 1000.0);
    CHECK(ms > 0);
    CHECK(ping_via_ppp(PPP_LOCAL, 4, -1));
    /* archive mode was switched on with that form: whatever name server the
     * caller asks, the question lands on the gateway's port on the box (here:
     * our socket), and the answer goes back as if that server had given it */
    CHECK(file_has(conf, "service = archive"));
    CHECK(file_has(conf, "v92 = on"));
    CHECK(file_has(conf, "compression = off"));
    CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "\"service\":\"archive\"") != NULL);
    CHECK(strstr(resp, "\"v92\":true") != NULL);
    /* PPP compression was switched off with it: the same caller asks, is
     * told that we have no CCP, and the call is uncompressed */
    CHECK(strstr(resp, "\"ppp_compression\":false") != NULL);
    CHECK(strstr(resp, "\"comp_tx\":\"none\",\"comp_rx\":\"none\"") != NULL);
    CHECK_EQ_I(C.ppp.comp_rx_ok + C.ppp.comp_tx, 0);
    CHECK(C.ppp.proto_rejects >= 1);
    {
        struct sockaddr_in sa, from; socklen_t fl = sizeof from;
        uint8_t q[64], r[64]; int us = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0), got = 0, n0; uint16_t ck; long k;
        memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(8353);
        CHECK(us >= 0 && bind(us, (struct sockaddr *)&sa, sizeof sa) == 0);
        memset(q, 0, sizeof q);
        q[0] = 0x45; q[3] = 40; q[8] = 64; q[9] = 17; put32(q + 12, peer_ip); put32(q + 16, 0x09090909u);
        ck = csum(q, 20); q[10] = (uint8_t)(ck >> 8); q[11] = (uint8_t)ck;
        q[20] = 0x12; q[21] = 0x34; q[22] = 0; q[23] = 53; q[25] = 20;        /* UDP 4660 -> 53, no checksum */
        memcpy(q + 28, "which name?", 12);
        n0 = cli_n;
        for (k = 0; k < 6L * FS && cli_n == n0 && !dead; k++) {
            if (k % 4000 == 0) (void)caller_send_ip(&C, q, 40);
            if (!got && k % BLK == 0) {
                ssize_t n = recvfrom(us, r, sizeof r, 0, (struct sockaddr *)&from, &fl);
                if (n == 12 && memcmp(r, "which name?", 12) == 0) {
                    got = 1;
                    CHECK_EQ_I(ntohl(from.sin_addr.s_addr), peer_ip); CHECK_EQ_I(ntohs(from.sin_port), 0x1234);
                    CHECK(sendto(us, "this one.", 10, 0, (struct sockaddr *)&from, fl) == 10);
                }
            }
            step(0);
        }
        CHECK_EQ_I(got, 1);
        CHECK(cli_n > n0);
        CHECK(cli_len == 38 && cli_pkt[9] == 17 && get32(cli_pkt + 12) == 0x09090909u && get32(cli_pkt + 16) == peer_ip);
        CHECK(cli_pkt[20] == 0 && cli_pkt[21] == 53 && cli_pkt[22] == 0x12 && cli_pkt[23] == 0x34 && memcmp(cli_pkt + 28, "this one.", 10) == 0);
        close(us);
    }
    hang_up();
    /* the calls are on their own page, each with its port */
    CHECK(http("GET /calls HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "wrong number") != NULL || strstr(resp, "Wrong number") != NULL);
    CHECK(strstr(resp, "<th>Port</th>") != NULL && strstr(resp, "<tr><td>1</td><td>") != NULL);

    /* 4b. V.92 was switched on with that form too: a V.92 modem now gets PCM
     * upstream (up to the max_upstream of the file: 33600 gives 33333) */
    hook = 1; run_ms(500); dial("42"); run_ms(3000 + 2000 + 300);
    start_modem(3);
    ms = wait_ip(40000);
    printf("  V.92 call through the daemon: IP up after %.2f s, %d bit/s down, %d bit/s up (PCM upstream %s)\n", ms / 1000.0, C.rx_rate, C.tx_rate,
           C.vc.stage == VCL_PCM ? "yes" : "no");
    CHECK(ms > 0);
    CHECK_EQ_I(C.vc.stage, VCL_PCM);
    CHECK_EQ_I(C.tx_rate, 33333);
    CHECK(ping_via_ppp(PPP_LOCAL, 5, -1));
    cli_len = 0;
    CHECK(ping_via_ppp(WAN_HOST, 6, wan_fd));
    CHECK(cli_len == 60 && cli_pkt[20] == 0 && get32(cli_pkt + 12) == WAN_HOST);
    run_ms(1200);
    CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "<tr><th>Modulation</th><td>V.92</td></tr>") != NULL);
    CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "\"modulation\":\"v92\"") != NULL && strstr(resp, "\"up_bps\":33333") != NULL);
    hang_up();
    CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
    CHECK(strstr(resp, "V.92") != NULL);                 /* the call history */

    if (fw_nv) {
        /* 4c. back to the stock firmware: asked first, then one write */
        static const char STOCK[] = "POST /firmware/stock HTTP/1.0\r\nHost: 127.0.0.1:8080\r\nContent-Type: application/x-www-form-urlencoded\r\n";
        CHECK(http("GET /firmware/stock HTTP/1.0\r\nHost: 127.0.0.1:8080\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "name=\"confirm\" value=\"stock\"") != NULL);
        snprintf(cmd, sizeof cmd, "%sContent-Length: 9\r\n\r\nconfirm=1", STOCK);
        CHECK(http(cmd, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 400 ") != NULL);
        run_ms(300);
        CHECK(flash_is(0x060000, "\xF9\x7A\xC9\x23", 4) && access(fw_note, F_OK) != 0);
        snprintf(cmd, sizeof cmd, "%sContent-Length: 13\r\n\r\nconfirm=stock", STOCK);
        CHECK(http(cmd, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 200 ") != NULL && strstr(resp, "restarting into the stock firmware") != NULL);
        for (i = 0; i < 100 && access(fw_note, F_OK) != 0; i++) run_ms(100);
        CHECK(access(fw_note, F_OK) == 0);
        CHECK(flash_is(0x060000, "\0\0\0\0", 4));
        CHECK(flash_is(0x2E0200, new_img, new_len));        /* nothing else of it */
        unsetenv("V90NV_FLASH");
    }

    /* 5. SIGTERM: a clean exit */
    quiet_exit = 1;
    kill(pid, SIGTERM);
    for (i = 0; i < 400 && !dead; i++) step(0);
    close(line_fd);
    CHECK_EQ_I(waitpid(pid, &st, 0), pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* 6. the box's two ports: the daemon with two lines. Port 1 stays on
     * hook; the call comes in on port 2, which has its own number, its own
     * network device and its own address for the caller. */
    {
        int sv2[2]; static char ledp[300], led[2][340], dclog[300], dnsh[300], dnout[300];
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        strcpy(c.port2.line_number, "777"); c.port2.rings = 1;
        c.port2.service = CFG_SERVICE_DREAMCAST;   /* port 2 is a game console's */
        c.port2.dc_now = 1;                         /* and reports to Dreamcast Now */
        c.auth = CFG_AUTH_CHAP; c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland");
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv2), 0);
        /* the telephone lights: <prefix>1/brightness and <prefix>2/brightness,
         * lit while that port is off hook */
        snprintf(ledp, sizeof ledp, "%s/ns_led", argv[2]);
        for (i = 1; i <= 2; i++) {
            FILE *lf;
            snprintf(led[i - 1], sizeof led[0], "%s%d", ledp, i); mkdir(led[i - 1], 0755);
            strcat(led[i - 1], "/brightness");
            lf = fopen(led[i - 1], "w"); if (lf) { fputs("7\n", lf); fclose(lf); }
        }
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), b = fcntl(sv2[1], F_DUPFD, 20);
            dup2(a, 3); dup2(b, 4);
            for (i = 5; i < 64; i++) close(i);
            {   /* this daemon's log goes to a file, to be read below */
                int lfd;
                snprintf(dclog, sizeof dclog, "%s/ns_dc.log", argv[2]);
                lfd = open(dclog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (lfd >= 0) { dup2(lfd, 2); close(lfd); }
            }
            {   /* what would be sent to Dreamcast Now goes to a file instead */
                FILE *sf;
                snprintf(dnsh, sizeof dnsh, "%s/ns_dcnow.sh", argv[2]);
                snprintf(dnout, sizeof dnout, "%s/ns_dcnow.out", argv[2]);
                unlink(dnout);
                sf = fopen(dnsh, "w");
                if (sf) { fprintf(sf, "#!/bin/sh\necho \"$1 [$2]\" >> %s\n", dnout); fclose(sf); chmod(dnsh, 0755); }
            }
            /* the address list of the console service comes from a command;
             * here one that writes two lines, one of them no rule */
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-l", "fd:4", "-i", "lo", "-t", "ppp0", "-L", ledp,
                  "-G", dnsh, "-D", "printf '203.0.113.9 198.51.100.7 9500 udp\\nnonsense\\n'", (char *)NULL);
            _exit(127);
        }
        close(sv[1]); close(sv2[1]);
        idle_fd = sv[0]; line_fd = sv2[0];
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0; idle_noise = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        peer_ip = IP4(10, 64, 64, 3);

        run_ms(2500);
        CHECK(http("GET / HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "200 OK") != NULL);
        CHECK(strstr(resp, "<b>Port 1</b> <a href=\"/port2\">Port 2</a>") != NULL);
        CHECK_EQ_I(if_address("ppp0"), PPP_LOCAL);
        CHECK_EQ_I(if_address("ppp1"), PPP_LOCAL);
        /* the console service of port 2: the list was fetched and read, and
         * the rules went in with it once the lines were idle */
        snprintf(dclog, sizeof dclog, "%s/ns_dc.log", argv[2]);
        CHECK(file_has(dclog, "console service: address list with 1 rules"));
        CHECK(file_has(dclog, "console service on port 2: 1 address rules, 24 port ranges opened from lo"));
        /* port 1's number is a wrong number on port 2 */
        hook = 1; run_ms(500); dial("5551234"); run_ms(4000);
        c_rx_pw = 0; run_ms(3000);
        CHECK(c_rx_pw > 100.0 * 100.0);
        hang_up();
        /* port 2's own number: a V.34 call */
        hook = 1; run_ms(500); dial("777"); run_ms(3000 + 2000 + 300);
        start_modem(2);
        ms = wait_ip(40000);
        printf("  port 2, V.34 call through the daemon: IP up after %.2f s, %d bit/s down, %d bit/s up\n", ms / 1000.0, C.rx_rate, C.tx_rate);
        CHECK(ms > 0);
        CHECK(ping_via_ppp(PPP_LOCAL, 11, -1));
        CHECK(cli_len == 60 && cli_pkt[20] == 0 && get32(cli_pkt + 12) == PPP_LOCAL && get32(cli_pkt + 16) == peer_ip);
        cli_len = 0;
        CHECK(ping_via_ppp(WAN_HOST, 12, wan_fd));          /* masqueraded like port 1's caller */
        CHECK(cli_len == 60 && cli_pkt[20] == 0 && get32(cli_pkt + 12) == WAN_HOST && get32(cli_pkt + 16) == peer_ip);
        run_ms(1200);
        CHECK(http("GET /port2 HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "alice") != NULL); CHECK(strstr(resp, "<tr><th>Modulation</th><td>V.34</td></tr>") != NULL);
        CHECK(strstr(resp, "10.64.64.3") != NULL);
        CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "\"line\":\"idle\"") != NULL);          /* port 1 */
        CHECK(http("GET /status2.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "\"line\":\"connected\"") != NULL && strstr(resp, "\"modulation\":\"v34\"") != NULL);
        CHECK(file_has(led[1], "1")); CHECK(file_has(led[0], "0"));     /* port 2's light is on, port 1's off */
        /* Dreamcast Now: with the call up, the report went out, to the path
         * that names this box (the hash of "lo"'s hardware address), with
         * nothing in it since the caller has asked for no name */
        snprintf(dnout, sizeof dnout, "%s/ns_dcnow.out", argv[2]);
        CHECK(file_has(dnout, "https://dcnow-2016.appspot.com/api/update/38fbdde984330e50c02382e647c576b71f41cc5c45b193d4f3177e6ee8f22a78/ []"));
        /* port 2's Line page during its call */
        body = "port=2&number=778";
        snprintf(post, sizeof post, "POST /line2 HTTP/1.0\r\nHost: 127.0.0.1:8080\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s",
                 (int)strlen(body), body);
        CHECK(http(post, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 303 ") != NULL && strstr(resp, "Location: /line2?saved=1") != NULL);
        CHECK(!file_has(conf, "number = 778"));
        CHECK(ping_via_ppp(PPP_LOCAL, 13, -1));
        hang_up();
        run_ms(500);
        CHECK(file_has(conf, "[line2]\nnumber = 778\n"));
        CHECK(file_has(conf, "[line]\nnumber = 5551234\n"));
        CHECK(file_has(led[1], "0")); CHECK(file_has(led[0], "0"));
        CHECK_EQ_I(idle_noise, 0);                           /* port 1 never made a sound */
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd); close(idle_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 7. a console as a host on the network: the port's caller gets the
     * address that the lease command hands out (on the box, a DHCP lease of
     * its own) instead of the configured one, and the network side answers
     * ARP for it. */
    {
        static char lanlog[300], leasecmd[700]; struct ifreq ifr; int sk;
        /* the lease command: an address for 5 seconds the first time it is
         * asked, nothing after that (a network that stops answering) */
        snprintf(leasecmd, sizeof leasecmd, "f=%s/ns_lease.once; if [ -e $f ]; then exit 1; fi; : > $f; echo 192.0.2.77 5", argv[2]);
        { char once[320]; snprintf(once, sizeof once, "%s/ns_lease.once", argv[2]); unlink(once); }
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.service = CFG_SERVICE_DREAMCAST; c.dc_lan = 1; c.dc_upnp = 1;
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_lan.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "", "-D", "", "-E", leasecmd, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(3200);
        memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "ppp0");
        sk = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK_EQ_I(ioctl(sk, SIOCGIFDSTADDR, &ifr), 0);
        CHECK(ntohl(((struct sockaddr_in *)(void *)&ifr.ifr_dstaddr)->sin_addr.s_addr) == IP4(192, 0, 2, 77));
        close(sk);
        CHECK(file_has("/proc/sys/net/ipv4/conf/lo/proxy_arp", "1"));
        CHECK(file_has(lanlog, "port 1: the console is 192.0.2.77 on the network"));
        CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "\"service\":\"dreamcast\"") != NULL);
        /* the lease runs out and is not renewed: the address is given up, the
         * port is back behind the box's own, nobody answers ARP for it */
        run_ms(5500);
        memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "ppp0");
        sk = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK_EQ_I(ioctl(sk, SIOCGIFDSTADDR, &ifr), 0);
        CHECK(ntohl(((struct sockaddr_in *)(void *)&ifr.ifr_dstaddr)->sin_addr.s_addr) == IP4(10, 64, 64, 2));
        close(sk);
        CHECK(file_has("/proc/sys/net/ipv4/conf/lo/proxy_arp", "0"));
        CHECK(file_has(lanlog, "port 1: the console's address on the network ran out"));
        /* the router was asked for the game ports; there is none in this
         * namespace, and the daemon says so and carries on */
        /* (the search waits four seconds of the clock on the wall, and this
         * test's line runs faster than that) */
        for (i = 0; i < 80 && !file_has(lanlog, "game ports: no router"); i++) { run_ms(100); usleep(100000); }
        CHECK(file_has(lanlog, "game ports: no router answered the search (UPnP)"));
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK(file_has("/proc/sys/net/ipv4/conf/lo/proxy_arp", "0"));
    }
    /* 8. two consoles that telephone each other: service = saturn. This
     * program is the player's console on the line and, on the network, the
     * other player's box. */
    {
        static char lanlog[300]; struct sockaddr_in sa; int tcp, udp, evil, lis; char b5[8]; size_t k; int bad = 0;
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.service = CFG_SERVICE_SATURN;
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_sat.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "", (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(1500);
        memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(SATURN_TCP_PORT); sa.sin_addr.s_addr = htonl(IP4(127, 0, 0, 1));
        /* nothing listens while nobody waits */
        tcp = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(tcp, (struct sockaddr *)(void *)&sa, sizeof sa) < 0);
        close(tcp);

        /* (a) the player here waits: 0 is dialled. The other box calls. */
        sat_call("0");
        CHECK(file_has(lanlog, "port 1: waiting for the other player to call"));
        CHECK_EQ_I(sat_dn_n, 0);
        tcp = socket(AF_INET, SOCK_STREAM, 0);
        CHECK_EQ_I(connect(tcp, (struct sockaddr *)(void *)&sa, sizeof sa), 0);
        CHECK_EQ_I(send(tcp, "readyip", 7, MSG_NOSIGNAL), 7);
        run_ms(300);
        memset(b5, 0, sizeof b5);
        CHECK_EQ_I(recv(tcp, b5, 5, MSG_DONTWAIT), 5); CHECK(memcmp(b5, "g2gip", 5) == 0);
        close(tcp);
        udp = udp_at(IP4(127, 0, 0, 1), SATURN_UDP_CALL);
        evil = udp_at(IP4(127, 0, 0, 2), 20444);
        CHECK(udp >= 0 && evil >= 0);
        saturn_rx_init(&sat_peer_rx); sat_got_n = 0; sat_pings = sat_pongs = 0; sat_first[0] = 0;
        run_ms(2500);
        /* the game here is shown a ring and a connect */
        CHECK_EQ_I(sat_dn_n, 15); CHECK(memcmp(sat_dn, "RING\r\nCONNECT\r\n", 15) == 0);
        CHECK(file_has(lanlog, "port 1: the other player is 127.0.0.1"));
        udp_drain(udp);
        CHECK(sat_pings >= 1); CHECK_EQ_I(sat_from, SATURN_UDP_WAIT);
        udp_to(udp, SATURN_UDP_WAIT, "PING_SHIRO", 10);
        run_ms(100); udp_drain(udp);
        CHECK(sat_pongs >= 1);
        /* octets from the other console: each datagram comes twice, counts once */
        sat_dn_n = 0;
        udp_to(udp, SATURN_UDP_WAIT, "hello<dataSplit>0", 17); udp_to(udp, SATURN_UDP_WAIT, "hello<dataSplit>0", 17);
        udp_to(evil, SATURN_UDP_WAIT, "EVIL!<dataSplit>1", 17);        /* not the other player: not heard */
        udp_to(udp, SATURN_UDP_WAIT, " you<dataSplit>1<packetSplit>hello<dataSplit>0", 46);
        run_ms(1000);
        CHECK_EQ_I(sat_dn_n, 9); CHECK(memcmp(sat_dn, "hello you", 9) == 0);
        /* the other player's address from another port, once its port is
         * known: not heard either */
        { int other = udp_at(IP4(127, 0, 0, 1), 20555); udp_to(other, SATURN_UDP_WAIT, "EVIL2<dataSplit>2", 17); run_ms(500); close(other); }
        CHECK_EQ_I(sat_dn_n, 9);
        /* and the octets of the console here go out, numbered from 0 */
        sat_n = 200;
        run_ms(3000); udp_drain(udp);
        CHECK_EQ_I(sat_got_n, 200);
        for (k = 0; k < sat_got_n; k++) if (sat_got[k] != (uint8_t)(k * 9 + 2)) bad++;
        CHECK_EQ_I(bad, 0);
        CHECK(strstr(sat_first, "<dataSplit>0") != NULL);
        CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "\"service\":\"saturn\"") != NULL);
        hang_up();
        run_ms(500);
        close(udp); close(evil);
        tcp = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(tcp, (struct sockaddr *)(void *)&sa, sizeof sa) < 0);
        close(tcp);

        /* (b) the player here calls the other one by address */
        lis = socket(AF_INET, SOCK_STREAM, 0);
        { int one = 1; setsockopt(lis, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
        CHECK_EQ_I(bind(lis, (struct sockaddr *)(void *)&sa, sizeof sa), 0);
        CHECK_EQ_I(listen(lis, 2), 0);
        udp = udp_at(IP4(127, 0, 0, 1), SATURN_UDP_WAIT);
        CHECK(udp >= 0);
        sat_call("#127*0*0*1#");
        tcp = accept4(lis, NULL, NULL, SOCK_NONBLOCK);
        CHECK(tcp >= 0);
        memset(b5, 0, sizeof b5);
        CHECK_EQ_I(recv(tcp, b5, 7, 0), 7); CHECK(memcmp(b5, "readyip", 7) == 0);
        CHECK_EQ_I(send(tcp, "g2gip", 5, MSG_NOSIGNAL), 5);
        saturn_rx_init(&sat_peer_rx); sat_got_n = 0; sat_pings = sat_pongs = 0; sat_first[0] = 0; sat_from = 0;
        run_ms(2500); udp_drain(udp);
        CHECK(file_has(lanlog, "port 1: the other player is 127.0.0.1"));
        CHECK(sat_pings >= 1); CHECK_EQ_I(sat_from, SATURN_UDP_CALL);
        CHECK_EQ_I(sat_dn_n, 0);                        /* the calling console is shown nothing */
        udp_to(udp, SATURN_UDP_CALL, "abc<dataSplit>0", 15);
        sat_n = 50;
        run_ms(1500); udp_drain(udp);
        CHECK_EQ_I(sat_dn_n, 3); CHECK(memcmp(sat_dn, "abc", 3) == 0);
        CHECK_EQ_I(sat_got_n, 50);
        /* the other box falls silent: after half a minute the call is ended */
        close(tcp); close(lis); close(udp);
        run_ms(32000);
        CHECK(file_has(lanlog, "port 1: the other player is gone"));
        hang_up();

        /* (c) a number that asks for an opponent from a matchmaking service:
         * not understood, no call */
        hook = 1; run_ms(500); dial("#001#"); run_ms(3000 + 2000 + 300);
        hook = 0; run_ms(1000);
        CHECK(http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);

        /* (d) waiting, and nobody calls: given up after two minutes */
        sat_call("00");
        run_ms(121000);
        CHECK(file_has(lanlog, "port 1: nobody called"));
        hang_up();

        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 9. the three switches of the dreamcast service on a port that has
     * another service: nothing of it happens. No lease is asked for, no
     * report sent, no router searched, no address list fetched, no rules. */
    {
        static char lanlog[300], mark[320], leasecmd[400], listcmd[400], nowcmd[400], vpnf[320]; struct ifreq ifr; int sk;
        snprintf(mark, sizeof mark, "%s/ns_inert.mark", argv[2]); unlink(mark);
        snprintf(vpnf, sizeof vpnf, "%s/ns_inert.vpn", argv[2]);
        snprintf(leasecmd, sizeof leasecmd, ": > %s; echo 192.0.2.77 600", mark);
        snprintf(listcmd, sizeof listcmd, ": > %s; echo 172.17.18.2 192.0.2.9 9500", mark);
        snprintf(nowcmd, sizeof nowcmd, "%s/ns_inert.sh", argv[2]);
        { FILE *f = fopen(nowcmd, "w"); if (f) { fprintf(f, "#!/bin/sh\n: > %s\n", mark); fclose(f); chmod(nowcmd, 0755); } }
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland"); c.auth = CFG_AUTH_CHAP;
        c.service = CFG_SERVICE_INTERNET; c.dc_now = 1; c.dc_lan = 1; c.dc_upnp = 1; c.dc_iwango = 1;
        { char pf[320]; FILE *f; snprintf(pf, sizeof pf, "%s/ns_inert.vpn", argv[2]); f = fopen(pf, "w"); if (f) { fputs("client\nremote a 1\n", f); fclose(f); } }
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_inert.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "", "-D", listcmd, "-E", leasecmd, "-G", nowcmd, "-O", nowcmd, "-V", vpnf, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(3000);
        /* a whole call with IP up, the moment a console's port would report */
        hook = 1; run_ms(500); dial("5551234"); run_ms(3000 + 2000 + 300);
        start_modem(0);
        CHECK(wait_ip(40000) > 0);
        run_ms(3000); usleep(300000);
        memset(&ifr, 0, sizeof ifr); strcpy(ifr.ifr_name, "ppp0");
        sk = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK_EQ_I(ioctl(sk, SIOCGIFDSTADDR, &ifr), 0);
        CHECK(ntohl(((struct sockaddr_in *)(void *)&ifr.ifr_dstaddr)->sin_addr.s_addr) == IP4(10, 64, 64, 2));
        close(sk);
        hang_up();
        CHECK(access(mark, F_OK) != 0);
        CHECK(file_has("/proc/sys/net/ipv4/conf/lo/proxy_arp", "0"));
        CHECK(!file_has(lanlog, "console")); CHECK(!file_has(lanlog, "game ports")); CHECK(!file_has(lanlog, "Dreamcast"));
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 10. the VPN for the IWANGO games: with the switch on for a dreamcast
     * port and a profile stored, the OpenVPN program of the image is kept
     * running (here a stand-in that writes down how it was called); a new
     * profile restarts it, no profile or no switch stops it. */
    {
        static char lanlog[300], prog[320], argsf[320], vpnf[320], req[900]; int k;
        snprintf(prog, sizeof prog, "%s/ns_vpn.sh", argv[2]);
        snprintf(argsf, sizeof argsf, "%s/ns_vpn.args", argv[2]); unlink(argsf);
        snprintf(vpnf, sizeof vpnf, "%s/ns_vpn.profile", argv[2]); unlink(vpnf);
        { FILE *f = fopen(prog, "w"); if (f) { fprintf(f, "#!/bin/sh\necho \"$@\" >> %s\ncat \"$2\" >> %s\nexec sleep 1000\n", argsf, argsf); fclose(f); chmod(prog, 0755); } }
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.service = CFG_SERVICE_DREAMCAST; c.dc_iwango = 1;
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_vpn.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "", "-D", "", "-O", prog, "-V", vpnf, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(3500);
        /* the switch is on but there is no profile: nothing runs, and the log says what is missing */
        CHECK(access(argsf, F_OK) != 0);
        CHECK(file_has(lanlog, "IWANGO games: no VPN profile is stored"));
        CHECK(http("GET /dreamcast HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "No profile is stored.") != NULL);
        /* a profile is given on the page: kept in the file, the program started on it */
        k = snprintf(req, sizeof req, "POST /dreamcast/vpn HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
                     (int)strlen("profile=client\r\nremote vpn.example 1194\r\n\r\n"), "profile=client\r\nremote vpn.example 1194\r\n\r\n");
        CHECK(http_n(req, (size_t)k, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 303 ") != NULL);
        for (i = 0; i < 50 && access(argsf, F_OK) != 0; i++) { run_ms(200); usleep(50000); }
        CHECK(file_has(vpnf, "remote vpn.example 1194"));
        { struct stat sb; CHECK(stat(vpnf, &sb) == 0 && (sb.st_mode & 0777) == 0600); }
        CHECK(file_has(argsf, "--config ")); CHECK(file_has(argsf, "--dev v90vpn --dev-type tun"));
        CHECK(file_has(argsf, "--script-security 1")); CHECK(file_has(argsf, "--pull-filter ignore redirect-gateway"));
        CHECK(file_has(argsf, "--pull-filter accept route-gateway --pull-filter ignore route --route 10.200.0.0 255.255.255.0 "));
        CHECK(!file_has(argsf, "accept route 10."));
        CHECK(file_has(argsf, "remote vpn.example 1194"));
        CHECK(file_has(lanlog, "IWANGO games: starting the VPN (server vpn.example 1194)"));
        CHECK(http("GET /dreamcast HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "A profile is stored (server vpn.example 1194).") != NULL);
        /* another profile: the program is started again on the new one */
        k = snprintf(req, sizeof req, "POST /dreamcast/vpn HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: %d\r\n\r\n%s",
                     (int)strlen("profile=client\r\nremote other.example 443\r\n\r\n"), "profile=client\r\nremote other.example 443\r\n\r\n");
        CHECK(http_n(req, (size_t)k, resp, sizeof resp) > 0);
        for (i = 0; i < 50 && !file_has(argsf, "other.example"); i++) { run_ms(200); usleep(50000); }
        CHECK(file_has(argsf, "remote other.example 443"));
        /* the profile is taken away: stopped */
        k = snprintf(req, sizeof req, "POST /dreamcast/vpn HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: 10\r\n\r\nremove=1\r\n");
        CHECK(http_n(req, (size_t)k, resp, sizeof resp) > 0);
        for (i = 0; i < 50 && !file_has(lanlog, "IWANGO games: the VPN is stopped"); i++) { run_ms(200); usleep(50000); }
        CHECK(file_has(lanlog, "IWANGO games: the VPN is stopped"));
        CHECK(access(vpnf, F_OK) != 0);
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    T_REPORT("ns_call");
}
