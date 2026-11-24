/* The DNS relay for the callers (dnsrelay.h). Ours; written for this project. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "dnsrelay.h"

#define LIS_MAX   4
#define Q_MAX     512           /* a question from a caller: a UDP DNS message as RFC 1035 has it */
#define A_MAX     4096          /* an answer: EDNS sizes */
#define REBIND_MS 5000
#define RESOLV_MS 5000

/* ------------------------------------------------------------ messages */
int dnsrelay_qtype(const uint8_t *m, size_t n)
{
    size_t i = 12;
    if (n < 12 + 5) return -1;
    if ((m[2] & 0x80) || ((m[2] >> 3) & 0x0f) != 0) return -1;    /* a response, or not a standard query */
    if (m[4] != 0 || m[5] != 1) return -1;                          /* one question */
    for (;;) {                                                      /* its name: labels, no pointers */
        if (i >= n) return -1;
        if (m[i] == 0) { i++; break; }
        if (m[i] & 0xc0) return -1;
        i += 1u + m[i];
    }
    if (i + 4 > n) return -1;
    return m[i] << 8 | m[i + 1];
}

size_t dnsrelay_none(const uint8_t *q, size_t n, uint8_t *out, size_t cap)
{
    size_t i = 12;
    if (dnsrelay_qtype(q, n) < 0) return 0;
    while (q[i]) i += 1u + q[i];
    i += 1 + 4;                                 /* the root label, type, class */
    if (i > cap) return 0;
    memcpy(out, q, i);
    out[2] = (uint8_t)(0x80 | (q[2] & 0x01));   /* a response; RD as asked; not authoritative */
    out[3] = 0x80;                              /* RA; NOERROR */
    memset(out + 6, 0, 6);                      /* no answers, authority or additional */
    return i;
}

/* ------------------------------------------------------------- the relay */
static struct {
    int on, nlis;
    uint32_t lis[LIS_MAX], bound[LIS_MAX];
    int fd[LIS_MAX];
    uint32_t rebind_at[LIS_MAX];
    uint32_t up_set[DNSRELAY_UP_MAX];
    struct { int fam; uint8_t a[16]; } up[DNSRELAY_SRV_MAX];   /* IPv4 in a[0..4) */
    int nup, nown;              /* up[0..nown): the settings'; the rest the system's */
    uint32_t peer[DNSRELAY_PEERS_MAX]; int npeer;
    uint32_t resolv_at; int resolv_read;
    int urnd;                   /* /dev/urandom */
    uint8_t pool[128]; size_t pool_n;
    struct {
        int used, lis, srv, tries;
        int fd;                 /* its own socket to the server asked: its own random port */
        uint16_t id, client_id;
        struct sockaddr_in client;
        uint32_t t0;
        uint8_t q[Q_MAX]; size_t qn;
    } p[DNSRELAY_PENDING];
    int inited;
} R;

static void close_fd(int *fd) { if (*fd >= 0) close(*fd); *fd = -1; }

static void init_once(void)
{
    int i;
    if (R.inited) return;
    R.inited = 1;
    for (i = 0; i < LIS_MAX; i++) R.fd[i] = -1;
    for (i = 0; i < DNSRELAY_PENDING; i++) R.p[i].fd = -1;
    R.urnd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
}

/* A question's id, from the kernel's random source: with the port of its
 * own socket, what keeps an answer from being forged by guessing. */
static uint16_t next_id(void)
{
    if (R.pool_n < 2 && R.urnd >= 0) {
        ssize_t r = read(R.urnd, R.pool, sizeof R.pool);
        R.pool_n = r > 0 ? (size_t)r : 0;
    }
    if (R.pool_n < 2) {                         /* no /dev/urandom (never on the box): the clock */
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        return (uint16_t)(t.tv_nsec ^ (t.tv_nsec >> 16) ^ getpid());
    }
    R.pool_n -= 2;
    return (uint16_t)(R.pool[R.pool_n] << 8 | R.pool[R.pool_n + 1]);
}

/* a question is over: answered, given up, or giving way */
static void drop(int k) { close_fd(&R.p[k].fd); R.p[k].used = 0; }

void dnsrelay_set(int on, const uint32_t listen[], int n_listen, const uint32_t peers[], int n_peers,
                  const uint32_t up[DNSRELAY_UP_MAX])
{
    int i, j;
    init_once();
    R.npeer = 0;
    for (i = 0; i < n_peers && R.npeer < DNSRELAY_PEERS_MAX; i++) if (peers[i]) R.peer[R.npeer++] = peers[i];
    R.on = on;
    if (n_listen > LIS_MAX) n_listen = LIS_MAX;
    R.nlis = 0;
    for (i = 0; i < n_listen; i++) {            /* each address once (two ports may share one) */
        if (!listen[i]) continue;
        for (j = 0; j < R.nlis && R.lis[j] != listen[i]; j++) ;
        if (j == R.nlis) R.lis[R.nlis++] = listen[i];
    }
    for (i = 0; i < DNSRELAY_UP_MAX; i++) R.up_set[i] = up[i];
    R.resolv_read = 0;
    if (!on) {
        for (i = 0; i < LIS_MAX; i++) { close_fd(&R.fd[i]); R.bound[i] = 0; }
        for (i = 0; i < DNSRELAY_PENDING; i++) drop(i);
    }
}

