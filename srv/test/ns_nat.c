/* The console rules of nat.h, with real packets through the kernel. Run in a
 * network namespace of its own (`unshare -Urn`, see the Makefile): two TUN
 * devices stand for the telephone side ("ppp7", caller 10.64.64.2) and the
 * network side ("wan7", default route), and what is written into one is read
 * back from the other after the kernel has routed and translated it. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <net/route.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include "test.h"
#include "nat.h"
#include "tun.h"

#define IP4(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define CALLER IP4(10, 64, 64, 2)
#define WAN_US IP4(192, 0, 2, 1)
#define WAN_GW IP4(192, 0, 2, 2)

static int ppp_fd, wan_fd;

static void put_file(const char *path, const char *v)
{
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { if (write(fd, v, strlen(v)) < 0) perror(path); close(fd); }
}

static unsigned sum16(const uint8_t *p, size_t n, unsigned long s)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) s += ((unsigned)p[i] << 8) | p[i + 1];
    if (i < n) s += (unsigned)p[i] << 8;
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (unsigned)s;
}

/* a UDP datagram (8 octets of data) or a TCP SYN; returns the length */
static size_t mkpkt(uint8_t *p, int proto, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport)
{
    size_t tl = proto == 6 ? 20 : 16, n = 20 + tl; unsigned c;
    memset(p, 0, n);
    p[0] = 0x45; p[2] = (uint8_t)(n >> 8); p[3] = (uint8_t)n; p[8] = 64; p[9] = (uint8_t)proto;
    p[12] = (uint8_t)(src >> 24); p[13] = (uint8_t)(src >> 16); p[14] = (uint8_t)(src >> 8); p[15] = (uint8_t)src;
    p[16] = (uint8_t)(dst >> 24); p[17] = (uint8_t)(dst >> 16); p[18] = (uint8_t)(dst >> 8); p[19] = (uint8_t)dst;
    c = 0xffff - sum16(p, 20, 0); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c;
    p[20] = (uint8_t)(sport >> 8); p[21] = (uint8_t)sport; p[22] = (uint8_t)(dport >> 8); p[23] = (uint8_t)dport;
    if (proto == 6) {
        p[27] = 1; p[32] = 0x50; p[33] = 0x02; p[34] = 0x20;
        c = 0xffff - sum16(p + 20, tl, sum16(p + 12, 8, 6 + tl));
        p[36] = (uint8_t)(c >> 8); p[37] = (uint8_t)c;
    } else {
        p[25] = (uint8_t)tl;                    /* no UDP checksum */
    }
    return n;
}

static uint32_t get32(const uint8_t *p) { return IP4(p[0], p[1], p[2], p[3]); }

/* write a packet into `in`, wait for the next IPv4 packet of that protocol on
 * `out`; its source, destination and destination port, or 0 when none came */
static int through(int in, int out, const uint8_t *pkt, size_t len, uint32_t *src, uint32_t *dst, unsigned *dport)
{
    uint8_t b[2000]; struct pollfd pf; ssize_t n; int tries;
    for (;;) { pf.fd = out; pf.events = POLLIN; if (poll(&pf, 1, 0) <= 0) break; if (read(out, b, sizeof b) <= 0) break; }
    if (write(in, pkt, len) != (ssize_t)len) return 0;
    for (tries = 0; tries < 20; tries++) {
        pf.fd = out; pf.events = POLLIN;
        if (poll(&pf, 1, 300) <= 0) return 0;
        n = read(out, b, sizeof b);
        if (n < 28 || (b[0] >> 4) != 4 || b[9] != pkt[9]) continue;
        *src = get32(b + 12); *dst = get32(b + 16); *dport = ((unsigned)b[22] << 8) | b[23];
        return 1;
    }
    return 0;
}

