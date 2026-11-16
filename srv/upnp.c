/* See upnp.h. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "upnp.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* "http://host[:port][/path]" at s[0..n) */
static int url_parse(const char *s, size_t n, upnp_url *u)
{
    size_t i = 7, h = 0, p = 0; unsigned long port = 80;
    if (n < 8 || lower(s[0]) != 'h' || lower(s[1]) != 't' || lower(s[2]) != 't' || lower(s[3]) != 'p' ||
        s[4] != ':' || s[5] != '/' || s[6] != '/')
        return -1;
    while (i < n && s[i] != ':' && s[i] != '/') {
        if (s[i] <= ' ' || (unsigned char)s[i] > 0x7e || h + 1 >= sizeof u->host) return -1;
        u->host[h++] = s[i++];
    }
    if (!h) return -1;
    u->host[h] = 0;
    if (i < n && s[i] == ':') {
        size_t d = 0;
        port = 0; i++;
        while (i < n && s[i] >= '0' && s[i] <= '9' && d < 5) { port = port * 10 + (unsigned long)(s[i++] - '0'); d++; }
        if (!d || port < 1 || port > 65535 || (i < n && s[i] != '/')) return -1;
    }
    u->port = (uint16_t)port;
    while (i < n) {
        if (s[i] <= ' ' || (unsigned char)s[i] > 0x7e || p + 1 >= sizeof u->path) return -1;
        u->path[p++] = s[i++];
    }
    if (!p) u->path[p++] = '/';
    u->path[p] = 0;
    return 0;
}

int upnp_location(const char *resp, size_t len, upnp_url *u)
{
    static const char key[] = "location:";
    size_t at = 0;
    while (at < len) {
        size_t end = at, k;
        while (end < len && resp[end] != '\n') end++;
        for (k = 0; k < 9 && at + k < end && lower(resp[at + k]) == key[k]; k++) ;
        if (k == 9) {
            size_t a = at + 9, b = end;
            while (a < b && (resp[a] == ' ' || resp[a] == '\t')) a++;
            while (b > a && (resp[b - 1] == '\r' || resp[b - 1] == ' ' || resp[b - 1] == '\t')) b--;
            return url_parse(resp + a, b - a, u);
        }
        at = end + 1;
    }
    return -1;
}

int upnp_trust(uint32_t local, uint32_t from, unsigned from_port, const upnp_url *u)
{
    struct in_addr a; uint32_t h; int loop = (local >> 24) == 127;
    if (from_port != UPNP_PORT || inet_pton(AF_INET, u->host, &a) != 1) return 0;
    h = ntohl(a.s_addr);
    if (h != from || h == 0 || (h >> 28) >= 14) return 0;           /* multicast and above */
    if ((h >> 16) == 0xA9FE) return 0;                              /* link-local */
    if (loop) return (h >> 24) == 127;
    return (h >> 24) != 127 && h != local;
}

/* the first place at or after `from` where text t stands, or len */
static size_t find(const char *x, size_t len, size_t from, const char *t)
{
    size_t n = strlen(t), i;
    if (n > len) return len;
    for (i = from; i + n <= len; i++) if (memcmp(x + i, t, n) == 0) return i;
    return len;
}

/* what stands between <tag> and </tag>, first one at or after `from` and
 * before `to`, without white space around it; 0 when there is none */
static int element(const char *x, size_t from, size_t to, const char *open, const char *close, const char **v, size_t *vn)
{
    size_t a = find(x, to, from, open), b;
    if (a >= to) return 0;
    a += strlen(open);
    b = find(x, to, a, close);
    if (b >= to) return 0;
    while (a < b && (unsigned char)x[a] <= ' ') a++;
    while (b > a && (unsigned char)x[b - 1] <= ' ') b--;
    *v = x + a; *vn = b - a;
    return 1;
}

int upnp_control(const char *xml, size_t len, upnp_ctl *c)
{
    static const char *const want[2] = { ":service:WANIPConnection:", ":service:WANPPPConnection:" };
    int w;
    for (w = 0; w < 2; w++) {
        size_t at = 0;
        for (;;) {
            size_t s = find(xml, len, at, "<service>"), e;
            const char *t, *u; size_t tn, un;
            if (s >= len) break;
            e = find(xml, len, s, "</service>");
            at = e;
            if (!element(xml, s, e, "<serviceType>", "</serviceType>", &t, &tn)) continue;
            if (tn >= sizeof c->type || find(t, tn, 0, want[w]) >= tn) continue;
            if (!element(xml, s, e, "<controlURL>", "</controlURL>", &u, &un) || !un) continue;
            memcpy(c->type, t, tn); c->type[tn] = 0;
            if (un > 7 && lower(u[0]) == 'h' && u[4] == ':') {       /* a whole URL: its path */
                upnp_url tmp;
                if (url_parse(u, un, &tmp) < 0) continue;
                strcpy(c->path, tmp.path);
            } else {
                size_t lead = u[0] == '/' ? 0 : 1, i;
                if (un + lead >= sizeof c->path) continue;
                for (i = 0; i < un; i++) if (u[i] <= ' ' || (unsigned char)u[i] > 0x7e) break;
                if (i < un) continue;
                c->path[0] = '/';
                memcpy(c->path + lead, u, un); c->path[un + lead] = 0;
            }
            return 0;
        }
    }
    return -1;
}