/* The servers to ask: the settings' (IPv4), or with none set the system's
 * (DHCP's and the router's, ra6.h). With the settings', the system's IPv6
 * ones are kept as a fallback, asked only when none of the settings' can be
 * sent to (a network with IPv6 alone): a router advertisement, which anyone
 * on the LAN can send, does not stand beside servers the owner chose. */
static void servers(uint32_t now)
{
    int i, own = R.up_set[0] || R.up_set[1];
    FILE *f;
    char l[256];
    if (R.resolv_read && now - R.resolv_at < RESOLV_MS) return;
    R.resolv_read = 1; R.resolv_at = now;
    R.nup = 0;
    for (i = 0; i < DNSRELAY_UP_MAX; i++) if (R.up_set[i]) {
        uint32_t v = R.up_set[i];
        R.up[R.nup].fam = AF_INET;
        R.up[R.nup].a[0] = (uint8_t)(v >> 24); R.up[R.nup].a[1] = (uint8_t)(v >> 16); R.up[R.nup].a[2] = (uint8_t)(v >> 8); R.up[R.nup].a[3] = (uint8_t)v;
        R.nup++;
    }
    R.nown = R.nup;
    f = fopen(getenv("V90D_RESOLV") ? getenv("V90D_RESOLV") : "/etc/resolv.conf", "r");   /* ra6.c writes the same */
    while (f && R.nup < DNSRELAY_SRV_MAX && fgets(l, sizeof l, f)) {
        char a[64];
        if (sscanf(l, " nameserver %63s", a) != 1) continue;
        if (!own && inet_pton(AF_INET, a, R.up[R.nup].a) == 1) R.up[R.nup++].fam = AF_INET;
        else if (inet_pton(AF_INET6, a, R.up[R.nup].a) == 1) R.up[R.nup++].fam = AF_INET6;
    }
    if (f) fclose(f);
}

static int udp(void)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd;
}