int main(void)
{
    static const nat_rewrite rw[] = {
        { IP4(203, 0, 113, 9),  IP4(198, 51, 100, 7), 9500, 6 },
        { IP4(203, 0, 113, 10), IP4(198, 51, 100, 7), 9500, 17 },
        { IP4(203, 0, 113, 11), IP4(198, 51, 100, 8), 0,    6 },
    };
    static const nat_open op[] = { { 17, 7980, 7980 }, { 6, 2300, 2400 } };
    nat_plan p; uint8_t pkt[64]; size_t n; uint32_t s, d; unsigned dp; int sk; struct rtentry rt; struct sockaddr_in *sa;

    ppp_fd = tun_open("ppp7", IP4(10, 64, 64, 1), CALLER, 1500);
    wan_fd = tun_open("wan7", WAN_US, WAN_GW, 1500);
    if (ppp_fd < 0 || wan_fd < 0) { printf("ns_nat: no TUN device here (%s): not tried\n", strerror(errno)); return 0; }
    put_file("/proc/sys/net/ipv4/conf/all/rp_filter", "0");
    put_file("/proc/sys/net/ipv4/conf/ppp7/rp_filter", "0");
    put_file("/proc/sys/net/ipv4/conf/wan7/rp_filter", "0");
    sk = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&rt, 0, sizeof rt);
    sa = (struct sockaddr_in *)(void *)&rt.rt_dst; sa->sin_family = AF_INET;
    sa = (struct sockaddr_in *)(void *)&rt.rt_genmask; sa->sin_family = AF_INET;
    sa = (struct sockaddr_in *)(void *)&rt.rt_gateway; sa->sin_family = AF_INET; sa->sin_addr.s_addr = htonl(WAN_GW);
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    CHECK_EQ_I(ioctl(sk, SIOCADDRT, &rt), 0);
    close(sk);
    CHECK_EQ_I(nat_ip_forward(1), 0);

    memset(&p, 0, sizeof p);
    p.peer[0] = CALLER; p.masquerade = 1; p.console[0] = 1;
    p.rewrite = rw; p.n_rewrite = 3; p.open = op; p.n_open = 2;
    strcpy(p.wan, "wan7");
    CHECK_EQ_I(nat_apply(&p), 0);

    /* a plain packet of the caller leaves masqueraded, destination as it was */
    n = mkpkt(pkt, 17, CALLER, 4000, IP4(203, 0, 113, 50), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp));
    CHECK(s == WAN_US); CHECK(d == IP4(203, 0, 113, 50)); CHECK_EQ_I((int)dp, 9500);

    /* rewrite with a port and a protocol: only that protocol, only that port */
    n = mkpkt(pkt, 6, CALLER, 4001, IP4(203, 0, 113, 9), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp));
    CHECK(d == IP4(198, 51, 100, 7)); CHECK_EQ_I((int)dp, 9500); CHECK(s == WAN_US);
    n = mkpkt(pkt, 6, CALLER, 4002, IP4(203, 0, 113, 9), 80);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp)); CHECK(d == IP4(203, 0, 113, 9));
    n = mkpkt(pkt, 17, CALLER, 4003, IP4(203, 0, 113, 9), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp)); CHECK(d == IP4(203, 0, 113, 9));
    n = mkpkt(pkt, 17, CALLER, 4004, IP4(203, 0, 113, 10), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp)); CHECK(d == IP4(198, 51, 100, 7));
    n = mkpkt(pkt, 6, CALLER, 4005, IP4(203, 0, 113, 10), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp)); CHECK(d == IP4(203, 0, 113, 10));
    /* without a port: every port of that protocol, the port kept */
    n = mkpkt(pkt, 6, CALLER, 4006, IP4(203, 0, 113, 11), 12345);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp));
    CHECK(d == IP4(198, 51, 100, 8)); CHECK_EQ_I((int)dp, 12345);

    /* ports opened towards the console: from the network side to our own
     * address, they come out on the telephone side for the caller */
    n = mkpkt(pkt, 17, IP4(198, 51, 100, 99), 5555, WAN_US, 7980);
    CHECK(through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp));
    CHECK(d == CALLER); CHECK_EQ_I((int)dp, 7980); CHECK(s == IP4(198, 51, 100, 99));
    n = mkpkt(pkt, 6, IP4(198, 51, 100, 99), 5556, WAN_US, 2350);
    CHECK(through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp)); CHECK(d == CALLER); CHECK_EQ_I((int)dp, 2350);
    n = mkpkt(pkt, 6, IP4(198, 51, 100, 99), 5557, WAN_US, 2401);
    CHECK(!through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp));
    n = mkpkt(pkt, 17, IP4(198, 51, 100, 99), 5558, WAN_US, 7981);
    CHECK(!through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp));

    /* a port that is not a console's gets none of this */
    p.console[0] = 0;
    CHECK_EQ_I(nat_apply(&p), 0);
    n = mkpkt(pkt, 6, CALLER, 4010, IP4(203, 0, 113, 11), 12346);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp)); CHECK(d == IP4(203, 0, 113, 11));
    n = mkpkt(pkt, 17, IP4(198, 51, 100, 99), 5560, WAN_US, 7980);
    CHECK(!through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp));

    /* a console that is a host on the network itself (lan): its packets
     * leave with its own address, the address rewrites still hold, and no
     * port is opened towards it here (the network reaches it directly) */
    p.console[0] = 1; p.lan[0] = 1;
    CHECK_EQ_I(nat_apply(&p), 0);
    n = mkpkt(pkt, 17, CALLER, 4020, IP4(203, 0, 113, 50), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp));
    CHECK(s == CALLER); CHECK(d == IP4(203, 0, 113, 50));
    n = mkpkt(pkt, 17, CALLER, 4021, IP4(203, 0, 113, 10), 9500);
    CHECK(through(ppp_fd, wan_fd, pkt, n, &s, &d, &dp));
    CHECK(s == CALLER); CHECK(d == IP4(198, 51, 100, 7));
    n = mkpkt(pkt, 17, IP4(198, 51, 100, 99), 5570, WAN_US, 7980);
    CHECK(!through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp));
    p.lan[0] = 0;

    /* the full lists fit in one transaction */
    {
        static nat_rewrite many[NAT_REWRITE_MAX]; static nat_open wide[NAT_OPEN_MAX]; int i;
        for (i = 0; i < NAT_REWRITE_MAX; i++) { many[i].from = IP4(203, 0, 113, 1) + (uint32_t)i; many[i].to = IP4(198, 51, 100, 7); many[i].port = 9500; many[i].proto = 6; }
        for (i = 0; i < NAT_OPEN_MAX; i++) { wide[i].proto = 17; wide[i].lo = wide[i].hi = (uint16_t)(20000 + i); }
        p.console[0] = 1; p.peer[1] = IP4(10, 64, 64, 3); p.console[1] = 1;
        p.rewrite = many; p.n_rewrite = NAT_REWRITE_MAX; p.open = wide; p.n_open = NAT_OPEN_MAX;
        CHECK_EQ_I(nat_apply(&p), 0);
        n = mkpkt(pkt, 17, IP4(198, 51, 100, 99), 5561, WAN_US, (uint16_t)(20000 + NAT_OPEN_MAX - 1));
        CHECK(through(wan_fd, ppp_fd, pkt, n, &s, &d, &dp)); CHECK(d == CALLER);
    }
    CHECK_EQ_I(nat_remove(), 0);
    T_REPORT("ns_nat");
}