int upnp_request(char *out, size_t cap, const upnp_url *dev, const upnp_ctl *c,
                 int add, int proto, unsigned port, uint32_t ip, const char *desc)
{
    char body[700]; int bn, n;
    const char *act = add ? "AddPortMapping" : "DeletePortMapping";
    const char *pr = proto == 17 ? "UDP" : "TCP";
    if (add)
        bn = snprintf(body, sizeof body,
            "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
            " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">"
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>%s</NewProtocol>"
            "<NewInternalPort>%u</NewInternalPort><NewInternalClient>%u.%u.%u.%u</NewInternalClient>"
            "<NewEnabled>1</NewEnabled><NewPortMappingDescription>%s</NewPortMappingDescription>"
            "<NewLeaseDuration>0</NewLeaseDuration></u:%s></s:Body></s:Envelope>\r\n",
            act, c->type, port, pr, port, (unsigned)(ip >> 24), (unsigned)((ip >> 16) & 255), (unsigned)((ip >> 8) & 255),
            (unsigned)(ip & 255), desc ? desc : "", act);
    else
        bn = snprintf(body, sizeof body,
            "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\""
            " s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">"
            "<NewRemoteHost></NewRemoteHost><NewExternalPort>%u</NewExternalPort><NewProtocol>%s</NewProtocol>"
            "</u:%s></s:Body></s:Envelope>\r\n",
            act, c->type, port, pr, act);
    if (bn < 0 || (size_t)bn >= sizeof body) return -1;
    n = snprintf(out, cap,
        "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: text/xml; charset=\"utf-8\"\r\n"
        "SOAPAction: \"%s#%s\"\r\nConnection: close\r\nContent-Length: %d\r\n\r\n%s",
        c->path, dev->host, (unsigned)dev->port, c->type, act, bn, body);
    if (n < 0 || (size_t)n >= cap) { if (cap) out[0] = 0; return -1; }
    return n;
}

int upnp_ok(const char *resp, size_t len)
{
    return len >= 12 && memcmp(resp, "HTTP/1.", 7) == 0 && resp[8] == ' ' && memcmp(resp + 9, "200", 3) == 0;
}

/* ---- the conversation ---- */

/* one request to the device, the answer into resp (0-terminated); its length or -1 */
static int http_talk(const upnp_url *dev, const char *req, size_t reqlen, char *resp, size_t cap)
{
    struct sockaddr_in sa; struct timeval tv; int fd; size_t got = 0; ssize_t r;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_port = htons(dev->port);
    if (inet_pton(AF_INET, dev->host, &sa.sin_addr) != 1) return -1;       /* routers name themselves by address */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    tv.tv_sec = 4; tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)(void *)&sa, sizeof sa) < 0) { close(fd); return -1; }
    while (reqlen) {
        r = send(fd, req, reqlen, MSG_NOSIGNAL);
        if (r <= 0) { close(fd); return -1; }
        req += r; reqlen -= (size_t)r;
    }
    while (got + 1 < cap && (r = recv(fd, resp + got, cap - 1 - got, 0)) > 0) got += (size_t)r;
    close(fd);
    resp[got] = 0;
    return (int)got;
}

int upnp_run(uint32_t local_ip, int add, uint32_t ip, const uint8_t *proto, const uint16_t *lo, const uint16_t *hi, int n,
             const char *desc)
{
    static char buf[16384], req[1500];
    struct sockaddr_in sa; struct in_addr ifa; struct pollfd pf;
    upnp_url dev; upnp_ctl ctl; int fd, got = 0, tries, i, len, okc = 0; unsigned port;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(local_ip);
    (void)bind(fd, (struct sockaddr *)(void *)&sa, sizeof sa);
    ifa.s_addr = htonl(local_ip);
    (void)setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &ifa, sizeof ifa);
    sa.sin_port = htons(UPNP_PORT); sa.sin_addr.s_addr = htonl(UPNP_GROUP);
    for (tries = 0; tries < 2 && !got; tries++) {
        (void)sendto(fd, UPNP_SEARCH, sizeof UPNP_SEARCH - 1, 0, (struct sockaddr *)(void *)&sa, sizeof sa);
        for (;;) {
            ssize_t r;
            pf.fd = fd; pf.events = POLLIN;
            if (poll(&pf, 1, 2000) <= 0) break;
            struct sockaddr_in from; socklen_t fl = sizeof from;
            r = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)(void *)&from, &fl);
            if (r > 0 && upnp_ok(buf, (size_t)r) && upnp_location(buf, (size_t)r, &dev) == 0 &&
                upnp_trust(local_ip, ntohl(from.sin_addr.s_addr), ntohs(from.sin_port), &dev)) { got = 1; break; }
        }
    }
    close(fd);
    if (!got) return -1;

    len = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%u\r\nConnection: close\r\n\r\n", dev.path, dev.host, (unsigned)dev.port);
    len = http_talk(&dev, req, (size_t)len, buf, sizeof buf);
    if (len <= 0 || !upnp_ok(buf, (size_t)len) || upnp_control(buf, (size_t)len, &ctl) < 0) return -2;

    for (i = 0; i < n; i++)
        for (port = lo[i]; port <= hi[i]; port++) {
            if (add == 2) {
                len = upnp_request(req, sizeof req, &dev, &ctl, 0, proto[i], port, 0, NULL);
                if (len > 0) (void)http_talk(&dev, req, (size_t)len, buf, sizeof buf);
            }
            len = upnp_request(req, sizeof req, &dev, &ctl, add, proto[i], port, ip, desc);
            if (len < 0) continue;
            len = http_talk(&dev, req, (size_t)len, buf, sizeof buf);
            if (len > 0 && upnp_ok(buf, (size_t)len)) okc++;
        }
    return okc;
}