static void bind_all(uint32_t now)
{
    int i;
    for (i = 0; i < LIS_MAX; i++) {
        struct sockaddr_in a; int one = 1;
        if (i >= R.nlis) { close_fd(&R.fd[i]); R.bound[i] = 0; continue; }
        if (R.fd[i] >= 0 && R.bound[i] == R.lis[i]) continue;
        if (R.fd[i] < 0 && R.rebind_at[i] && now - R.rebind_at[i] < REBIND_MS) continue;
        close_fd(&R.fd[i]);
        R.fd[i] = udp(); R.rebind_at[i] = now ? now : 1;
        if (R.fd[i] < 0) continue;
        (void)setsockopt(R.fd[i], SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        /* the PPP address may be on no interface yet (no call so far) */
        (void)setsockopt(R.fd[i], IPPROTO_IP, IP_FREEBIND, &one, sizeof one);
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(53); a.sin_addr.s_addr = htonl(R.lis[i]);
        if (bind(R.fd[i], (struct sockaddr *)(void *)&a, sizeof a) < 0) { close_fd(&R.fd[i]); continue; }
        R.bound[i] = R.lis[i];
    }
}

/* ask the question's current server, on a new socket connected to it: a
 * random port of the kernel's, and nothing from any other address is
 * taken on it */
static int ask_in(int k, int from, int n);
static void ask(int k)
{
    close_fd(&R.p[k].fd);
    /* the settings' servers if there are any, else the system's; the
     * system's after the settings' only when none of those can be sent to */
    if (R.nown) { if (ask_in(k, 0, R.nown) || R.nup == R.nown) return; ask_in(k, R.nown, R.nup - R.nown); }
    else ask_in(k, 0, R.nup);
}

/* from the question's current server on, among up[from..from+n): the first
 * that can be sent to (no route to one: the next); 1 = sent */
static int ask_in(int k, int from, int n)
{
    int tried;
    for (tried = 0; tried < n; tried++, R.p[k].srv++) {
        int i = from + R.p[k].srv % n;
        struct sockaddr_in a4; struct sockaddr_in6 a6;
        const struct sockaddr *sa; socklen_t sl;
        if (R.up[i].fam == AF_INET) {
            memset(&a4, 0, sizeof a4); a4.sin_family = AF_INET; a4.sin_port = htons(53);
            memcpy(&a4.sin_addr, R.up[i].a, 4);
            sa = (const struct sockaddr *)(const void *)&a4; sl = sizeof a4;
        } else {
            memset(&a6, 0, sizeof a6); a6.sin6_family = AF_INET6; a6.sin6_port = htons(53);
            memcpy(&a6.sin6_addr, R.up[i].a, 16);
            sa = (const struct sockaddr *)(const void *)&a6; sl = sizeof a6;
        }
        R.p[k].fd = socket(R.up[i].fam, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (R.p[k].fd < 0) continue;
        if (connect(R.p[k].fd, sa, sl) == 0 && send(R.p[k].fd, R.p[k].q, R.p[k].qn, 0) >= 0) return 1;
        close_fd(&R.p[k].fd);
    }
    return 0;
}

/* the answer's question is the one asked (the id alone is 16 bits) */
static int same_question(const uint8_t *q, size_t qn, const uint8_t *m, size_t n)
{
    size_t i = 12;
    while (i < qn && q[i]) i += 1u + q[i];
    i += 1 + 4;
    if (i > qn || i > n || m[4] != 0 || m[5] != 1) return 0;
    for (size_t j = 12; j < i; j++) {
        uint8_t x = q[j], y = m[j];
        if (x >= 'A' && x <= 'Z') x = (uint8_t)(x + 32);     /* names are the same in any case */
        if (y >= 'A' && y <= 'Z') y = (uint8_t)(y + 32);
        if (x != y) return 0;
    }
    return 1;
}

static void from_caller(int li, uint32_t now)
{
    int n;
    for (n = 0; n < 16; n++) {
        uint8_t m[Q_MAX], out[Q_MAX];
        struct sockaddr_in c; socklen_t cl = sizeof c;
        ssize_t r = recvfrom(R.fd[li], m, sizeof m, 0, (struct sockaddr *)(void *)&c, &cl);
        int t, k, oldest = 0;
        if (r <= 0) return;
        for (k = 0; k < R.npeer && R.peer[k] != ntohl(c.sin_addr.s_addr); k++) ;
        if (k == R.npeer) continue;             /* not a caller: the LAN gets no name server here */
        t = dnsrelay_qtype(m, (size_t)r);
        if (t < 0) continue;
        if (t == DNS_T_AAAA) {                  /* the caller has no IPv6: none */
            size_t o = dnsrelay_none(m, (size_t)r, out, sizeof out);
            if (o) (void)sendto(R.fd[li], out, o, 0, (struct sockaddr *)(void *)&c, cl);
            continue;
        }
        for (k = 0; k < DNSRELAY_PENDING && R.p[k].used; k++)
            if (now - R.p[k].t0 > now - R.p[oldest].t0) oldest = k;
        if (k == DNSRELAY_PENDING) { k = oldest; drop(k); }    /* full: the oldest question gives way */
        R.p[k].used = 1; R.p[k].lis = li; R.p[k].srv = 0; R.p[k].tries = 1;
        R.p[k].client = c; R.p[k].client_id = (uint16_t)(m[0] << 8 | m[1]);
        R.p[k].id = next_id(); R.p[k].t0 = now;
        m[0] = (uint8_t)(R.p[k].id >> 8); m[1] = (uint8_t)R.p[k].id;
        memcpy(R.p[k].q, m, (size_t)r); R.p[k].qn = (size_t)r;
        ask(k);
    }
}

static void from_servers(void)
{
    int k, n;
    for (k = 0; k < DNSRELAY_PENDING; k++) {
        for (n = 0; n < 4 && R.p[k].used && R.p[k].fd >= 0; n++) {
            uint8_t m[A_MAX];
            ssize_t r = recv(R.p[k].fd, m, sizeof m, 0);     /* connected: only from the server asked */
            if (r <= 0) break;
            if (r < 12 || !(m[2] & 0x80) || (uint16_t)(m[0] << 8 | m[1]) != R.p[k].id
                || !same_question(R.p[k].q, R.p[k].qn, m, (size_t)r)) continue;
            m[0] = (uint8_t)(R.p[k].client_id >> 8); m[1] = (uint8_t)R.p[k].client_id;
            if (R.fd[R.p[k].lis] >= 0)
                (void)sendto(R.fd[R.p[k].lis], m, (size_t)r, 0, (struct sockaddr *)(void *)&R.p[k].client, sizeof R.p[k].client);
            drop(k);
        }
    }
}

void dnsrelay_poll(uint32_t now)
{
    int i, k;
    init_once();
    if (!R.on) return;
    servers(now);
    bind_all(now);
    for (i = 0; i < R.nlis; i++) if (R.fd[i] >= 0) from_caller(i, now);
    from_servers();
    for (k = 0; k < DNSRELAY_PENDING; k++) {    /* no answer: the next server, or give up */
        if (!R.p[k].used || now - R.p[k].t0 < DNSRELAY_WAIT_MS) continue;
        if (R.p[k].tries >= DNSRELAY_TRIES) { drop(k); continue; }
        R.p[k].tries++; R.p[k].srv++; R.p[k].t0 = now;
        ask(k);
    }
}
