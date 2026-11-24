/* v90arch - archive mode gateway of the v90d dial-up server.
 *
 * The kernel redirects the caller's DNS (UDP 53), HTTP (TCP 80) and FTP
 * (TCP 21 and a small passive port range) to this program. It answers every
 * DNS question with an address from a private pool, and takes the HTTP and
 * FTP connections made to those addresses to the Protoweb proxy, which
 * serves restored old sites. The old PC needs no proxy setting.
 *
 * One process, one poll() loop, non-blocking sockets, fixed tables, no
 * allocation after startup (the one exception is inside getaddrinfo when
 * the proxy is given by name). See README.md. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "dns.h"
#include "http.h"
#include "listing.h"
#include "ftp.h"

#define MAX_SESS   12            /* TCP sessions at a time; more are refused */
#define PEER_SESS  8             /* of them, from one caller: the other line keeps some */
#define HEAD_S     30            /* the whole request head, from the connect on */
#define IN_MAX     8192          /* request head limit; also the relay buffer */
#define UP_MAX     (IN_MAX + 1024)
#define DN_MAX     2048          /* converted listing on its way to the client */
#define IDLE_S     120           /* a session with nothing moving is dropped (-t) */
#define CONNECT_S  20            /* connect to the proxy; not longer than the idle time */
#define LINGER_S   30            /* wait for the client to close after the answer; the same */
#define RESOLVE_S  60            /* the proxy name is looked up again at most this often */
#define SOCK_BUF   16384         /* kernel buffer asked for per socket: the line is slow,
                                  * there is no point in queueing megabytes on the box */

enum { K_FREE = 0, K_HTTP, K_FTP };
enum { H_HEAD, H_CONNECT, H_RELAY, H_LINGER };                     /* HTTP session */
enum { X_IDLE, X_CONNECT, X_SEND, X_HEAD, X_DATAWAIT, X_STREAM };  /* FTP fetch */
enum { R_SIG, R_DNS, R_LHTTP, R_LFTP, R_RES, R_C, R_P, R_D, R_L };        /* what a polled fd is */

typedef struct {
    int      kind, state;
    int      cfd;                /* client (FTP: the control connection) */
    int      pfd;                /* proxy */
    int      dfd, lfd;           /* FTP data connection, passive listener */
    unsigned lport;
    unsigned gen;                /* bumped whenever one of the fds changes */
    long     deadline;
    uint32_t peer, orig;         /* client address, address it wanted to reach */
    int      c_eof, p_eof, strip, closing, d_conn, x_state, act, x_err, lst_done;
    int      x_open;             /* the 150 went out: the data connection is this fetch's */
    long     body_left, x_clen;
    unsigned long x_got, n_up, n_down;
    unsigned n_cmd, n_xfer;
    size_t   in_len, in_off, up_len, up_off, dn_len, dn_off, ln_len, rp_len, rp_off;
    char     host[HTTP_HOST_MAX];
    char     method[16];
    ftp_t    ftp;
    lst_t    lst;
    char     ln[512];            /* FTP command line being read */
    char     rp[768];            /* FTP replies on their way out */
    /* the big buffers last: a new session clears only what is above */
    char     in[IN_MAX];         /* HTTP: request head, then the response; FTP: from the proxy */
    char     up[UP_MAX];         /* towards the proxy */
    char     dn[DN_MAX];
} sess_t;

static sess_t sess[MAX_SESS];
static dns_t  dns;

static char     proxy_host[128] = "wayback.protoweb.org";
static unsigned proxy_port = 7851, dns_port = 8053, http_port = 8080, ftp_port = 8021;
static unsigned pasv_lo = 50000, pasv_hi = 50015, pasv_next;
static uint32_t bind_ip;         /* host order, 0 = any */
static int      verbose;
static int      redirected_only; /* -r: a connection made to our port itself is refused */
static unsigned idle_s = IDLE_S, connect_s = CONNECT_S, linger_s = LINGER_S;

static struct sockaddr_in proxy_sa;
static int  proxy_ok, proxy_numeric, proxy_stale, proxy_tried_once;
static long proxy_tried;

static int sig_pipe[2] = { -1, -1 };

static struct pollfd pfds[5 + MAX_SESS * 4];
static struct { int role; unsigned gen; sess_t *s; } pmeta[5 + MAX_SESS * 4];
static int npfd;

static const char page_502[] = "Archive mode could not reach the archive (the Protoweb proxy).\r\n";

static long now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

static void logv(const char *fmt, ...)
{
    va_list ap;
    if (!verbose) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int again(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }

static int nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    return fl < 0 ? -1 : fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void small_buf(int fd, int opt)
{
    int v = SOCK_BUF;
    setsockopt(fd, SOL_SOCKET, opt, &v, sizeof v);
}

static void fill_sa(struct sockaddr_in *sa, uint32_t ip, unsigned port)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(ip);
    sa->sin_port = htons((uint16_t)port);
}

static int listen_on(int type, uint32_t ip, unsigned port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, type, 0), one = 1;

    if (fd < 0) return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    fill_sa(&sa, ip, port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        (type == SOCK_STREAM && listen(fd, 8) != 0) || nonblock(fd) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* The address the client wanted to reach. Behind a kernel redirect that is
 * the original destination kept by conntrack; without one (tests on a PC)
 * it is simply the local address of the connection. */
static uint32_t orig_dst(int fd, unsigned *port)
{
    struct sockaddr_in sa;
    socklen_t len = sizeof sa;

    *port = 0;
#ifdef __linux__
    memset(&sa, 0, sizeof sa);
    if (getsockopt(fd, IPPROTO_IP, 80 /* SO_ORIGINAL_DST */, &sa, &len) == 0 && sa.sin_family == AF_INET) {
        *port = ntohs(sa.sin_port);
        return ntohl(sa.sin_addr.s_addr);
    }
    len = sizeof sa;
#endif
    memset(&sa, 0, sizeof sa);
    if (getsockname(fd, (struct sockaddr *)&sa, &len) != 0) return 0;
    return ntohl(sa.sin_addr.s_addr);
}

/* pending error of a socket whose connect() was in progress; 0 = connected */
static int sock_err(int fd)
{
    int e = 0;
    socklen_t len = sizeof e;
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0) return errno ? errno : -1;
    return e;
}

