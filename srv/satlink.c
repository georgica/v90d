/* See satlink.h. Extracted from srv/main.c unchanged (0.2.3), behaviour the
 * same; main.c owns S[], n_ports, lan_if, logf_ and ip_str (daemon.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include "daemon.h"
#include "satlink.h"
#include "saturn.h"
#include "stun.h"

/* ---- service = saturn: the network side of a console-to-console call
 * (saturn.h). Nothing here waits: every socket is non-blocking and looked
 * at once per block of the line. One conversation per port.
 *
 * The tunnel's words are another project's and carry no proof of who is
 * speaking, so what can be done without changing them is done: the sockets
 * exist only while a console's call is up, they are tied to the network
 * interface (a caller on the other port cannot reach them), only the
 * address that greeted us is heard, from the port its first datagram came
 * from, and a record number far ahead is not believed (saturn.h). Somebody
 * who can forge that address and port on the way can still put octets into
 * the game. ---- */
enum { SL_IDLE = 0, SL_LISTEN, SL_HELLO_IN, SL_CONNECT, SL_HELLO_OUT, SL_MM_STUN, SL_MM_QUERY, SL_LINKED, SL_OVER };
#define SL_WAIT_MS   120000u    /* waiting for the other player's call */
#define SL_REACH_MS   60000u    /* trying to reach the other player's box */
#define SL_HELLO_MS    5000u    /* for the greeting, once connected */
#define SL_SILENT_MS  30000u    /* the other box sends a ping every second */
static struct sat_link {
    int      st, lis, tcp, udp;
    uint32_t peer; uint16_t peer_port; int port_known;
    uint32_t t0, t_hello, next_try, ping_at, heard;
    char     hello[8]; size_t hello_n;
    saturn_tx tx; saturn_rx rx;
    uint8_t  up[SATURN_REC_MAX]; size_t up_n;      /* from the modem, for the next datagram */
    uint8_t  dn[4096]; size_t dn_n;                /* for the modem, as it takes them */
    /* matchmaking (saturn.h): STUN for the public endpoint, then the match server */
    char     game[3];           /* the two-character game id */
    int      mm_wait;           /* 1 = we wait to be paired, 0 = we call */
    uint32_t pub; uint16_t pub_port;               /* our public address and port by STUN */
    uint8_t  txid[STUN_TXID_LEN];
    uint32_t stun_at; int stun_tries;
    uint32_t q_at; pid_t q_pid; int q_fd; int q_asked;
    char     q_text[600]; size_t q_len;
} sl[2] = { { .lis = -1, .tcp = -1, .udp = -1, .q_fd = -1 }, { .lis = -1, .tcp = -1, .udp = -1, .q_fd = -1 } };

/* the match server and STUN server; the test overrides them (-M, -S) so it
 * needs no internet and no DNS */
static const char *mm_cmd = "";                    /* -M: run instead of wget; args: action game client_ip port */
static int mm_any;                                 /* -X: accept any opponent address (testing only) */
static uint32_t    stun_ip; static uint16_t stun_port = SATURN_STUN_PORT;   /* -S A.B.C.D[:port] */

