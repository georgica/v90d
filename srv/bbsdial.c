#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
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

int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s, int self_ok)
{
    struct addrinfo hint, *res = NULL, *ai; int s = -1; char buf[2048];
    struct pollfd p[2];

    memset(&hint, 0, sizeof hint);
    hint.ai_family = AF_INET; hint.ai_socktype = SOCK_STREAM;
    /* a name server that never answers: the alarm ends this process, which
     * is as good as "not reached" to the daemon */
    (void)alarm(connect_s);
    if (getaddrinfo(host, port, &hint, &res) != 0) res = NULL;
    (void)alarm(0);
    for (ai = res; ai && s < 0; ai = ai->ai_next) {
        uint32_t addr = ntohl(((const struct sockaddr_in *)(const void *)ai->ai_addr)->sin_addr.s_addr);
        if (self_ok != 1 && bbsdial_is_self(addr)) continue;   /* the box itself, by whatever name */
        if (self_ok == BBSDIAL_PUBLIC && !bbsdial_is_public(addr)) continue;  /* a list entry: the internet only */
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s >= 0 && connect_within(s, ai->ai_addr, ai->ai_addrlen, (int)connect_s * 1000) < 0) { close(s); s = -1; }
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