/* --- the proxy ----------------------------------------------------------- */

/* Look the proxy up: at startup and after a failed connect, never more
 * often than once in RESOLVE_S. The lookup can take seconds, and every
 * session would wait for it: a child process makes it and writes the
 * addresses (IPv4, network order, at most RES_MAX) to a pipe the loop polls
 * (res_fd, proxy_answer). After a failure another address of the name is
 * preferred when there is one. */
#define RES_MAX 8
static int   res_fd = -1;
static pid_t res_pid;

static void proxy_resolve(void)
{
    int p[2];
    long t = now_s();

    if (proxy_numeric || res_fd >= 0) return;
    if (proxy_tried_once && t - proxy_tried < RESOLVE_S) return;
    proxy_tried_once = 1;
    proxy_tried = t;
    if (pipe(p) != 0) return;
    res_pid = fork();
    if (res_pid == 0) {
        struct addrinfo hints, *res = NULL, *ai;
        uint32_t a[RES_MAX];
        size_t n = 0;
        close(p[0]);
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(proxy_host, NULL, &hints, &res) == 0) {
            for (ai = res; ai != NULL && n < RES_MAX; ai = ai->ai_next)
                if (ai->ai_family == AF_INET) a[n++] = ((struct sockaddr_in *)(void *)ai->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
        if (n > 0 && write(p[1], a, n * sizeof a[0]) < 0) _exit(1);   /* one write, under PIPE_BUF: whole */
        _exit(0);
    }
    close(p[1]);
    if (res_pid < 0) { close(p[0]); return; }
    res_fd = p[0];
    (void)fcntl(res_fd, F_SETFD, FD_CLOEXEC);
}

static void proxy_answer(void)
{
    uint32_t a[RES_MAX];
    ssize_t n = read(res_fd, a, sizeof a);
    size_t i, k = n > 0 ? (size_t)n / sizeof a[0] : 0;

    close(res_fd);
    res_fd = -1;
    (void)waitpid(res_pid, NULL, 0);     /* it has written, or ended: no wait to speak of */
    if (k == 0) { logv("proxy %s does not resolve", proxy_host); return; }
    for (i = 0; i < k; i++) {
        if (proxy_ok && proxy_stale && a[i] == proxy_sa.sin_addr.s_addr && i + 1 < k) continue;
        proxy_sa.sin_addr.s_addr = a[i];
        proxy_ok = 1;
        break;
    }
    proxy_stale = 0;
}

static void proxy_failed(void) { proxy_stale = 1; }

static void p_close(sess_t *s)
{
    if (s->pfd >= 0) { close(s->pfd); s->pfd = -1; s->gen++; }
}

/* start a non-blocking connect to the proxy; 0 when under way */
static int proxy_start(sess_t *s)
{
    int fd;

    if (!proxy_ok || proxy_stale) proxy_resolve();
    if (!proxy_ok) return -1;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    small_buf(fd, SO_RCVBUF);
    if (nonblock(fd) != 0 ||
        (connect(fd, (struct sockaddr *)&proxy_sa, sizeof proxy_sa) != 0 && errno != EINPROGRESS)) {
        close(fd);
        proxy_failed();
        return -1;
    }
    s->pfd = fd;
    s->gen++;
    s->deadline = now_s() + (long)connect_s;
    return 0;
}

/* --- sessions ------------------------------------------------------------ */

static void touch(sess_t *s) { s->deadline = now_s() + (long)idle_s; }

static void d_close(sess_t *s)
{
    if (s->dfd >= 0) { close(s->dfd); s->dfd = -1; s->gen++; }
    s->d_conn = 0;
}

static void l_close(sess_t *s)
{
    if (s->lfd >= 0) { close(s->lfd); s->lfd = -1; s->gen++; }
}

static void sess_close(sess_t *s, const char *why)
{
    char who[20];

    if (verbose) {
        dns_dotted(s->peer, who, sizeof who);
        if (s->kind == K_HTTP)
            logv("http %s %s %s up=%lu down=%lu %s", who, s->host[0] ? s->host : "-",
                 s->method[0] ? s->method : "-", s->n_up, s->n_down, why);
        else
            logv("ftp %s %s cmds=%u transfers=%u down=%lu %s", who, s->ftp.host,
                 s->n_cmd, s->n_xfer, s->n_down, why);
    }
    p_close(s);
    d_close(s);
    l_close(s);
    close(s->cfd);
    s->cfd = -1;
    s->gen++;
    s->kind = K_FREE;
}

static sess_t *sess_new(int kind, int fd, const struct sockaddr_in *peer)
{
    uint32_t ip = ntohl(peer->sin_addr.s_addr);
    unsigned port;
    int i, mine = 0;

    for (i = 0; i < MAX_SESS; i++) if (sess[i].kind != K_FREE && sess[i].peer == ip) mine++;
    if (mine >= PEER_SESS) return NULL;
    for (i = 0; i < MAX_SESS; i++) {
        sess_t *s = &sess[i];
        unsigned gen = s->gen;
        if (s->kind != K_FREE) continue;
        memset(s, 0, offsetof(sess_t, in));
        s->gen = gen + 1;
        s->kind = kind;
        s->cfd = fd;
        s->pfd = s->dfd = s->lfd = -1;
        s->peer = ip;
        s->orig = orig_dst(fd, &port);
        s->x_clen = -1;
        touch(s);
        /* the whole head in this time, not moved on by a byte at a time */
        if (kind == K_HTTP) s->deadline = now_s() + (long)(idle_s < HEAD_S ? idle_s : HEAD_S);
        return s;
    }
    return NULL;
}

/* --- HTTP relay ---------------------------------------------------------- */

/* answer the client ourselves: the page goes out through the relay path */
static void http_page(sess_t *s, const char *status, const char *text)
{
    int n = snprintf(s->in, IN_MAX, "HTTP/1.0 %s\r\nContent-Type: text/plain\r\nContent-Length: %u\r\n"
                     "Connection: close\r\n\r\n%s", status, (unsigned)strlen(text), text);
    s->in_off = 0;
    s->in_len = (n > 0 && n < IN_MAX) ? (size_t)n : 0;
    s->up_off = s->up_len = 0;
    s->body_left = 0;
    s->strip = 0;
    s->p_eof = 1;
    s->x_got = 1;
    p_close(s);
    s->state = H_RELAY;
    touch(s);
}

/* more of the request head arrived: when it is whole, rewrite it and go */
static void http_head(sess_t *s)
{
    http_req_t r;
    size_t hl = http_head_len(s->in, s->in_len), rest, take;
    long n;

    if (hl == 0) {
        if (s->in_len >= IN_MAX) http_page(s, "400 Bad Request", "Request head too large for archive mode.\r\n");
        return;
    }
    if (http_parse(s->in, hl, &r) != 0) {
        http_page(s, "400 Bad Request", "Not a request archive mode can pass on.\r\n");
        return;
    }
    if (r.chunked) {             /* a body we would not know the end of; no browser of the time sends one */
        http_page(s, "411 Length Required", "Archive mode takes a request body with a Content-Length only.\r\n");
        return;
    }
    memcpy(s->method, r.method, sizeof s->method);
    dns_host_for(&dns, r.host, s->orig, s->host, sizeof s->host);
    n = http_build(s->in, hl, &r, s->host, s->up, UP_MAX);
    if (n < 0) {
        http_page(s, "400 Bad Request", "Not a request archive mode can pass on.\r\n");
        return;
    }
    s->up_off = 0;
    s->up_len = (size_t)n;
    /* body bytes that came with the head */
    rest = s->in_len - hl;
    s->body_left = r.body;
    take = rest;
    if ((long)take > s->body_left) take = (size_t)s->body_left;
    if (take > UP_MAX - s->up_len) take = UP_MAX - s->up_len;
    memcpy(s->up + s->up_len, s->in + hl, take);
    s->up_len += take;
    s->body_left -= (long)take;
    s->in_len = s->in_off = 0;
    s->strip = r.v09;            /* an HTTP/0.9 client wants no response head */
    if (proxy_start(s) != 0) {
        http_page(s, "502 Bad Gateway", page_502);
        return;
    }
    s->state = H_CONNECT;
}

/* client data after the head: the body up to Content-Length goes to the
 * proxy, anything more (the stray CR LF old browsers add) is dropped. It
 * has to be read: closing a socket with unread input resets it. */
static void http_client_in(sess_t *s)
{
    static char waste[512];
    ssize_t n;

    if (s->body_left > 0) {
        size_t room;
        if (s->up_off == s->up_len) s->up_off = s->up_len = 0;
        room = UP_MAX - s->up_len;
        if (room == 0) return;
        if ((long)room > s->body_left) room = (size_t)s->body_left;
        n = read(s->cfd, s->up + s->up_len, room);
        if (n > 0) { s->up_len += (size_t)n; s->body_left -= (long)n; }
    } else {
        n = read(s->cfd, waste, sizeof waste);
    }
    if (n == 0) s->c_eof = 1;
    else if (n < 0 && !again()) sess_close(s, "client error");
    else if (n > 0) touch(s);
}

static void http_event(sess_t *s, int role, int rev)
{
    ssize_t n;
    int st;
    long hl;

    if (role == R_C) {
        if ((rev & POLLOUT) && s->state == H_RELAY && s->in_off < s->in_len && !s->strip) {
            n = write(s->cfd, s->in + s->in_off, s->in_len - s->in_off);
            if (n < 0 && !again()) { sess_close(s, "client gone"); return; }
            if (n > 0) { s->in_off += (size_t)n; s->n_down += (unsigned long)n; touch(s); }
        }
        if (rev & POLLIN) {
            if (s->state == H_HEAD) {
                n = read(s->cfd, s->in + s->in_len, IN_MAX - s->in_len);
                if (n == 0 || (n < 0 && !again())) { sess_close(s, "closed before request"); return; }
                if (n > 0) { s->in_len += (size_t)n; http_head(s); }
            } else if (s->state == H_LINGER) {
                static char waste[512];
                n = read(s->cfd, waste, sizeof waste);
                if (n == 0 || (n < 0 && !again())) { sess_close(s, "done"); return; }
            } else {
                http_client_in(s);
                if (s->kind == K_FREE) return;
            }
        } else if (rev & (POLLERR | POLLHUP)) {
            sess_close(s, s->state == H_LINGER ? "done" : "client gone");
            return;
        }
    } else if (role == R_P) {
        if (s->state == H_CONNECT) {
            if (sock_err(s->pfd) != 0) {
                proxy_failed();
                http_page(s, "502 Bad Gateway", page_502);
            } else {
                s->state = H_RELAY;
                touch(s);
            }
        } else if (s->state == H_RELAY) {
            if ((rev & POLLOUT) && s->up_off < s->up_len) {
                n = write(s->pfd, s->up + s->up_off, s->up_len - s->up_off);
                if (n < 0 && !again()) s->p_eof = 1;
                if (n > 0) { s->up_off += (size_t)n; s->n_up += (unsigned long)n; touch(s); }
            }
            if ((rev & (POLLIN | POLLERR | POLLHUP)) && !s->p_eof) {
                if (s->in_off == s->in_len) s->in_off = s->in_len = 0;
                if (s->in_len < IN_MAX) {
                    n = read(s->pfd, s->in + s->in_len, IN_MAX - s->in_len);
                    if (n > 0) { s->in_len += (size_t)n; s->x_got += (unsigned long)n; touch(s); }
                    else if (n == 0 || !again()) s->p_eof = 1;
                }
            }
        }
    }

    if (s->state != H_RELAY) return;
    if (s->strip) {
        hl = http_resp_head(s->in, s->in_len, &st);
        if (hl > 0) { s->in_off = (size_t)hl; s->strip = 0; }
        else if (hl < 0 || s->p_eof || s->in_len >= IN_MAX) s->strip = 0;
    }
    if (s->p_eof) p_close(s);
    if (s->p_eof && s->in_off == s->in_len) {
        if (s->x_got == 0) {     /* the proxy went away without a word */
            http_page(s, "502 Bad Gateway", page_502);
            return;
        }
        /* all sent: say so, then wait for the client to close its side */
        shutdown(s->cfd, SHUT_WR);
        if (s->c_eof) { sess_close(s, "done"); return; }
        s->state = H_LINGER;
        s->deadline = now_s() + (long)linger_s;
    }
}

/* --- FTP gateway --------------------------------------------------------- */

static void rp_put(sess_t *s, const char *text, size_t n)
{
    if (s->rp_off == s->rp_len) s->rp_off = s->rp_len = 0;
    if (n > sizeof s->rp - s->rp_len) return;    /* cannot happen: one command at a time */
    memcpy(s->rp + s->rp_len, text, n);
    s->rp_len += n;
}

static void xfer_drop(sess_t *s)
{
    p_close(s);
    d_close(s);
    l_close(s);
    s->x_state = X_IDLE;
}

/* a fetch ended one way or another: tell the client. One that failed
 * before its 150, with no data connection made yet, leaves the PASV or
 * PORT for the next command, as any FTP server does. (A client that has
 * connected closes that connection on the 550, as a rule: it goes.) */
static void xfer_end(sess_t *s, int result)
{
    char rep[FTP_REPLY_MAX];
    size_t n;
    int keep = !s->x_open && s->dfd < 0 && (result == FTP_R_NOTFOUND || result == FTP_R_NOPROXY),
        data = s->ftp.data;

    if (s->act == FTP_LIST || s->act == FTP_NLST || s->act == FTP_RETR) s->n_xfer++;
    if (keep) { p_close(s); s->x_state = X_IDLE; }
    else xfer_drop(s);
    n = ftp_reply_done(&s->ftp, result, rep, sizeof rep);
    if (keep) s->ftp.data = data;
    rp_put(s, rep, n);
    touch(s);
}

static unsigned pasv_open(sess_t *s)
{
    unsigned count = pasv_hi - pasv_lo + 1, i;
    int k;

    for (i = 0; i < count; i++) {
        unsigned port = pasv_lo + (pasv_next++ % count);
        int fd, taken = 0;
        for (k = 0; k < MAX_SESS; k++)
            if (sess[k].kind == K_FTP && sess[k].lfd >= 0 && sess[k].lport == port) taken = 1;
        if (taken) continue;
        fd = listen_on(SOCK_STREAM, bind_ip, port);
        if (fd < 0) continue;
        s->lfd = fd;
        s->lport = port;
        s->gen++;
        return port;
    }
    return 0;
}

static void xfer_start(sess_t *s, int act)
{
    char url[3 * FTP_PATH_MAX + FTP_HOST_MAX + 16];
    int dir = (act == FTP_LIST || act == FTP_NLST || act == FTP_CWD), n;

    s->act = act;
    s->in_len = s->in_off = s->dn_len = s->dn_off = 0;
    s->p_eof = s->x_err = s->lst_done = s->x_open = 0;
    s->x_got = 0;
    s->x_clen = -1;
    if (ftp_url(&s->ftp, dir, url, sizeof url) < 0) { xfer_end(s, FTP_R_NOTFOUND); return; }
    n = snprintf(s->up, UP_MAX, "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
                 act == FTP_SIZE ? "HEAD" : "GET", url, s->ftp.host);
    s->up_off = 0;
    s->up_len = (size_t)n;
    if (proxy_start(s) != 0) { xfer_end(s, FTP_R_NOPROXY); return; }
    s->x_state = X_CONNECT;
}

static void ftp_act(sess_t *s, int act)
{
    char rep[FTP_REPLY_MAX];
    size_t n;

    switch (act) {
    case FTP_CLOSE:
        xfer_drop(s);
        s->closing = 1;
        break;
    case FTP_ABORT:
        xfer_drop(s);
        break;
    case FTP_PASV:
        /* The 227 names the address the client believes it is talking to;
         * the kernel redirect brings the data connection back to us. */
        d_close(s);
        l_close(s);
        n = ftp_reply_pasv(&s->ftp, s->orig, pasv_open(s), rep, sizeof rep);
        rp_put(s, rep, n);
        break;
    case FTP_PORT:
        d_close(s);
        l_close(s);
        break;
    case FTP_LIST: case FTP_NLST: case FTP_RETR: case FTP_CWD: case FTP_SIZE:
        xfer_start(s, act);
        break;
    default:
        break;
    }
}

/* While a fetch is busy, the lines waiting behind the first are looked
 * through for an ABOR or QUIT (ftp_line answers DEFER to anything else):
 * that one is taken out of the queue and done at once. */
static void ftp_urgent(sess_t *s)
{
    char line[sizeof s->ln], rep[FTP_REPLY_MAX];
    size_t at = 0;

    while (at < s->ln_len) {
        char *nl = memchr(s->ln + at, '\n', s->ln_len - at);
        size_t l, t;
        int act;
        if (nl == NULL) return;
        l = (size_t)(nl - (s->ln + at)) + 1;
        t = l - 1;
        if (t > 0 && s->ln[at + t - 1] == '\r') t--;
        memcpy(line, s->ln + at, t);
        line[t] = '\0';
        if (at > 0 && (act = ftp_line(&s->ftp, line, rep, sizeof rep)) != FTP_DEFER) {
            memmove(s->ln + at, s->ln + at + l, s->ln_len - at - l);
            s->ln_len -= l;
            s->n_cmd++;
            rp_put(s, rep, strlen(rep));
            ftp_act(s, act);
            return;
        }
        at += l;
    }
}

/* take complete command lines, one at a time, while nothing is in the way */
static void ftp_ctl(sess_t *s)
{
    char line[sizeof s->ln], rep[FTP_REPLY_MAX];

    while (s->kind == K_FTP && !s->closing && s->rp_off == s->rp_len) {
        char *nl = memchr(s->ln, '\n', s->ln_len);
        size_t l, t;
        int act;

        if (nl == NULL) {
            if (s->ln_len >= sizeof s->ln - 1) {
                rp_put(s, "500 Line too long.\r\n", 20);
                s->ln_len = 0;
            }
            break;
        }
        l = (size_t)(nl - s->ln) + 1;
        t = l - 1;
        if (t > 0 && s->ln[t - 1] == '\r') t--;
        memcpy(line, s->ln, t);
        line[t] = '\0';
        act = ftp_line(&s->ftp, line, rep, sizeof rep);
        if (act == FTP_DEFER) {              /* stays in the buffer until the fetch is over, */
            ftp_urgent(s);                   /* but an ABOR or QUIT behind it is taken now */
            break;
        }
        memmove(s->ln, s->ln + l, s->ln_len - l);
        s->ln_len -= l;
        s->n_cmd++;
        rp_put(s, rep, strlen(rep));
        ftp_act(s, act);
    }
}

/* the answer of the proxy is good: open the data connection */
static void data_open(sess_t *s)
{
    struct sockaddr_in sa;
    int fd;

    if (s->ftp.data == FTP_DATA_PASV) {
        if (s->dfd >= 0) s->x_state = X_STREAM;          /* the client is there already */
        else if (s->lfd < 0) xfer_end(s, FTP_R_NODATA);
        return;                                          /* else: wait for it to connect */
    }
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { xfer_end(s, FTP_R_NODATA); return; }
    small_buf(fd, SO_SNDBUF);
    fill_sa(&sa, s->ftp.port_ip, s->ftp.port_port);
    if (nonblock(fd) != 0 || (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0 && errno != EINPROGRESS)) {
        close(fd);
        xfer_end(s, FTP_R_NODATA);
        return;
    }
    s->dfd = fd;
    s->d_conn = 1;
    s->gen++;
}

/* the response head of the proxy is in s->in */
static void ftp_head(sess_t *s, size_t hl, int status)
{
    char rep[FTP_REPLY_MAX];
    size_t n;
    long v;

    switch (s->act) {
    case FTP_CWD:
        /* a directory answers 200 with its listing, anything else does not */
        p_close(s);
        s->x_state = X_IDLE;
        n = ftp_reply_cwd(&s->ftp, status == 200, rep, sizeof rep);
        rp_put(s, rep, n);
        break;
    case FTP_SIZE:
        v = (status == 200) ? http_header_long(s->in, hl, "Content-Length") : -1;
        p_close(s);
        s->x_state = X_IDLE;
        n = ftp_reply_size(&s->ftp, v, rep, sizeof rep);
        rp_put(s, rep, n);
        break;
    default:
        if (status != 200) { xfer_end(s, FTP_R_NOTFOUND); break; }
        if (s->act == FTP_RETR) {
            s->x_clen = http_header_long(s->in, hl, "Content-Length");
        } else {
            time_t t = time(NULL);
            struct tm tm;
            memset(&tm, 0, sizeof tm);
            gmtime_r(&t, &tm);
            lst_init(&s->lst, s->act == FTP_NLST, tm.tm_year + 1900, tm.tm_mon + 1);
        }
        s->in_off = hl;
        n = ftp_reply_open(&s->ftp, rep, sizeof rep);
        rp_put(s, rep, n);
        s->x_open = 1;
        s->x_state = X_DATAWAIT;
        data_open(s);
        break;
    }
}

/* move a listing through the converter, and notice the end of a transfer */
static void ftp_pump(sess_t *s)
{
    int listing = (s->act == FTP_LIST || s->act == FTP_NLST);

    if (s->x_state != X_STREAM) return;
    if (listing && s->dn_off == s->dn_len) {
        s->dn_off = s->dn_len = 0;
        if (s->in_off < s->in_len)
            s->in_off += lst_feed(&s->lst, s->in + s->in_off, s->in_len - s->in_off, s->dn, DN_MAX, &s->dn_len);
        if (s->dn_len == 0 && s->in_off == s->in_len && s->p_eof && !s->lst_done) {
            s->dn_len = lst_finish(&s->lst, s->dn, DN_MAX);
            s->lst_done = 1;
        }
    }
    if (s->p_eof && s->in_off == s->in_len && s->dn_off == s->dn_len) {
        int bad = s->x_err || (s->x_clen >= 0 && (unsigned long)s->x_clen != s->x_got);
        xfer_end(s, bad ? FTP_R_ABORTED : FTP_R_OK);
    }
}

static void ftp_event(sess_t *s, int role, int rev)
{
    struct sockaddr_in sa;
    socklen_t len;
    ssize_t n;
    long hl;
    int fd, st = 0;

    switch (role) {
    case R_C:
        if ((rev & POLLOUT) && s->rp_off < s->rp_len) {
            n = write(s->cfd, s->rp + s->rp_off, s->rp_len - s->rp_off);
            if (n < 0 && !again()) { sess_close(s, "client gone"); return; }
            if (n > 0) { s->rp_off += (size_t)n; touch(s); }
        }
        if (rev & POLLIN) {
            if (s->ln_len < sizeof s->ln - 1) {
                n = read(s->cfd, s->ln + s->ln_len, sizeof s->ln - 1 - s->ln_len);
                if (n == 0 || (n < 0 && !again())) { sess_close(s, "client closed"); return; }
                if (n > 0) { s->ln_len += (size_t)n; touch(s); }
            }
        } else if (rev & (POLLERR | POLLHUP)) {
            sess_close(s, "client gone");
            return;
        }
        break;

    case R_L:
        len = sizeof sa;
        fd = accept(s->lfd, (struct sockaddr *)&sa, &len);
        if (fd < 0) break;
        if (ntohl(sa.sin_addr.s_addr) != s->peer || nonblock(fd) != 0) {
            close(fd);           /* not our client: keep waiting */
            break;
        }
        small_buf(fd, SO_SNDBUF);
        l_close(s);
        s->dfd = fd;
        s->gen++;
        if (s->x_state == X_DATAWAIT) s->x_state = X_STREAM;
        touch(s);
        break;

    case R_P:
        if (s->x_state == X_CONNECT) {
            if (sock_err(s->pfd) != 0) {
                proxy_failed();
                xfer_end(s, FTP_R_NOPROXY);
            } else {
                s->x_state = X_SEND;
                touch(s);
            }
        } else if (s->x_state == X_SEND) {
            n = write(s->pfd, s->up + s->up_off, s->up_len - s->up_off);
            if (n < 0 && !again()) { xfer_end(s, FTP_R_NOPROXY); break; }
            if (n > 0) s->up_off += (size_t)n;
            if (s->up_off == s->up_len) s->x_state = X_HEAD;
        } else if (s->x_state == X_HEAD) {
            if (s->in_len < IN_MAX) {
                n = read(s->pfd, s->in + s->in_len, IN_MAX - s->in_len);
                if (n > 0) { s->in_len += (size_t)n; touch(s); }
                else if (n == 0 || !again()) s->p_eof = 1;
            }
            hl = http_resp_head(s->in, s->in_len, &st);
            if (hl < 0 || (hl == 0 && (s->p_eof || s->in_len >= IN_MAX))) xfer_end(s, FTP_R_NOPROXY);
            else if (hl > 0) ftp_head(s, (size_t)hl, st);
        } else if (s->x_state == X_STREAM && s->in_off == s->in_len && !s->p_eof) {
            s->in_off = s->in_len = 0;
            n = read(s->pfd, s->in, IN_MAX);
            if (n > 0) { s->in_len = (size_t)n; touch(s); }
            else if (n == 0) s->p_eof = 1;
            else if (!again()) { s->p_eof = 1; s->x_err = 1; }
        }
        break;

    case R_D:
        if (s->x_state == X_DATAWAIT && s->d_conn) {
            if (!(rev & (POLLOUT | POLLERR | POLLHUP))) break;
            if (sock_err(s->dfd) != 0) xfer_end(s, FTP_R_NODATA);
            else { s->d_conn = 0; s->x_state = X_STREAM; touch(s); }
            break;
        }
        if (rev & (POLLERR | POLLHUP)) {     /* the client dropped the data connection */
            if (s->x_state >= X_DATAWAIT) xfer_end(s, FTP_R_ABORTED);
            else d_close(s);
            break;
        }
        if (s->x_state == X_STREAM && (rev & POLLOUT)) {
            if (s->dn_off < s->dn_len) {
                n = write(s->dfd, s->dn + s->dn_off, s->dn_len - s->dn_off);
                if (n > 0) s->dn_off += (size_t)n;
            } else if (s->act == FTP_RETR && s->in_off < s->in_len) {
                n = write(s->dfd, s->in + s->in_off, s->in_len - s->in_off);
                if (n > 0) { s->in_off += (size_t)n; s->x_got += (unsigned long)n; }
            } else {
                break;
            }
            if (n < 0 && !again()) { xfer_end(s, FTP_R_ABORTED); break; }
            if (n > 0) { s->n_down += (unsigned long)n; touch(s); }
        }
        break;

    default:
        break;
    }

    ftp_pump(s);
    ftp_ctl(s);
    if (s->kind == K_FTP && s->closing && s->rp_off == s->rp_len) sess_close(s, "quit");
}

/* --- the loop ------------------------------------------------------------ */

static void add(int fd, int events, int role, sess_t *s)
{
    pfds[npfd].fd = fd;
    pfds[npfd].events = (short)events;
    pfds[npfd].revents = 0;
    pmeta[npfd].role = role;
    pmeta[npfd].s = s;
    pmeta[npfd].gen = s ? s->gen : 0;
    npfd++;
}

/* which fds of a session matter right now */
static void sess_poll(sess_t *s)
{
    int ev = 0;

    if (s->kind == K_HTTP) {
        int body = (!s->c_eof && (s->body_left == 0 || s->up_off == s->up_len || s->up_len < UP_MAX)) ? POLLIN : 0;
        switch (s->state) {
        case H_HEAD:
        case H_LINGER:
            add(s->cfd, POLLIN, R_C, s);
            break;
        case H_CONNECT:
            add(s->cfd, body, R_C, s);
            add(s->pfd, POLLOUT, R_P, s);
            break;
        default:
            add(s->cfd, body | ((s->in_off < s->in_len && !s->strip) ? POLLOUT : 0), R_C, s);
            if (s->pfd >= 0) {
                if (s->up_off < s->up_len) ev |= POLLOUT;
                if (!s->p_eof && (s->in_off == s->in_len || (s->strip && s->in_len < IN_MAX))) ev |= POLLIN;
                if (ev) add(s->pfd, ev, R_P, s);
            }
            break;
        }
        return;
    }
    if (!s->closing && s->ln_len < sizeof s->ln - 1) ev |= POLLIN;
    if (s->rp_off < s->rp_len) ev |= POLLOUT;
    add(s->cfd, ev, R_C, s);
    if (s->lfd >= 0) add(s->lfd, POLLIN, R_L, s);
    if (s->x_state == X_CONNECT || s->x_state == X_SEND) add(s->pfd, POLLOUT, R_P, s);
    else if (s->x_state == X_HEAD) add(s->pfd, POLLIN, R_P, s);
    else if (s->x_state == X_STREAM && !s->p_eof && s->in_off == s->in_len) add(s->pfd, POLLIN, R_P, s);
    if (s->dfd >= 0) {
        ev = 0;
        if (s->x_state == X_DATAWAIT && s->d_conn) ev = POLLOUT;
        else if (s->x_state == X_STREAM &&
                 (s->dn_off < s->dn_len || (s->act == FTP_RETR && s->in_off < s->in_len))) ev = POLLOUT;
        add(s->dfd, ev, R_D, s);
    }
}

static void sess_timeout(sess_t *s)
{
    if (s->kind == K_HTTP) {
        if (s->state == H_CONNECT) {
            proxy_failed();
            http_page(s, "502 Bad Gateway", page_502);
        } else if (s->state == H_RELAY && s->x_got == 0) {
            http_page(s, "502 Bad Gateway", page_502);       /* connected, but no answer came */
        } else {
            sess_close(s, s->state == H_LINGER ? "done" : "timeout");
        }
        return;
    }
    if (s->x_state != X_IDLE) {
        if (s->x_state == X_CONNECT) proxy_failed();
        xfer_end(s, s->x_state <= X_HEAD ? FTP_R_NOPROXY :
                    s->x_state == X_DATAWAIT ? FTP_R_NODATA : FTP_R_ABORTED);
        return;
    }
    if (write(s->cfd, "421 Timeout.\r\n", 14) < 0) { /* best effort */ }
    sess_close(s, "timeout");
}

static void on_accept(int lfd, int kind)
{
    static const char busy_http[] = "HTTP/1.0 503 Busy\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
                                    "Archive mode is busy, try again.\r\n";
    static const char busy_ftp[] = "421 Archive mode is busy, try again.\r\n";
    struct sockaddr_in sa;
    socklen_t len;
    sess_t *s;
    int fd, k, nb;

    for (k = 0; k < 8; k++) {
        len = sizeof sa;
        memset(&sa, 0, sizeof sa);
        fd = accept(lfd, (struct sockaddr *)&sa, &len);
        if (fd < 0) return;
        if (redirected_only) {
            /* the callers' port 80 and 21 are sent here by the kernel; a
             * connection made to this port itself is someone on the LAN
             * using the box as a relay */
            unsigned port;
            if (orig_dst(fd, &port) != 0 && port == (kind == K_HTTP ? http_port : ftp_port)) { close(fd); logv("refused: not redirected"); continue; }
        }
        nb = nonblock(fd) == 0;
        s = nb ? sess_new(kind, fd, &sa) : NULL;
        if (s == NULL) {
            const char *msg = (kind == K_HTTP) ? busy_http : busy_ftp;
            char waste[512];
            if (write(fd, msg, strlen(msg)) < 0) { /* best effort */ }
            /* input left unread makes the close a reset, which can overtake
             * the answer: take what has come (the socket is non-blocking) */
            while (nb && read(fd, waste, sizeof waste) > 0) ;
            close(fd);
            logv("%s refused: no session free for this caller", kind == K_HTTP ? "http" : "ftp");
            continue;
        }
        small_buf(fd, SO_SNDBUF);
        if (kind == K_HTTP) {
            s->state = H_HEAD;
        } else {
            char host[FTP_HOST_MAX], rep[FTP_REPLY_MAX];
            dns_host_for(&dns, "", s->orig, host, sizeof host);
            ftp_init(&s->ftp, host, s->peer);
            rp_put(s, rep, ftp_banner(&s->ftp, rep, sizeof rep));
        }
    }
}

static void on_dns(int fd)
{
    static uint8_t q[512], r[512 + 16];
    struct sockaddr_in sa;
    socklen_t len;
    ssize_t n;
    size_t rn;
    int k;

    for (k = 0; k < 16; k++) {
        len = sizeof sa;
        n = recvfrom(fd, q, sizeof q, 0, (struct sockaddr *)&sa, &len);
        if (n < 0) return;
        rn = dns_answer(&dns, q, (size_t)n, r, sizeof r);
        if (rn > 0 && sendto(fd, r, rn, 0, (struct sockaddr *)&sa, len) < 0) { /* dropped: the client asks again */ }
    }
}

static void on_signal(int sig)
{
    int e = errno;
    (void)sig;
    if (write(sig_pipe[1], "x", 1) < 0) { /* the pipe is full: a stop is pending anyway */ }
    errno = e;
}

static int parse_num(const char *s, unsigned lo, unsigned hi, unsigned *out)
{
    char *end;
    unsigned long v;

    if (*s < '0' || *s > '9') return -1;
    v = strtoul(s, &end, 10);
    if (*end != '\0' || v < lo || v > hi) return -1;
    *out = (unsigned)v;
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: v90arch [-x proxyhost:port] [-d dnsport] [-w httpport] [-f ftpport]\n"
        "               [-p pasvlo-pasvhi] [-n pool_base/prefix] [-b bind_address]\n"
        "               [-t idle_seconds] [-r] [-v]\n"
        "-r: only connections the kernel redirected here (the callers'), none made to the ports themselves\n"
        "defaults: -x wayback.protoweb.org:7851 -d 8053 -w 8080 -f 8021\n"
        "          -p 50000-50015 -n 198.18.0.0/15 -b 0.0.0.0 -t 120\n");
}

