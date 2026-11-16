#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
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

int bbsdial_run(const char *host, const char *port, int fd, unsigned connect_s)
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
