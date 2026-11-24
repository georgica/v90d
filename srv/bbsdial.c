#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include "bbsdial.h"

/* all of b[0..n) to fd, however long the other end takes */
static int put_all(int fd, const char *b, size_t n)
{
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd p = { fd, POLLOUT, 0 };
            (void)poll(&p, 1, -1);
            continue;
        }
        if (w <= 0) return -1;
        b += w; n -= (size_t)w;
    }
    return 0;
}

/* a connect that gives up after ms */
static int connect_within(int s, const struct sockaddr *a, socklen_t al, int ms)
{
    struct pollfd p = { s, POLLOUT, 0 }; int err = 0; socklen_t el = sizeof err;
    int fl = fcntl(s, F_GETFL, 0);
    if (fl < 0 || fcntl(s, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
    if (connect(s, a, al) < 0) {
        if (errno != EINPROGRESS) return -1;
        if (poll(&p, 1, ms) <= 0) return -1;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) return -1;
    }
    return fcntl(s, F_SETFL, fl);
}

int bbsdial_is_self(uint32_t addr)
{
    uint32_t net = addr >> 24;
    struct sockaddr_in a; socklen_t al = sizeof a; int u, own = 0;

    if (net == 127 || net == 0) return 1;           /* the box itself */
    if (addr >> 16 == 0xa9fe) return 1;             /* link-local */
    if (net >= 224) return 1;                       /* multicast, reserved, broadcast */
    /* One of our interfaces' addresses? The address the kernel would send
     * from towards it is then that very address. A datagram socket's
     * connect() only looks the route up; nothing goes out. */
    u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u < 0) return 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(9); a.sin_addr.s_addr = htonl(addr);
    if (connect(u, (const struct sockaddr *)&a, sizeof a) == 0 && getsockname(u, (struct sockaddr *)&a, &al) == 0)
        own = ntohl(a.sin_addr.s_addr) == addr;
    close(u);
    return own;
}

int bbsdial_is_public(uint32_t a)
{
    unsigned t = a >> 24;
    if (t == 0 || t == 10 || t == 127 || t >= 224) return 0;
    if ((a & 0xffc00000u) == 0x64400000u) return 0;     /* 100.64/10 */
    if ((a & 0xffff0000u) == 0xa9fe0000u) return 0;     /* 169.254/16 */
    if ((a & 0xfff00000u) == 0xac100000u) return 0;     /* 172.16/12 */
    if ((a & 0xffffff00u) == 0xc0000000u) return 0;     /* 192.0.0/24 */
    if ((a & 0xffffff00u) == 0xc0000200u) return 0;     /* 192.0.2/24 */
    if ((a & 0xffff0000u) == 0xc0a80000u) return 0;     /* 192.168/16 */
    if ((a & 0xfffe0000u) == 0xc6120000u) return 0;     /* 198.18/15 */
    if ((a & 0xffffff00u) == 0xc6336400u) return 0;     /* 198.51.100/24 */
    if ((a & 0xffffff00u) == 0xcb007100u) return 0;     /* 203.0.113/24 */
    return 1;
}

static uint32_t v4_at(const uint8_t *a, int at) { return (uint32_t)a[at] << 24 | (uint32_t)a[at + 1] << 16 | (uint32_t)a[at + 2] << 8 | a[at + 3]; }

static uint8_t pref64[16];
static int pref64_len;
void bbsdial_set_pref64(const uint8_t p[16], int len)
{
    pref64_len = (len == 32 || len == 40 || len == 48 || len == 56 || len == 64 || len == 96) ? len : 0;
    memset(pref64, 0, sizeof pref64);
    if (pref64_len) memcpy(pref64, p, 16);
}

/* the IPv4 address in a, if a is in the NAT64 prefix p/len (RFC 6052 2.2:
 * the octets past the prefix, octet 8 skipped) */
static int in_nat64(const uint8_t a[16], const uint8_t *p, int len, uint32_t *v4)
{
    static const int at[6][4] = { { 4, 5, 6, 7 }, { 5, 6, 7, 9 }, { 6, 7, 9, 10 }, { 7, 9, 10, 11 }, { 9, 10, 11, 12 }, { 12, 13, 14, 15 } };
    int k = len == 32 ? 0 : len == 40 ? 1 : len == 48 ? 2 : len == 56 ? 3 : len == 64 ? 4 : len == 96 ? 5 : -1;
    if (k < 0 || memcmp(a, p, (size_t)len / 8) != 0) return 0;
    *v4 = (uint32_t)a[at[k][0]] << 24 | (uint32_t)a[at[k][1]] << 16 | (uint32_t)a[at[k][2]] << 8 | a[at[k][3]];
    return 1;
}

/* an address that carries an IPv4 one: NAT64's (well-known or the
 * network's) and 6to4; 1 with it in *v4 */