int main(int argc, char **argv)
{
    uint32_t pool_base = ((uint32_t)198 << 24) | ((uint32_t)18 << 16);
    unsigned pool_prefix = 15;
    struct sigaction sa;
    struct in_addr ia;
    int c, i, dfd, hfd, ffd, stop = 0;
    char *p;

    while ((c = getopt(argc, argv, "x:d:w:f:p:n:b:t:rv")) != -1) {
        int bad = 0;
        size_t n;
        switch (c) {
        case 'x':
            p = strrchr(optarg, ':');
            if (p == NULL || p == optarg || (size_t)(p - optarg) >= sizeof proxy_host ||
                parse_num(p + 1, 1, 65535, &proxy_port) != 0) { bad = 1; break; }
            memcpy(proxy_host, optarg, (size_t)(p - optarg));
            proxy_host[p - optarg] = '\0';
            break;
        case 'd': bad = parse_num(optarg, 1, 65535, &dns_port); break;
        case 'w': bad = parse_num(optarg, 1, 65535, &http_port); break;
        case 'f': bad = parse_num(optarg, 1, 65535, &ftp_port); break;
        case 'p':
            p = strchr(optarg, '-');
            if (p == NULL) { bad = 1; break; }
            *p = '\0';
            bad = parse_num(optarg, 1, 65535, &pasv_lo) || parse_num(p + 1, 1, 65535, &pasv_hi) || pasv_lo > pasv_hi;
            break;
        case 'n':
            n = dns_parse_quad(optarg, &pool_base);
            bad = (n == 0 || optarg[n] != '/' || parse_num(optarg + n + 1, 8, 22, &pool_prefix) != 0);
            break;
        case 'b':
            n = dns_parse_quad(optarg, &bind_ip);
            bad = (n == 0 || optarg[n] != '\0');
            break;
        case 't': bad = parse_num(optarg, 1, 86400, &idle_s); break;
        case 'r': redirected_only = 1; break;
        case 'v': verbose = 1; break;
        default: bad = 1; break;
        }
        if (bad) { usage(); return 2; }
    }
    if (optind != argc) { usage(); return 2; }
    if (dns_init(&dns, pool_base, (int)pool_prefix) != 0) { usage(); return 2; }
    if (connect_s > idle_s) connect_s = idle_s;
    if (linger_s > idle_s) linger_s = idle_s;

    for (i = 0; i < MAX_SESS; i++) sess[i].cfd = sess[i].pfd = sess[i].dfd = sess[i].lfd = -1;

    if (pipe(sig_pipe) != 0 || nonblock(sig_pipe[0]) != 0 || nonblock(sig_pipe[1]) != 0) {
        perror("v90arch: pipe");
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    dfd = listen_on(SOCK_DGRAM, bind_ip, dns_port);
    hfd = listen_on(SOCK_STREAM, bind_ip, http_port);
    ffd = listen_on(SOCK_STREAM, bind_ip, ftp_port);
    if (dfd < 0 || hfd < 0 || ffd < 0) {
        fprintf(stderr, "v90arch: cannot listen on %s port %u: %s\n",
                dfd < 0 ? "UDP" : "TCP", dfd < 0 ? dns_port : hfd < 0 ? http_port : ftp_port, strerror(errno));
        return 1;
    }

    /* the proxy: a numeric address is used as it is, a name is looked up
     * from now on (a child, proxy_resolve) and, when that fails or a connect
     * fails, again later */
    fill_sa(&proxy_sa, 0, proxy_port);
    if (inet_pton(AF_INET, proxy_host, &ia) == 1) {
        proxy_sa.sin_addr = ia;
        proxy_ok = proxy_numeric = 1;
    } else {
        proxy_resolve();
    }
    logv("v90arch: proxy %s:%u%s, dns %u, http %u, ftp %u, pasv %u-%u", proxy_host, proxy_port,
         proxy_ok ? "" : " (being looked up)", dns_port, http_port, ftp_port, pasv_lo, pasv_hi);

    while (!stop) {
        int busy = 0, r;
        long now;

        npfd = 0;
        add(sig_pipe[0], POLLIN, R_SIG, NULL);
        add(dfd, POLLIN, R_DNS, NULL);
        add(hfd, POLLIN, R_LHTTP, NULL);
        add(ffd, POLLIN, R_LFTP, NULL);
        add(res_fd, POLLIN, R_RES, NULL);    /* -1 when no lookup runs: poll passes over it */
        for (i = 0; i < MAX_SESS; i++) {
            if (sess[i].kind == K_FREE) continue;
            busy = 1;
            sess_poll(&sess[i]);
        }
        /* with no session there is nothing to time out: sleep until asked */
        r = poll(pfds, (nfds_t)npfd, busy || res_fd >= 0 ? 1000 : -1);
        if (r < 0 && errno != EINTR) { perror("v90arch: poll"); break; }
        if (r > 0) {
            /* sessions first, new connections last, so that a slot freed in
             * this round is not handed out while its old events are pending;
             * an entry whose session changed its fds meanwhile is skipped
             * (poll reports it again next round) */
            for (i = 5; i < npfd; i++) {
                sess_t *s = pmeta[i].s;
                if (pfds[i].revents == 0 || s->kind == K_FREE || s->gen != pmeta[i].gen) continue;
                if (s->kind == K_HTTP) http_event(s, pmeta[i].role, pfds[i].revents);
                else ftp_event(s, pmeta[i].role, pfds[i].revents);
            }
            if (pfds[0].revents) stop = 1;
            if (pfds[1].revents) on_dns(dfd);
            if (pfds[2].revents) on_accept(hfd, K_HTTP);
            if (pfds[3].revents) on_accept(ffd, K_FTP);
            if (pfds[4].revents && res_fd >= 0) proxy_answer();
        }
        now = now_s();
        for (i = 0; i < MAX_SESS; i++)
            if (sess[i].kind != K_FREE && now >= sess[i].deadline) sess_timeout(&sess[i]);
    }

    for (i = 0; i < MAX_SESS; i++)
        if (sess[i].kind != K_FREE) sess_close(&sess[i], "shutdown");
    close(dfd); close(hfd); close(ffd);
    if (res_fd >= 0) { (void)kill(res_pid, SIGKILL); close(res_fd); (void)waitpid(res_pid, NULL, 0); }
    logv("v90arch: stopped");
    return 0;
}