static void sl_close(struct sat_link *l)
{
    if (l->lis >= 0) close(l->lis);
    if (l->tcp >= 0) close(l->tcp);
    if (l->udp >= 0) close(l->udp);
    if (l->q_pid > 0) { (void)kill(l->q_pid, SIGKILL); (void)waitpid(l->q_pid, NULL, 0); }
    if (l->q_fd >= 0) close(l->q_fd);
    l->lis = l->tcp = l->udp = l->q_fd = -1; l->q_pid = 0;
}
static void sl_over(int k, const char *why)
{
    logf_(CFG_LOG_INFO, "port %d: %s", k + 1, why);
    sl_close(&sl[k]);
    sl[k].st = SL_OVER;
    srv_hangup(&S[k]);
}
static void sl_send(struct sat_link *l, const void *b, size_t n)
{
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(l->peer_port); sa.sin_addr.s_addr = htonl(l->peer);
    (void)sendto(l->udp, b, n, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)(void *)&sa, sizeof sa);
}
static void sl_flush(struct sat_link *l)
{
    static uint8_t dg[SATURN_DGRAM_MAX]; size_t n;
    if (!l->up_n) return;
    n = saturn_pack(&l->tx, l->up, l->up_n, dg);
    l->up_n = 0;
    sl_send(l, dg, n); sl_send(l, dg, n);           /* every datagram twice: nothing is asked for again */
}
/* octets from the console's modem */
void satlink_from_modem(void *ctx, const uint8_t *oct, size_t n)
{
    struct sat_link *l = &sl[(int)(((srv_t *)ctx) - S)];
    if (l->st != SL_LINKED) return;                 /* nobody to tell yet */
    while (n) {
        size_t m = sizeof l->up - l->up_n;
        if (m > n) m = n;
        memcpy(l->up + l->up_n, oct, m); l->up_n += m; oct += m; n -= m;
        if (l->up_n == sizeof l->up) sl_flush(l);
    }
}
static int sl_tcp(void)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
    if (fd >= 0) (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (fd >= 0) (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, lan_if, (socklen_t)strlen(lan_if) + 1);
    return fd;
}
/* a UDP socket of our own, bound to local_port, tied to the network side */
static int sl_udp_bind(struct sat_link *l, int local_port)
{
    struct sockaddr_in sa; int one = 1, tos = SATURN_TOS;
    l->udp = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (l->udp < 0) return -1;
    (void)setsockopt(l->udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    (void)setsockopt(l->udp, SOL_SOCKET, SO_BINDTODEVICE, lan_if, (socklen_t)strlen(lan_if) + 1);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons((uint16_t)local_port);
    if (bind(l->udp, (struct sockaddr *)(void *)&sa, sizeof sa) < 0) { close(l->udp); l->udp = -1; return -1; }
    (void)setsockopt(l->udp, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    return 0;
}
/* the two boxes know each other's address now: begin the UDP conversation.
 * l->udp must be bound and l->peer / l->peer_port set. ring: the waiting
 * side's game is shown RING + CONNECT. */
static void sl_begin(int k, uint32_t now_ms, int ring, int fixed_port)
{
    struct sat_link *l = &sl[k]; char a[16];
    if (l->tcp >= 0) { close(l->tcp); l->tcp = -1; }
    if (l->lis >= 0) { close(l->lis); l->lis = -1; }
    /* direct: the port the opponent really sends from is learned from its
     * first datagram (it may be rewritten by a router). Matchmaking: the
     * match server already gave the opponent's public port; keep it, so a
     * stray reply (for instance from the STUN server) cannot replace it. */
    l->port_known = fixed_port;
    saturn_tx_init(&l->tx); saturn_rx_init(&l->rx);
    l->up_n = 0; l->dn_n = 0;
    if (ring) { memcpy(l->dn, SATURN_RING, sizeof SATURN_RING - 1); l->dn_n = sizeof SATURN_RING - 1; }
    l->heard = l->ping_at = now_ms;
    l->st = SL_LINKED;
    sl_send(l, SATURN_OPEN, sizeof SATURN_OPEN - 1);
    logf_(CFG_LOG_INFO, "port %d: the other player is %s", k + 1, ip_str(l->peer, a));
}
/* direct play: bind our port, then begin */
static void sl_linked(int k, uint32_t now_ms, int local_port, int peer_port)
{
    struct sat_link *l = &sl[k];
    if (sl_udp_bind(l, local_port) < 0) { sl_over(k, "no UDP port for the game's octets"); return; }
    l->peer_port = (uint16_t)peer_port;
    sl_begin(k, now_ms, 0, 0);
}

/* ---- matchmaking (saturn.h): STUN, then the match server ---- */
/* a fresh, unguessable STUN transaction id (so a reply cannot be forged by
 * an off-path sender): the kernel's random source, never a predictable PRNG */
static void sl_random(uint8_t *b, size_t n)
{
    static int fd = -2;
    size_t got = 0; ssize_t r;
    if (fd == -2) fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    while (fd >= 0 && got < n && (r = read(fd, b + got, n - got)) > 0) got += (size_t)r;
    /* /dev/urandom should never fail on Linux; if it did, at least do not
     * reuse a fixed id */
    for (; got < n; got++) { static uint32_t x; x = x * 1664525u + 1013904223u + (uint32_t)time(NULL); b[got] = (uint8_t)(x >> 16); }
}
static void sl_stun_send(struct sat_link *l)
{
    uint8_t req[STUN_REQ_LEN]; struct sockaddr_in sa;
    sl_random(l->txid, STUN_TXID_LEN);
    stun_request(req, l->txid);
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(stun_port); sa.sin_addr.s_addr = htonl(stun_ip);
    (void)sendto(l->udp, req, sizeof req, MSG_DONTWAIT | MSG_NOSIGNAL, (struct sockaddr *)(void *)&sa, sizeof sa);
}
/* The STUN server's address, learnt once (the test gives it with -S). A
 * name lookup can take seconds: a child makes it, while a port has
 * matchmaking on, and hands the address back over a pipe; the loop never
 * waits for it. A call that comes before it is known is ended. */
static pid_t stun_pid; static int stun_fd = -1; static uint32_t stun_next;
static void sl_stun_poll(uint32_t now_ms)
{
    int k, want = 0, pp[2];
    if (stun_pid > 0) {
        uint8_t a[4]; ssize_t r = read(stun_fd, a, sizeof a);
        if (r == 4) stun_ip = (uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3];
        if (r == 4 || r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
            close(stun_fd); stun_fd = -1;
            (void)kill(stun_pid, SIGKILL); (void)waitpid(stun_pid, NULL, 0);   /* done, or as good as: it has written or gone */
            stun_pid = 0;
        }
        return;
    }
    if (stun_ip) return;
    for (k = 0; k < n_ports; k++) if (S[k].cfg.service == CFG_SERVICE_SATURN && S[k].cfg.sat_match) want = 1;
    if (!want || (int32_t)(now_ms - stun_next) < 0) return;
    stun_next = now_ms + 60000;                 /* not found: again in a minute */
    if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) return;
    stun_pid = fork();
    if (stun_pid == 0) {
        struct addrinfo hints, *res = NULL;
        child_plain(-1, -1, 0);
        memset(&hints, 0, sizeof hints); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM;
        if (getaddrinfo(SATURN_STUN_HOST, NULL, &hints, &res) == 0 && res) {
            uint32_t ip = ntohl(((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr.s_addr);
            uint8_t a[4] = { (uint8_t)(ip >> 24), (uint8_t)(ip >> 16), (uint8_t)(ip >> 8), (uint8_t)ip };
            if (write(pp[1], a, sizeof a) != (ssize_t)sizeof a) _exit(1);
        }
        _exit(0);
    }
    close(pp[1]);
    if (stun_pid < 0) { stun_pid = 0; close(pp[0]); return; }
    stun_fd = pp[0];
}
/* start a match-server query for `action`; the answer is read over a pipe */
static void sl_query(int k, const char *action)
{
    struct sat_link *l = &sl[k]; int pp[2]; char ip12[13], url[300], portstr[8];
    saturn_ip12(l->pub, ip12);
    snprintf(portstr, sizeof portstr, "%u", (unsigned)l->pub_port);
    if (pipe2(pp, O_CLOEXEC | O_NONBLOCK) < 0) return;
    l->q_len = 0; l->q_at = 0;
    l->q_pid = fork();
    if (l->q_pid == 0) {
        (void)fcntl(pp[1], F_SETFL, 0);
        child_plain(-1, pp[1], 0);
        if (mm_cmd[0])
            execl("/bin/sh", "sh", "-c", mm_cmd, "mm", action, l->game, ip12, portstr, (char *)NULL);
        snprintf(url, sizeof url, "https://%s%s?action=%s&gameID=%s&client_ip=%s&port=%s&key=%s",
                 SATURN_MATCH_HOST, SATURN_MATCH_PATH, action, l->game, ip12, portstr, SATURN_MATCH_KEY);
        execlp("wget", "wget", "-q", "-T", "8", "-O", "-", "-U", SATURN_MATCH_AGENT, url, (char *)NULL);
        _exit(127);
    }
    close(pp[1]);
    if (l->q_pid < 0) { l->q_pid = 0; close(pp[0]); return; }
    l->q_fd = pp[0];
}
static void sl_qdrain(struct sat_link *l)
{
    ssize_t r;
    while (l->q_fd >= 0 && l->q_len + 1 < sizeof l->q_text &&
           (r = read(l->q_fd, l->q_text + l->q_len, sizeof l->q_text - 1 - l->q_len)) > 0)
        l->q_len += (size_t)r;
}
/* an address the match server returns may be a player on the public
 * internet and nothing else: never loopback, a private or CGNAT range,
 * link-local, multicast, broadcast, or this box's own public address. A
 * server that is confused or hostile must not aim the game's UDP at an
 * address inside the network. */
static int sl_peer_ok(uint32_t a, uint32_t pub)
{
    unsigned hi = a >> 24;
    if (mm_any) return 1;                                   /* the test uses loopback */
    if (a == 0 || a == 0xFFFFFFFFu || a == pub) return 0;
    if (hi == 127 || hi == 10 || hi >= 224) return 0;       /* loopback, 10/8, multicast and up */
    if ((a & 0xFFF00000u) == 0xAC100000u) return 0;         /* 172.16/12 */
    if ((a & 0xFFFF0000u) == 0xC0A80000u) return 0;         /* 192.168/16 */
    if ((a & 0xFFC00000u) == 0x64400000u) return 0;         /* 100.64/10 (CGNAT) */
    if ((a & 0xFFFF0000u) == 0xA9FE0000u) return 0;         /* 169.254/16 (link-local) */
    return 1;
}

void satlink_poll(uint32_t now_ms)
{
    int k;
    sl_stun_poll(now_ms);
    for (k = 0; k < n_ports; k++) {
        struct sat_link *l = &sl[k]; uint32_t addr = 0; int on = 0, kind = srv_link(&S[k], &addr, &on);
        struct sockaddr_in sa; socklen_t sl_n; ssize_t r; int i; char a[16];

        if (kind == SATURN_NONE || kind == SRV_LINK_BBS || kind == SRV_LINK_SVC) {      /* a bulletin board or service call is bbslink.c's */ if (l->st != SL_IDLE) { sl_close(l); l->st = SL_IDLE; } continue; }
        switch (l->st) {
        case SL_IDLE:
            if (!on) break;                         /* the modem first */
            l->t0 = now_ms; l->next_try = now_ms; l->hello_n = 0;
            if (kind == SATURN_MATCH_WAIT || kind == SATURN_MATCH_CALL) {
                /* matchmaking: a UDP socket of our own, learn its public
                 * address by STUN, then ask the match server to pair us */
                l->mm_wait = kind == SATURN_MATCH_WAIT;
                { const char *g = srv_link_game(&S[k]); l->game[0] = g[0]; l->game[1] = g[1]; l->game[2] = 0; }
                if (!stun_ip) { sl_over(k, "matchmaking needs the STUN server address and has none"); break; }
                if (sl_udp_bind(l, l->mm_wait ? SATURN_UDP_WAIT : SATURN_UDP_CALL) < 0) { sl_over(k, "no UDP port for matchmaking"); break; }
                l->pub = 0; l->stun_tries = 0; l->stun_at = now_ms; l->q_asked = 0; l->q_pid = 0; l->q_fd = -1;
                sl_stun_send(l); l->stun_tries = 1;
                l->st = SL_MM_STUN;
                logf_(CFG_LOG_INFO, "port %d: matchmaking for game %s (%s)", k + 1, l->game, l->mm_wait ? "waiting" : "calling");
                break;
            }
            if (kind == SATURN_WAIT) {
                memset(&sa, 0, sizeof sa);
                sa.sin_family = AF_INET; sa.sin_port = htons(SATURN_TCP_PORT);
                l->lis = sl_tcp();
                if (l->lis < 0 || bind(l->lis, (struct sockaddr *)(void *)&sa, sizeof sa) < 0 || listen(l->lis, 2) < 0) {
                    sl_over(k, "cannot wait for the other player: the port for it is taken");
                    break;
                }
                l->st = SL_LISTEN;
                logf_(CFG_LOG_INFO, "port %d: waiting for the other player to call", k + 1);
            } else {
                l->peer = addr;
                l->st = SL_CONNECT;
                logf_(CFG_LOG_INFO, "port %d: calling the player at %s", k + 1, ip_str(addr, a));
            }
            break;
        case SL_LISTEN:
            sl_n = sizeof sa;
            l->tcp = accept4(l->lis, (struct sockaddr *)(void *)&sa, &sl_n, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (l->tcp >= 0) { l->peer = ntohl(sa.sin_addr.s_addr); l->hello_n = 0; l->t_hello = now_ms; l->st = SL_HELLO_IN; }
            else if (now_ms - l->t0 >= SL_WAIT_MS) sl_over(k, "nobody called");
            break;
        case SL_HELLO_IN:
            r = recv(l->tcp, l->hello + l->hello_n, 7 - l->hello_n, 0);
            if (r > 0) l->hello_n += (size_t)r;
            if (l->hello_n == 7 && memcmp(l->hello, SATURN_HELLO, 7) == 0) {
                (void)send(l->tcp, SATURN_HELLO_ACK, 5, MSG_NOSIGNAL);
                if (sl_udp_bind(l, SATURN_UDP_WAIT) < 0) { sl_over(k, "no UDP port for the game's octets"); break; }
                l->peer_port = SATURN_UDP_CALL;
                sl_begin(k, now_ms, 1, 0);          /* the waiting game is shown RING + CONNECT */
            } else if (l->hello_n == 7 || r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR) || now_ms - l->t_hello >= SL_HELLO_MS) {
                close(l->tcp); l->tcp = -1; l->st = SL_LISTEN;      /* not the other player */
            }
            break;
        case SL_CONNECT:
            if (l->tcp < 0) {
                if ((int32_t)(now_ms - l->next_try) < 0) break;
                if (now_ms - l->t0 >= SL_REACH_MS) { sl_over(k, "the other player's box does not answer"); break; }
                l->next_try = now_ms + 2000u;
                l->tcp = sl_tcp();
                if (l->tcp < 0) break;
                memset(&sa, 0, sizeof sa);
                sa.sin_family = AF_INET; sa.sin_port = htons(SATURN_TCP_PORT); sa.sin_addr.s_addr = htonl(l->peer);
                if (connect(l->tcp, (struct sockaddr *)(void *)&sa, sizeof sa) < 0 && errno != EINPROGRESS) { close(l->tcp); l->tcp = -1; }
            } else {
                struct pollfd pf; int err = 0; socklen_t el = sizeof err;
                pf.fd = l->tcp; pf.events = POLLOUT; pf.revents = 0;
                if (poll(&pf, 1, 0) <= 0) {
                    if ((int32_t)(now_ms - l->next_try) >= 0) { close(l->tcp); l->tcp = -1; }     /* try again */
                    break;
                }
                if (getsockopt(l->tcp, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) { close(l->tcp); l->tcp = -1; break; }
                (void)send(l->tcp, SATURN_HELLO, 7, MSG_NOSIGNAL);
                l->hello_n = 0; l->t_hello = now_ms; l->st = SL_HELLO_OUT;
            }
            break;
        case SL_HELLO_OUT:
            r = recv(l->tcp, l->hello + l->hello_n, 5 - l->hello_n, 0);
            if (r > 0) l->hello_n += (size_t)r;
            if (l->hello_n == 5 && memcmp(l->hello, SATURN_HELLO_ACK, 5) == 0) sl_linked(k, now_ms, SATURN_UDP_CALL, SATURN_UDP_WAIT);
            else if (l->hello_n == 5 || r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR) || now_ms - l->t_hello >= SL_HELLO_MS)
                sl_over(k, "the box at that address is not waiting for a call");
            break;
        case SL_MM_STUN: {
            static uint8_t dg[512];
            sl_n = sizeof sa;
            while ((r = recvfrom(l->udp, dg, sizeof dg, 0, (struct sockaddr *)(void *)&sa, &sl_n)) > 0) {
                if (ntohl(sa.sin_addr.s_addr) != stun_ip || ntohs(sa.sin_port) != stun_port) continue;  /* only the STUN server */
                if (stun_parse(dg, (size_t)r, l->txid, &l->pub, &l->pub_port)) break;
            }
            if (l->pub) {
                char b[16];
                logf_(CFG_LOG_INFO, "port %d: our public address is %s:%u (STUN)", k + 1, ip_str(l->pub, b), (unsigned)l->pub_port);
                l->st = SL_MM_QUERY; l->q_at = now_ms; l->q_asked = 0;
                break;
            }
            if ((int32_t)(now_ms - l->stun_at) >= 0 && (int32_t)(now_ms - l->stun_at) < 100000) {
                if (now_ms - l->t0 >= 8000u) { sl_over(k, "no answer from the STUN server"); break; }
                if (l->stun_tries < 16 && (l->stun_at == 0 || now_ms - l->stun_at >= 500u)) { sl_stun_send(l); l->stun_at = now_ms; l->stun_tries++; }
            }
            break; }
        case SL_MM_QUERY: {
            int st2;
            if (l->q_pid > 0) {                      /* a query is running */
                sl_qdrain(l);
                if (waitpid(l->q_pid, &st2, WNOHANG) != l->q_pid) break;
                sl_qdrain(l);
                l->q_pid = 0; close(l->q_fd); l->q_fd = -1;
                {
                    uint32_t opp = 0; uint16_t op = 0;
                    const char *want = l->mm_wait ? "matched" : "found opponent";
                    if (WIFEXITED(st2) && WEXITSTATUS(st2) == 0 &&
                        saturn_match_parse(l->q_text, l->q_len, want, &opp, &op)) {
                        if (op < 1024 || !sl_peer_ok(opp, l->pub)) { sl_over(k, "the match server gave an address that cannot be a player"); break; }
                        l->peer = opp; l->peer_port = op;
                        sl_begin(k, now_ms, l->mm_wait, 1);  /* the waiting game is shown RING + CONNECT */
                        break;
                    }
                }
                l->q_at = now_ms + 2000u;            /* ask again shortly */
                if (l->mm_wait && now_ms - l->t0 >= SL_WAIT_MS) { sl_query(k, "timeout"); sl_over(k, "nobody was paired"); break; }
                if (!l->mm_wait && now_ms - l->t0 >= SL_REACH_MS) { sl_over(k, "nobody was paired"); break; }
                break;
            }
            if ((int32_t)(now_ms - l->q_at) < 0) break;
            /* keep the NAT mapping fresh while we wait */
            sl_stun_send(l);
            sl_query(k, !l->q_asked ? (l->mm_wait ? "wait" : "match") : (l->mm_wait ? "status" : "match"));
            l->q_asked = 1;
            break; }
        case SL_LINKED:
            for (i = 0; i < 32; i++) {
                static uint8_t dg[8192];
                sl_n = sizeof sa;
                r = recvfrom(l->udp, dg, sizeof dg, 0, (struct sockaddr *)(void *)&sa, &sl_n);
                if (r <= 0) break;
                if (ntohl(sa.sin_addr.s_addr) != l->peer) continue;     /* only the other player is heard */
                /* its port as its router shows it to us: the first one heard, and no other after */
                if (!l->port_known) { l->peer_port = ntohs(sa.sin_port); l->port_known = 1; }
                else if (ntohs(sa.sin_port) != l->peer_port) continue;
                l->heard = now_ms;
                switch (saturn_control(dg, (size_t)r)) {
                case 1: sl_send(l, SATURN_PONG, sizeof SATURN_PONG - 1); break;
                case 2: break;
                default: l->dn_n += saturn_unpack(&l->rx, dg, (size_t)r, l->dn + l->dn_n, sizeof l->dn - l->dn_n); break;
                }
            }
            if (l->dn_n) {
                size_t took = srv_send_raw(&S[k], l->dn, l->dn_n);
                if (took) { memmove(l->dn, l->dn + took, l->dn_n - took); l->dn_n -= took; }
            }
            sl_flush(l);
            if (now_ms - l->ping_at >= 1000u) { l->ping_at = now_ms; sl_send(l, SATURN_PING, sizeof SATURN_PING - 1); }
            if (now_ms - l->heard >= SL_SILENT_MS) sl_over(k, "the other player is gone");
            break;
        default:        /* SL_OVER: until the call has ended */
            break;
        }
    }
}

/* ---- test overrides (satlink.h) ---- */
void satlink_set_stun(uint32_t ip, uint16_t port) { stun_ip = ip; stun_port = port ? port : SATURN_STUN_PORT; }
void satlink_set_match(const char *cmd) { mm_cmd = cmd ? cmd : ""; }
void satlink_set_any(int on) { mm_any = on != 0; }