static int carries_v4(const uint8_t a[16], uint32_t *v4)
{
    static const uint8_t wk[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (in_nat64(a, wk, 96, v4)) return 1;
    if (pref64_len && in_nat64(a, pref64, pref64_len, v4)) return 1;
    if (a[0] == 0x20 && a[1] == 0x02) { *v4 = v4_at(a, 2); return 1; }
    return 0;
}

int bbsdial_is_self6(const uint8_t a[16])
{
    static const uint8_t zero[12] = { 0 }, mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    struct sockaddr_in6 s; socklen_t sl = sizeof s; int u, own = 0;
    if (memcmp(a, zero, 12) == 0) return 1;                     /* ::, ::1 and the old IPv4-compatible */
    if (memcmp(a, mapped, 12) == 0) return bbsdial_is_self(v4_at(a, 12));
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return 1;       /* link-local */
    if (a[0] == 0xff) return 1;                                 /* multicast */
    {   /* NAT64 or 6to4 to the box itself */
        uint32_t v4;
        if (carries_v4(a, &v4) && bbsdial_is_self(v4)) return 1;
    }
    u = socket(AF_INET6, SOCK_DGRAM, 0);                        /* one of ours? (as for IPv4) */
    if (u < 0) return 0;
    memset(&s, 0, sizeof s);
    s.sin6_family = AF_INET6; s.sin6_port = htons(9); memcpy(&s.sin6_addr, a, 16);
    if (connect(u, (const struct sockaddr *)&s, sizeof s) == 0 && getsockname(u, (struct sockaddr *)&s, &sl) == 0)
        own = memcmp(&s.sin6_addr, a, 16) == 0;
    close(u);
    return own;
}

int bbsdial_is_public6(const uint8_t a[16])
{
    static const uint8_t wk[12] = { 0, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint32_t v4;
    /* the well-known NAT64 prefix and 6to4 (fixed, not anyone's say): by the
     * IPv4 address they carry */
    if (in_nat64(a, wk, 96, &v4)) return bbsdial_is_public(v4);
    if (a[0] == 0x20 && a[1] == 0x02) return bbsdial_is_public(v4_at(a, 2));
    if ((a[0] & 0xe0) != 0x20) return 0;                        /* global unicast is 2000::/3 */
    if (a[0] == 0x20 && a[1] == 0x01) {
        if (a[2] == 0x0d && a[3] == 0xb8) return 0;             /* documentation */
        if (a[2] == 0 && a[3] == 0) return 0;                   /* Teredo */
        if (a[2] == 0 && a[3] == 0x02 && a[4] == 0 && a[5] == 0) return 0;     /* benchmarking */
        if (a[2] == 0 && (a[3] & 0xf0) == 0x10) return 0;      /* ORCHID */
        if (a[2] == 0 && (a[3] & 0xf0) == 0x20) return 0;      /* ORCHIDv2 */
    }
    if (a[0] == 0x3f && a[1] == 0xff && (a[2] & 0xf0) == 0) return 0;  /* documentation 3fff::/20 */
    /* the network's own NAT64 prefix comes in an advertisement anyone on the
     * LAN can send: it may only make an address less public. One that has
     * passed the above and is in it must also carry a public IPv4 address. */
    if (pref64_len && in_nat64(a, pref64, pref64_len, &v4)) return bbsdial_is_public(v4);
    return 1;
}

/* may this address of the name be connected to? */
static int allowed(const struct addrinfo *ai, int self_ok)
{
    if (ai->ai_family == AF_INET) {
        uint32_t addr = ntohl(((const struct sockaddr_in *)(const void *)ai->ai_addr)->sin_addr.s_addr);
        if (self_ok != 1 && bbsdial_is_self(addr)) return 0;   /* the box itself, by whatever name */
        if (self_ok == BBSDIAL_PUBLIC && !bbsdial_is_public(addr)) return 0;  /* a list entry: the internet only */
        return 1;
    }
    if (ai->ai_family == AF_INET6) {
        const uint8_t *a = ((const struct sockaddr_in6 *)(const void *)ai->ai_addr)->sin6_addr.s6_addr;
        if (self_ok != 1 && bbsdial_is_self6(a)) return 0;
        if (self_ok == BBSDIAL_PUBLIC && !bbsdial_is_public6(a)) return 0;
        return 1;
    }
    return 0;
}

static long ms_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s, int self_ok)
{
    struct addrinfo hint, *res = NULL, *ai; int s = -1; char buf[2048];
    struct pollfd p[2];

    long end;
    int left = 0;

    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_UNSPEC; hint.ai_socktype = SOCK_STREAM;   /* IPv4 and IPv6 */
    /* a name server that never answers: the alarm ends this process, which
     * is as good as "not reached" to the daemon */
    end = ms_now() + (long)connect_s * 1000;
    (void)alarm(connect_s);
    if (getaddrinfo(host, port, &hint, &res) != 0) res = NULL;
    (void)alarm(0);
    for (ai = res; ai; ai = ai->ai_next) if (allowed(ai, self_ok)) left++;
    for (ai = res; ai && s < 0; ai = ai->ai_next) {
        long rest = end - ms_now(), share;
        if (!allowed(ai, self_ok)) continue;
        if (rest <= 0) break;
        /* each address its share of what is left, at least 3 s: one that is
         * routed but dead (IPv6 that goes nowhere) leaves the others time */
        share = left > 1 ? rest / left : rest;
        if (share < 3000) share = rest < 3000 ? rest : 3000;
        left--;
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s >= 0 && connect_within(s, ai->ai_addr, ai->ai_addrlen, (int)share) < 0) { close(s); s = -1; }
    }
    if (res) freeaddrinfo(res);
    if (s < 0) return 1;

    /* octets both ways until one side ends; nothing ends it for being quiet */
    p[0].fd = s; p[0].events = POLLIN; p[1].fd = fd; p[1].events = POLLIN;
    for (;;) {
        ssize_t r;
        p[0].revents = p[1].revents = 0;
        if (poll(p, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents) {
            r = recv(s, buf, sizeof buf, 0);
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0 || put_all(fd, buf, (size_t)r) < 0) break;
        }
        if (p[1].revents) {
            r = recv(fd, buf, sizeof buf, 0);
            if (r < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (r <= 0 || put_all(s, buf, (size_t)r) < 0) break;
        }
    }
    close(s);
    return 0;
}
