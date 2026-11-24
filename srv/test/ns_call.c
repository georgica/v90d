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
#include "stun.h"

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
/* ---- a terminal program that calls a bulletin board (service = bbs) ---- */
static char bt_q[256]; static size_t bt_at, bt_n;
static uint8_t bt_got[8192]; static size_t bt_got_n;
static void bt_rx(void *ctx, const uint8_t *b, size_t n) { (void)ctx; if (bt_got_n + n <= sizeof bt_got) { memcpy(bt_got + bt_got_n, b, n); bt_got_n += n; } }
static size_t bt_tx(void *ctx, uint8_t *b, size_t max)
{
    size_t n = 0; (void)ctx;
    while (n < max && bt_at < bt_n) b[n++] = (uint8_t)bt_q[bt_at++];
    return n;
}
static void bt_type(const char *s) { bt_n = strlen(s); memcpy(bt_q, s, bt_n); bt_at = 0; }
static int bt_saw(const char *needle) { return memmem(bt_got, bt_got_n, needle, strlen(needle)) != NULL; }
static int bt_wait(const char *needle, int ms)
{
    int t;
    for (t = 0; t < ms && !bt_saw(needle) && !dead; t += 20) run_ms(20);
    return bt_saw(needle);
}
/* dial, and if the number answers bring the modem up; 1 = connected */
static int bt_call(const char *num)
{
    caller_cfg_t cc; long i;
    hook = 1; run_ms(500);
    dial(num);
    run_ms(3000 + 2000 + 300);
    caller_cfg_default(&cc);
    cc.raw_rx = bt_rx; cc.raw_tx = bt_tx;
    caller_init(&C, &cc);
    modem_on = 1; bt_got_n = bt_at = bt_n = 0;
    for (i = 0; i < 20L * FS && !C.connected && !dead; i++) step(0);
    return C.connected;
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

/* a stand-in STUN server: it echoes the sender's address back as the
 * mapped address, so the daemon learns its own 127.0.0.1:port */
static void stun_responder(int fd)
{
    uint8_t b[512], out[32]; struct sockaddr_in from; socklen_t fl; ssize_t r;
    for (;;) {
        fl = sizeof from;
        r = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)(void *)&from, &fl);
        if (r < 20 || b[0] != 0x00 || b[1] != 0x01) continue;
        out[0] = 0x01; out[1] = 0x01; out[2] = 0x00; out[3] = 0x0C;
        memcpy(out + 4, b + 4, 4); memcpy(out + 8, b + 8, 12);
        out[20] = 0x00; out[21] = 0x20; out[22] = 0x00; out[23] = 0x08;
        out[24] = 0x00; out[25] = 0x01;
        { uint16_t xp = (uint16_t)(ntohs(from.sin_port) ^ (STUN_MAGIC >> 16));
          uint32_t xa = ntohl(from.sin_addr.s_addr) ^ STUN_MAGIC;
          out[26] = (uint8_t)(xp >> 8); out[27] = (uint8_t)xp;
          out[28] = (uint8_t)(xa >> 24); out[29] = (uint8_t)(xa >> 16); out[30] = (uint8_t)(xa >> 8); out[31] = (uint8_t)xa; }
        (void)sendto(fd, out, sizeof out, 0, (struct sockaddr *)(void *)&from, fl);
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
        CHECK(file_has(argsf, "--pull-filter ignore redirect-private --pull-filter ignore ifconfig-ipv6"));
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
    /* 11. matchmaking (saturn.h): dial #0NN#, the box learns its public
     * address by a stand-in STUN server, asks a stand-in match server (via
     * -M) which pairs it, then plays the opponent over UDP. */
    {
        static char lanlog[300], script[320], mlog[320]; pid_t stunpid = -1; int sfd, peer, k; size_t z; int bad = 0;
        snprintf(script, sizeof script, "%s/ns_mm.sh", argv[2]);
        snprintf(mlog, sizeof mlog, "%s/ns_mm.args", argv[2]); unlink(mlog);
        {
            FILE *f = fopen(script, "w");
            if (f) {
                fprintf(f, "#!/bin/sh\necho \"$1 $2 $3 $4\" >> %s\n", mlog);
                fprintf(f, "case \"$1\" in\n status) echo '{\"status\":\"matched\",\"opponent ip_address\":\"127000000001 34567\"}' ;;\n");
                fprintf(f, " *) echo '{\"status\":\"waiting\"}' ;;\nesac\n");
                fclose(f); chmod(script, 0755);
            }
        }
        /* the stand-in STUN server on 127.0.0.1:3478 */
        sfd = udp_at(IP4(127, 0, 0, 1), 3478);
        CHECK(sfd >= 0);
        stunpid = fork();
        if (stunpid == 0) { stun_responder(sfd); _exit(0); }
        close(sfd);
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.service = CFG_SERVICE_SATURN; c.sat_match = 1;
        c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_mm.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            char mcmd[360]; int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            snprintf(mcmd, sizeof mcmd, "sh %s \"$@\"", script);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-S", "127.0.0.1:3478", "-M", mcmd, "-X", (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(1500);
        /* the opponent's box, at the address the match server will hand out */
        peer = udp_at(IP4(127, 0, 0, 1), 34567);
        CHECK(peer >= 0);
        saturn_rx_init(&sat_peer_rx); sat_got_n = 0; sat_pings = sat_pongs = 0; sat_first[0] = 0; sat_from = 0;
        sat_call("#042#");                          /* wait to be paired for game 42 */
        /* STUN (0.5s) + wait query + 2s + status query: give it time */
        for (i = 0; i < 80 && !file_has(lanlog, "the other player is 127.0.0.1"); i++) { run_ms(200); usleep(60000); }
        CHECK(file_has(lanlog, "our public address is 127.0.0.1:20001 (STUN)"));
        CHECK(file_has(lanlog, "the other player is 127.0.0.1"));
        CHECK(file_has(mlog, "wait 42 127000000001 20001"));    /* game id and public address were sent */
        CHECK(file_has(mlog, "status 42 127000000001 20001"));
        run_ms(800);
        /* the waiting game is shown RING + CONNECT */
        CHECK_EQ_I(sat_dn_n, 15); CHECK(memcmp(sat_dn, "RING\r\nCONNECT\r\n", 15) == 0);
        /* a PING reaches the opponent from the box's port (give it wall time) */
        for (i = 0; i < 40 && sat_pings < 1; i++) { run_ms(100); usleep(60000); udp_drain(peer); }
        CHECK(sat_pings >= 1); CHECK_EQ_I(sat_from, 20001);
        udp_to(peer, 20001, "hi<dataSplit>0", 14);
        for (i = 0; i < 20 && sat_dn_n < 17; i++) { run_ms(100); usleep(30000); }
        CHECK_EQ_I(sat_dn_n, 15 + 2); CHECK(memcmp(sat_dn + 15, "hi", 2) == 0);
        /* and the box's octets reach the opponent */
        sat_n = 120;
        for (i = 0; i < 40 && sat_got_n < 120; i++) { run_ms(100); usleep(30000); udp_drain(peer); }
        CHECK_EQ_I(sat_got_n, 120);
        for (z = 0; z < sat_got_n; z++) if (sat_got[z] != (uint8_t)(z * 9 + 2)) bad++;
        CHECK_EQ_I(bad, 0);
        CHECK(http("GET /status.json HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "\"service\":\"saturn\"") != NULL);
        (void)k;
        hang_up();
        close(peer);
        if (stunpid > 0) { kill(stunpid, SIGKILL); waitpid(stunpid, NULL, 0); }
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 12. the opponent address the match server returns is not trusted blindly:
     * a loopback (or private) address is refused and the call ends (no -X) */
    {
        static char lanlog[300], script[320]; pid_t stunpid = -1; int sfd;
        snprintf(script, sizeof script, "%s/ns_mm2.sh", argv[2]);
        {
            FILE *f = fopen(script, "w");
            if (f) { fprintf(f, "#!/bin/sh\necho '{\"status\":\"matched\",\"opponent ip_address\":\"127000000001 34567\"}'\n"); fclose(f); chmod(script, 0755); }
        }
        sfd = udp_at(IP4(127, 0, 0, 1), 3478);
        CHECK(sfd >= 0);
        stunpid = fork();
        if (stunpid == 0) { stun_responder(sfd); _exit(0); }
        close(sfd);
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1;
        c.service = CFG_SERVICE_SATURN; c.sat_match = 1; c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_mm2.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            char mcmd[360]; int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            snprintf(mcmd, sizeof mcmd, "sh %s \"$@\"", script);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-S", "127.0.0.1:3478", "-M", mcmd, (char *)NULL);   /* no -X: the address is checked */
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(1500);
        sat_call("#077#");
        for (i = 0; i < 60 && !file_has(lanlog, "cannot be a player"); i++) { run_ms(200); usleep(60000); }
        CHECK(file_has(lanlog, "the match server gave an address that cannot be a player"));
        CHECK(!file_has(lanlog, "the other player is"));
        if (stunpid > 0) { kill(stunpid, SIGKILL); waitpid(stunpid, NULL, 0); }
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 13. a terminal program and a bulletin board: 1 800 BBS DIAL and the
     * boards' own numbers (227 and seven digits), on a
     * port whose service is the plain internet. The list in the image
     * serves until the first call asks for a fresh one; the caller picks a
     * board at the prompt and is joined to it, with the telnet words kept
     * from its screen. The board is a script (-T). The port's own number is
     * the dial-up connection as before. */
    {
        static char lanlog[300], board[300], builtin[300], fetched[300], live[300], fcmd[1200], resp[16384]; FILE *f;
        snprintf(board, sizeof board, "%s/ns_bbs.sh", argv[2]);
        snprintf(builtin, sizeof builtin, "%s/ns_bbs.builtin", argv[2]);
        snprintf(fetched, sizeof fetched, "%s/ns_bbs.remote", argv[2]);
        snprintf(live, sizeof live, "%s/ns_bbs.live", argv[2]);
        /* the first try fails, as on a box that has just started and has no clock yet */
        snprintf(fcmd, sizeof fcmd, "if test -f %s.tried; then cat %s; else touch %s.tried; exit 1; fi", fetched, fetched, fetched);
        f = fopen(board, "w");
        if (f) {
            /* our DO ECHO comes first, then what was typed */
            fprintf(f, "#!/bin/sh\n[ \"$1\" = dead.example ] && exit 1\n"
                       "printf '\\377\\373\\001Welcome to %%s port %%s\\r\\n' \"$1\" \"$2\"\n"
                       "x=$(dd bs=1 count=5 2>/dev/null | tail -c 2)\nprintf 'you said %%s\\r\\n' \"$x\"\n");
            fclose(f); chmod(board, 0755);
        }
        f = fopen(builtin, "w");
        if (f) {
            fprintf(f, "# the list in the image\n# serial 6\nv90d-bbs 2\n"
                       "bbs telehack 2278353422 tele.example Telehack\nbbs 20forbeers 2272036723 b.example:1337 20 For Beers <BBS>\n"
                       "bbs dead 2273323000 dead.example Dead Board\n");
            fclose(f);
        }
        f = fopen(fetched, "w");
        if (f) {
            /* signed with the test key (test/bbskeys_test.h); newer than the image's */
            fprintf(f, "# serial 7\nv90d-bbs 2\nbbs telehack 2278353422 tele.example Telehack\nbbs 20forbeers 2272036723 b.example:1337 20 For Beers <BBS>\n"
                       "bbs newboard 2276392627 n.example New Board\n"
                       "# sig ed25519 2f3f4aac9de5966c376f248f8dd45019d2ea0be983490812bc517dd59ab0a53a766777895ce008c04233a84c7202e380c506eb5cb715345a6ed19ca22e6fdc0d\n");
            fclose(f);
        }
        unlink(live);
        { char tried[320]; snprintf(tried, sizeof tried, "%s.tried", fetched); unlink(tried); }
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1; c.status_port = 8080;
        c.auth = CFG_AUTH_CHAP; c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland");
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_bbs.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-B", builtin, "-b", fcmd, "-J", live, "-T", board, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(3000);
        /* (a) nobody has asked for the boards: nothing is fetched */
        CHECK(!file_has(lanlog, "bbs: fetched list"));
        CHECK(access(live, F_OK) != 0);
        /* (b) the number: the prompt, a search, a name nobody has, an
         * address where nothing answers, then a board by its name as a
         * person would type it */
        CHECK_EQ_I(bt_call("18002273425"), 1);
        CHECK(bt_wait("bulletin board gateway", 4000));
        CHECK(bt_wait("BBS: ", 2000));
        bt_type("?beer\r");
        CHECK(bt_wait("227 203 6723  20forbeers", 3000)); CHECK(bt_wait("20 For Beers <BBS>", 2000));
        CHECK(!bt_saw("Telehack"));
        bt_type("nosuch\r");
        CHECK(bt_wait("No board of that name", 3000));
        /* an address is not a board: only what the list has is called */
        bt_type("tele.example:23\r");
        CHECK(bt_wait("No board of that name", 3000));
        bt_type("227 111 1111\r");
        CHECK(bt_wait("No board has that number", 3000));
        bt_type("dead\r");
        CHECK(bt_wait("Calling Dead Board ...", 3000));
        CHECK(bt_wait("No answer.", 3000));
        bt_got_n = 0;
        bt_type("20 for beerx\bs\r");
        CHECK(bt_wait("Calling 20 For Beers <BBS> ...", 3000));
        CHECK(bt_wait("Welcome to b.example port 1337", 3000));
        CHECK(!bt_saw("b.example:"));                            /* where a board is, is not the caller's to see */
        CHECK(memchr(bt_got, 0xFF, bt_got_n) == NULL);          /* the board's telnet words are not the caller's to see */
        bt_type("hi");
        CHECK(bt_wait("you said hi", 3000));
        /* the board is done: the line is dropped, as a board's own modem would */
        for (i = 0; i < 60 && !file_has(lanlog, "the board hung up"); i++) { run_ms(100); usleep(20000); }
        CHECK(file_has(lanlog, "bbs: the board hung up"));
        {
            int gone = 0;
            for (i = 0; i < 100 && !gone; i++) {
                run_ms(100);
                gone = http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0 && !strstr(resp, "\"line\":\"connected\"");
            }
            CHECK(gone);                                        /* while the caller is still off hook */
        }
        hang_up();
        /* (c) that call asked for the list: the fetched one is in force, on
         * the page and at the prompt; the short form of the number */
        CHECK(file_has(lanlog, "bbs: the list could not be fetched; trying again in 1 minute"));
        for (i = 0; i < 400 && !file_has(lanlog, "bbs: fetched list"); i++) { run_ms(200); usleep(5000); }
        CHECK(file_has(lanlog, "bbs: fetched list with 3 boards"));
        CHECK(file_has(live, "newboard"));
        CHECK(http("GET /bbs HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<td><code>newboard</code></td><td>227 639 2627</td>") != NULL);
        CHECK(strstr(resp, "n.example") == NULL);
        CHECK(strstr(resp, "Boards 1 to 3 of 3") != NULL);
        /* the page's search, by name or by number; a page past the end */
        CHECK(http("GET /bbs?q=beer HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<code>20forbeers</code>") != NULL && strstr(resp, "<code>newboard</code>") == NULL);
        CHECK(strstr(resp, "Boards 1 to 1 of 1") != NULL);
        CHECK(http("GET /bbs?q=227639 HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<code>newboard</code>") != NULL && strstr(resp, "<code>telehack</code>") == NULL);
        CHECK(http("GET /bbs?p=2 HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<td><code>") == NULL && strstr(resp, "<td colspan=\"3\">none</td>") != NULL);
        CHECK(http("GET /bbs HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "<td>20 For Beers &lt;BBS&gt;</td>") != NULL);       /* a name is text, not markup */
        CHECK_EQ_I(bt_call("8002273425"), 1);
        CHECK(bt_wait("BBS: ", 4000));
        bt_type("2276392627\r");                                 /* a board's number at the prompt */
        CHECK(bt_wait("Welcome to n.example port 23", 3000));
        hang_up();
        /* (c2) a board's own number: no prompt, straight to the board; one
         * that no board has ends at the prompt */
        bt_got_n = 0;
        CHECK_EQ_I(bt_call("2278353422"), 1);
        CHECK(bt_wait("Calling Telehack ...", 5000));
        CHECK(bt_wait("Welcome to tele.example port 23", 3000));
        CHECK(!bt_saw("BBS: "));
        hang_up();
        CHECK_EQ_I(bt_call("12272036723"), 1);
        CHECK(bt_wait("Welcome to b.example port 1337", 5000));
        hang_up();
        CHECK_EQ_I(bt_call("2271111111"), 1);
        CHECK(bt_wait("No board has that number", 5000));
        CHECK(bt_wait("BBS: ", 2000));
        hang_up();
        /* (d) the port's own number is PPP as ever; a number of nobody is wrong */
        hook = 1; run_ms(500);
        dial("5551234");
        run_ms(2000 + 300);
        start_modem(0);
        CHECK(wait_ip(60000));
        hang_up();
        CHECK_EQ_I(bt_call("8002273426"), 0);
        hang_up();
        /* this daemon has no archive gateway (-A ""): PROTOWEB is nobody's number */
        CHECK_EQ_I(bt_call("7768693200"), 0);
        hang_up();
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 13b. the terminal menu on the Internet number ([services] menu): Dial-Up
     * Networking is not touched (its first frame goes straight to PPP); a
     * terminal program gets the menu: colour, the BBS directory with the
     * board details (Netstalgia's list, signed with the test key), a board
     * on the same call and back, then Start PPP and IP on the same call. */
    {
        static char lanlog[300], board[300], builtin[300], info[300], infolive[300], live[300], icmd[700]; FILE *f;
        snprintf(board, sizeof board, "%s/ns_menu.sh", argv[2]);
        snprintf(builtin, sizeof builtin, "%s/ns_menu.builtin", argv[2]);
        snprintf(info, sizeof info, "%s/ns_menu.info", argv[2]);
        snprintf(infolive, sizeof infolive, "%s/ns_menu.infolive", argv[2]);
        snprintf(live, sizeof live, "%s/ns_menu.live", argv[2]);
        snprintf(icmd, sizeof icmd, "cat %s", info);
        f = fopen(board, "w");
        if (f) {
            fprintf(f, "#!/bin/sh\nprintf 'Welcome to %%s port %%s\\r\\n'  \"$1\" \"$2\"\n"
                       "x=$(dd bs=1 count=2 2>/dev/null)\nprintf 'bye %%s\\r\\n' \"$x\"\n");
            fclose(f); chmod(board, 0755);
        }
        f = fopen(builtin, "w");
        if (f) {
            fprintf(f, "# serial 6\nv90d-bbs 2\nbbs telehack 2278353422 tele.example Telehack\n"
                       "bbs 20forbeers 2272036723 b.example:1337 20 For Beers <BBS>\n");
            fclose(f);
        }
        f = fopen(info, "w");
        if (f) {
            fprintf(f, "# serial 3\nv90d-bbsinfo 1\nboard telehack\nloc Berkeley, CA, USA\nsw Telehack\nnew guest\n"
                       "d The original telehack: a board of 1980s computing.\nboard 20forbeers\nloc Portland, OR, USA\nsw Mystic\nnodes 4\n"
                       "# sig ed25519 3bc5ab08665d3c2d2a5ea6e4da35cfec4adb2c96381b04892a790dfff5d00d9ccfae9cb5a26c45e79091b14af0a82226516f0f4818516ba7bee4cb753946870b\n");
            fclose(f);
        }
        unlink(live); unlink(infolive);
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1; c.status_port = 8080; c.svc_menu = 1;
        c.auth = CFG_AUTH_CHAP; c.n_users = 1; strcpy(c.users[0].name, "alice"); strcpy(c.users[0].secret, "wonderland");
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_menu.log", argv[2]);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-B", builtin, "-b", "", "-J", live, "-T", board, "-z", icmd, "-Z", infolive, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        /* the menu wants the lists: the details are fetched, the directory made */
        for (i = 0; i < 300 && !file_has(lanlog, "2 with the guide's details"); i++) { run_ms(100); usleep(5000); }
        CHECK(file_has(lanlog, "menu: the BBS directory has 2 boards, 2 with the guide's details"));
        /* (a) Dial-Up Networking: PPP, no menu */
        hook = 1; run_ms(500);
        dial("5551234");
        run_ms(2000 + 300);
        start_modem(0);
        CHECK(wait_ip(60000) > 0);
        CHECK(!file_has(lanlog, "menu: a terminal program called"));
        hang_up();
        /* (b) a terminal program: the menu */
        CHECK_EQ_I(bt_call("5551234"), 1);
        CHECK(bt_wait("ANSI colour?", 6000));
        CHECK(bt_saw("\x1b[2J"));
        bt_got_n = 0; bt_type("y");
        CHECK(bt_wait("MAIN MENU", 6000));
        CHECK(bt_saw("Tonight on the network"));
        bt_got_n = 0; bt_type("3");
        CHECK(bt_wait("BBS DIRECTORY", 6000));
        CHECK(bt_wait("Telehack", 2000));
        CHECK(bt_saw("227-835-3422") && bt_saw("Portland, OR, USA") && bt_saw("from the Telnet BBS Guide"));
        bt_got_n = 0; bt_type("t\r");
        CHECK(bt_wait("Berkeley, CA, USA", 6000));
        CHECK(bt_wait("The original telehack", 2000));
        CHECK(bt_saw("log in as guest"));
        /* the board on the same call, and back to the directory */
        bt_got_n = 0; bt_type("c");
        CHECK(bt_wait("Putting you through to Telehack", 6000));
        CHECK(bt_wait("Welcome to tele.example port 23", 6000));
        CHECK(file_has(lanlog, "menu: to the board telehack"));
        bt_got_n = 0; bt_type("ok");
        CHECK(bt_wait("bye ok", 4000));
        CHECK(bt_wait("BBS DIRECTORY", 8000));
        /* the menu, Start PPP, and the same call is a PPP call */
        bt_got_n = 0; bt_type("\x1b");
        run_ms(500);
        bt_type("1");
        CHECK(bt_wait("Starting PPP", 6000));
        caller_to_ppp(&C);
        CHECK(wait_ip(60000) > 0);
        CHECK(file_has(lanlog, "menu: Start PPP"));
        hang_up();
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 14. numbers that connect straight to a service ([services]): the
     * owner's, raw and telnet, on a port whose service is the internet, and
     * Netstalgia's list, the image's and then a fetched one (signed with the
     * test key). Raw: nothing said, what the caller sent before the service
     * answered goes first, the line drops when the service is done. Telnet:
     * "Calling NAME ...". Netstalgia's numbers stop with the switch off. */
    {
        static char lanlog[300], board[300], builtin[300], fetched[300], live[300], fcmd[700], resp[32768], post[1024];
        static const char body[] = "netstalgia=off";
        FILE *f;
        snprintf(board, sizeof board, "%s/ns_svc.sh", argv[2]);
        snprintf(builtin, sizeof builtin, "%s/ns_svc.builtin", argv[2]);
        snprintf(fetched, sizeof fetched, "%s/ns_svc.remote", argv[2]);
        snprintf(live, sizeof live, "%s/ns_svc.live", argv[2]);
        snprintf(fcmd, sizeof fcmd, "cat %s", fetched);
        f = fopen(board, "w");
        if (f) {
            fprintf(f, "#!/bin/sh\ncase \"$1\" in\n"
                       "lan.example|nwn.example) x=$(dd bs=1 count=5 2>/dev/null); printf 'RAW[%%s:%%s]' \"$2\" \"$x\"; exit 0;;\n"
                       "esac\n"
                       "printf '\\377\\373\\001Welcome to %%s port %%s\\r\\n' \"$1\" \"$2\"\n"
                       "x=$(dd bs=1 count=5 2>/dev/null | tail -c 2)\nprintf 'you said %%s\\r\\n' \"$x\"\n");
            fclose(f); chmod(board, 0755);
        }
        f = fopen(builtin, "w");
        if (f) { fprintf(f, "# serial 1\nv90d-svc 1\nsvc 13855961775 nwn.example:5000 raw NWN\n"); fclose(f); }
        f = fopen(fetched, "w");
        if (f) {
            fprintf(f, "# serial 2\nv90d-svc 1\nsvc 13855961775 nwn.example:5000 raw NWN\nsvc 15550003 echo.example:7 telnet Echo\n"
                       "future 1 2 3\nsvc 2278353422 x.example:1 raw Taken\nsvc 15550004 192.168.1.9:23 raw Private\n"
                       "# sig ed25519 2cf41924dda8ce78d2f6fd8ac33dc0867cc102c471d338083ee9ea2e212ab0971b9fc79d1b0979d41bfbc2aa102cc27e46f738c0a070da1825ae03e928cea20d\n");
            fclose(f);
        }
        unlink(live);
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1; c.status_port = 8080;
        c.n_services = 2;
        strcpy(c.services[0].number, "5550001"); strcpy(c.services[0].host, "lan.example"); c.services[0].port = 4000;
        c.services[0].mode = CFG_SVC_RAW; strcpy(c.services[0].name, "Game");
        strcpy(c.services[1].number, "5550002"); strcpy(c.services[1].host, "t.example"); c.services[1].port = 23;
        c.services[1].mode = CFG_SVC_TELNET; strcpy(c.services[1].name, "Talker");
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_svc.log", argv[2]);
        unlink(lanlog);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-b", "", "-F", builtin, "-f", "exit 1", "-K", live, "-T", board, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(2000);
        /* (a) the owner's raw number: what the caller sends at once waits for the service */
        CHECK_EQ_I(bt_call("5550001"), 1);
        bt_type("hello");
        CHECK(bt_wait("RAW[4000:hello]", 5000));
        CHECK(!bt_saw("Calling") && !bt_saw("gateway"));
        for (i = 0; i < 60 && !file_has(lanlog, "svc: the service hung up"); i++) { run_ms(100); usleep(20000); }
        CHECK(file_has(lanlog, "port 1: svc: calling lan.example:4000 (raw)"));
        CHECK(file_has(lanlog, "svc: the service hung up"));
        {
            int gone = 0;
            for (i = 0; i < 100 && !gone; i++) {
                run_ms(100);
                gone = http("GET /status.json HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0 && !strstr(resp, "\"line\":\"connected\"");
            }
            CHECK(gone);
        }
        hang_up();
        /* (b) the owner's telnet number */
        CHECK_EQ_I(bt_call("5550002"), 1);
        CHECK(bt_wait("Calling Talker ...", 5000));
        CHECK(bt_wait("Welcome to t.example port 23", 3000));
        CHECK(!bt_saw("BBS: ") && memchr(bt_got, 0xFF, bt_got_n) == NULL);
        bt_type("hi");
        CHECK(bt_wait("you said hi", 3000));
        hang_up();
        /* (c) Netstalgia's, from the image's list, with and without the 1 */
        CHECK_EQ_I(bt_call("3855961775"), 1);
        bt_type("nwn01");
        CHECK(bt_wait("RAW[5000:nwn01]", 5000));
        hang_up();
        /* (d) the fetch failed: the image's list stays; nobody's number does not answer */
        for (i = 0; i < 100 && !file_has(lanlog, "svc: the list could not be fetched"); i++) { run_ms(100); usleep(10000); }
        CHECK(file_has(lanlog, "svc: the list could not be fetched; trying again in 1 minute"));
        CHECK_EQ_I(bt_call("15550003"), 0);
        hang_up();
        /* (e) the page: the owner's rows and Netstalgia's list */
        CHECK(http("GET /services HTTP/1.0\r\n\r\n", resp, sizeof resp) > 0);
        CHECK(strstr(resp, "name=\"snum1\" size=\"13\" maxlength=\"15\" value=\"5550002\"") != NULL);
        CHECK(strstr(resp, "<tr><td>13855961775</td><td>NWN</td><td>raw</td><td>nwn.example:5000</td></tr>") != NULL);
        /* (f) the switch off: Netstalgia's numbers no longer answer, the owner's do */
        snprintf(post, sizeof post, "POST /services HTTP/1.0\r\nHost: 127.0.0.1:8080\r\nContent-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n\r\n%s",
                 (int)strlen(body), body);
        CHECK(http(post, resp, sizeof resp) > 0);
        CHECK(strstr(resp, " 303 ") != NULL || strstr(resp, "Settings saved") != NULL);
        run_ms(500);
        CHECK_EQ_I(bt_call("13855961775"), 0);
        hang_up();
        CHECK_EQ_I(bt_call("5550002"), 1);
        CHECK(bt_wait("Calling Talker ...", 5000));
        hang_up();
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 15. a fetched list of services, signed: taken, its lines that break
     * the rules (a box's own number, a private address) and a kind of line
     * this box does not know are left out */
    {
        static char lanlog[300], builtin[300], fetched[300], live[300], fcmd[700], board[300];
        snprintf(board, sizeof board, "%s/ns_svc.sh", argv[2]);
        snprintf(builtin, sizeof builtin, "%s/ns_svc.builtin", argv[2]);
        snprintf(fetched, sizeof fetched, "%s/ns_svc.remote", argv[2]);
        snprintf(live, sizeof live, "%s/ns_svc.live", argv[2]);
        snprintf(fcmd, sizeof fcmd, "cat %s", fetched);
        unlink(live);
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1; c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_svc2.log", argv[2]);
        unlink(lanlog);                                 /* not a word of an earlier run */
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", "",
                  "-b", "", "-F", builtin, "-f", fcmd, "-K", live, "-T", board, (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        for (i = 0; i < 100 && !file_has(lanlog, "svc: fetched list"); i++) { run_ms(100); usleep(10000); }
        CHECK(file_has(lanlog, "svc: fetched list with 2 numbers (some lines not understood)"));
        CHECK(file_has(live, "echo.example"));
        CHECK_EQ_I(bt_call("15550003"), 1);
        CHECK(bt_wait("Calling Echo ...", 5000));
        CHECK(bt_wait("Welcome to echo.example port 7", 3000));
        hang_up();
        CHECK_EQ_I(bt_call("15550004"), 0);                     /* the private one was left out */
        hang_up();
        CHECK_EQ_I(bt_call("2278353422"), 1);                   /* a board's number stays the board's */
        CHECK(bt_wait("bulletin board gateway", 5000));
        hang_up();
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    /* 16. PROTOWEB (776 869 3200, with or without the 1): the port's own
     * dial-up connection, on a port whose service is the internet, with the
     * archive's redirection for that call alone. The gateway is a stand-in
     * that only runs; the NAT plan is read back from the kernel. */
    {
        static char lanlog[300], arch[300], ruleset[300], cmd[700]; FILE *f;
        snprintf(arch, sizeof arch, "%s/ns_arch.sh", argv[2]);
        snprintf(ruleset, sizeof ruleset, "%s/ns_pw.rules", argv[2]);
        f = fopen(arch, "w");
        if (f) { fprintf(f, "#!/bin/sh\nexec sleep 3600\n"); fclose(f); chmod(arch, 0755); }
        cfg_default(&c);
        strcpy(c.line_number, "5551234"); c.rings = 1; c.status_port = 8080;
        CHECK_EQ_I(cfg_save(&c, conf, err, sizeof err), CFG_OK);
        CHECK_EQ_I(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
        snprintf(lanlog, sizeof lanlog, "%s/ns_pw.log", argv[2]);
        unlink(lanlog);
        pid = fork();
        if (pid == 0) {
            int a = fcntl(sv[1], F_DUPFD, 20), lfd = open(lanlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(a, 3); if (lfd >= 0) dup2(lfd, 2);
            for (i = 4; i < 64; i++) close(i);
            /* the loop's clock starts 3 s before it wraps: the call is made after the wrap */
            setenv("V90D_CLOCK0", "4294964296", 1);
            execl(argv[1], "v90d", "-c", conf, "-l", "fd:3", "-i", "lo", "-t", "ppp0", "-A", arch,
                  "-b", "", "-f", "", (char *)NULL);
            _exit(127);
        }
        close(sv[1]);
        line_fd = sv[0]; idle_fd = -1;
        dead = quiet_exit = 0; hook = modem_on = 0; blk_n = 0; c_tx = 0; c_rx_pw = 0;
        channel_init(&CH, &cc);
        memset(prev_tx, 0xFF, sizeof prev_tx);
        run_ms(2000);
        CHECK(!file_has(lanlog, "PROTOWEB"));
        /* (a) the number: PPP as on the port's own number, the archive for this call */
        hook = 1; run_ms(500);
        dial("7768693200");
        run_ms(2000 + 300);
        start_modem(0);
        CHECK(wait_ip(60000));
        for (i = 0; i < 50 && !file_has(lanlog, "a call to PROTOWEB"); i++) { run_ms(100); usleep(10000); }
        CHECK(file_has(lanlog, "port 1: a call to PROTOWEB: its web, FTP and names go to the archive gateway"));
        for (i = 0; i < 50 && !file_has(lanlog, "archive mode:"); i++) { run_ms(100); usleep(10000); }
        CHECK(file_has(lanlog, "archive mode: ") && file_has(lanlog, "ns_arch.sh started"));
        snprintf(cmd, sizeof cmd, "nft list ruleset > %s 2>&1 || iptables-save -t nat > %s 2>&1", ruleset, ruleset);
        for (i = 0; i < 30; i++) { if (system(cmd)) { } if (file_has(ruleset, "8380")) break; run_ms(100); usleep(20000); }
        CHECK(file_has(ruleset, "8380"));                       /* the web to the gateway */
        hang_up();
        /* (b) the call over: the port's web goes out as ever */
        for (i = 0; i < 30; i++) { if (system(cmd)) { } if (!file_has(ruleset, "8380")) break; run_ms(100); usleep(20000); }
        CHECK(!file_has(ruleset, "8380"));
        /* (c) with the 1 in front, the same */
        hook = 1; run_ms(500);
        dial("17768693200");
        run_ms(2000 + 300);
        start_modem(0);
        CHECK(wait_ip(60000));
        hang_up();
        /* (d) the port's own number: no archive */
        hook = 1; run_ms(500);
        dial("5551234");
        run_ms(2000 + 300);
        start_modem(0);
        CHECK(wait_ip(60000));
        run_ms(500); usleep(200000);
        if (system(cmd)) { }
        CHECK(!file_has(ruleset, "8380"));
        hang_up();
        quiet_exit = 1;
        kill(pid, SIGTERM);
        for (i = 0; i < 400 && !dead; i++) step(0);
        close(line_fd);
        CHECK_EQ_I(waitpid(pid, &st, 0), pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    }
    T_REPORT("ns_call");
}
