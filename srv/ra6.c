/* What the network's IPv6 router says (ra6.h). Ours; written for this project. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/icmp6.h>
#include <sys/socket.h>
#include "ra6.h"

/* ------------------------------------------------------------- parsing */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void ra6_expire(ra6_info *in, uint32_t now)
{
    int i, k = 0;
    for (i = 0; i < in->n_dns; i++) {
        if ((int32_t)(in->dns_until[i] - now) <= 0) continue;
        if (k != i) { memcpy(in->dns[k], in->dns[i], 16); in->dns_until[k] = in->dns_until[i]; }
        k++;
    }
    in->n_dns = k;
    if (in->pref64_len && (int32_t)(in->pref64_until - now) <= 0) in->pref64_len = 0;
}

int ra6_parse(const uint8_t *m, size_t n, uint32_t now, ra6_info *in)
{
    ra6_info t = *in;
    size_t i = 16;
    int dns_seen = 0;
    if (n < 16 || m[0] != 134 || m[1] != 0) return -1;
    while (i + 2 <= n) {
        size_t len = (size_t)m[i + 1] * 8;
        const uint8_t *o = m + i;
        if (len == 0 || i + len > n) return -1;     /* RFC 4861 4.6: a zero length is invalid */
        if (o[0] == 25 && len >= 24) {              /* RDNSS: lifetime, then addresses */
            uint32_t life = be32(o + 4);
            size_t k, na = (len - 8) / 16;
            if (!dns_seen) { t.n_dns = 0; dns_seen = 1; }
            for (k = 0; k < na && life; k++) {
                const uint8_t *a = o + 8 + 16 * k;
                int j;
                for (j = 0; j < t.n_dns && memcmp(t.dns[j], a, 16); j++) ;
                if (j == t.n_dns) { if (t.n_dns == RA6_DNS_MAX) break; t.n_dns++; }
                memcpy(t.dns[j], a, 16);
                t.dns_until[j] = now + (life > 0x7fffffffu ? 0x7fffffffu : life);
            }
        } else if (o[0] == 38 && len == 16) {       /* PREF64: scaled lifetime (13 bits) and PLC (3), 96 bits */
            unsigned sl = (unsigned)(o[2] << 8 | o[3]), plc = sl & 7;
            static const int bits[6] = { 96, 64, 56, 48, 40, 32 };
            if (plc < 6) {
                memset(t.pref64, 0, 16);
                memcpy(t.pref64, o + 4, 12);
                t.pref64_len = (sl >> 3) ? bits[plc] : 0;    /* a lifetime of 0 withdraws it */
                t.pref64_until = now + (sl >> 3) * 8u;
                if (t.pref64_len < 96) {                     /* the bits past the length are not the prefix */
                    int b;
                    for (b = t.pref64_len; b < 96; b++) t.pref64[b / 8] &= (uint8_t)~(0x80u >> (b % 8));
                }
            }
        }
        i += len;
    }
    t.heard = now ? now : 1;
    *in = t;
    return 0;
}

/* ------------------------------------------------------------ the daemon */
#define SOLICIT_MS  60000       /* a solicitation this often while no router has spoken */
#define RESOLV_MS   10000       /* the box's resolv.conf looked at this often */

static struct {
    int on, fd;
    char ifname[IF_NAMESIZE];
    unsigned ifindex;
    ra6_info in;
    uint32_t next_solicit, next_resolv;
    int inited;
} R;

const ra6_info *ra6_now(void) { return &R.in; }

void ra6_want(const char *ifname, int on)
{
    if (!R.inited) { R.inited = 1; R.fd = -1; }
    if (R.fd >= 0 && (!on || !ifname || strcmp(ifname, R.ifname) != 0)) { close(R.fd); R.fd = -1; }
    R.on = on && ifname && *ifname;
    if (R.on) snprintf(R.ifname, sizeof R.ifname, "%s", ifname);
    if (!R.on) memset(&R.in, 0, sizeof R.in);
}

static void open_socket(void)
{
    struct icmp6_filter f;
    int one = 1, hops = 255;
    R.ifindex = if_nametoindex(R.ifname);
    if (!R.ifindex) return;
    R.fd = socket(AF_INET6, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMPV6);
    if (R.fd < 0) return;                       /* no IPv6 in this kernel */
    ICMP6_FILTER_SETBLOCKALL(&f);
    ICMP6_FILTER_SETPASS(ND_ROUTER_ADVERT, &f);
    (void)setsockopt(R.fd, IPPROTO_ICMPV6, ICMP6_FILTER, &f, sizeof f);
    (void)setsockopt(R.fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &one, sizeof one);
    (void)setsockopt(R.fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &one, sizeof one);
    (void)setsockopt(R.fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof hops);
    (void)setsockopt(R.fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &R.ifindex, sizeof R.ifindex);
}

static void solicit(void)
{
    uint8_t rs[8] = { 133, 0, 0, 0, 0, 0, 0, 0 };   /* the kernel fills in the checksum */
    struct sockaddr_in6 to;
    memset(&to, 0, sizeof to);
    to.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "ff02::2", &to.sin6_addr);  /* all routers */
    to.sin6_scope_id = R.ifindex;
    (void)sendto(R.fd, rs, sizeof rs, 0, (struct sockaddr *)(void *)&to, sizeof to);
}

static void hear(uint32_t now_s)
{
    int n;
    for (n = 0; n < 8; n++) {
        uint8_t m[1500], ctl[128];
        struct sockaddr_in6 from;
        struct iovec iov = { m, sizeof m };
        struct msghdr h;
        struct cmsghdr *c;
        int hop = -1; unsigned idx = 0;
        ssize_t r;
        memset(&h, 0, sizeof h);
        h.msg_name = &from; h.msg_namelen = sizeof from;
        h.msg_iov = &iov; h.msg_iovlen = 1;
        h.msg_control = ctl; h.msg_controllen = sizeof ctl;
        r = recvmsg(R.fd, &h, 0);
        if (r <= 0) return;
        for (c = CMSG_FIRSTHDR(&h); c; c = CMSG_NXTHDR(&h, c)) {
            if (c->cmsg_level != IPPROTO_IPV6) continue;
            if (c->cmsg_type == IPV6_HOPLIMIT) memcpy(&hop, CMSG_DATA(c), sizeof hop);
            if (c->cmsg_type == IPV6_PKTINFO) { struct in6_pktinfo pi; memcpy(&pi, CMSG_DATA(c), sizeof pi); idx = pi.ipi6_ifindex; }
        }
        /* RFC 4861 6.1.2: from a link-local router, never forwarded, and
         * here: on the LAN port only */
        if (hop != 255 || idx != R.ifindex || !IN6_IS_ADDR_LINKLOCAL(&from.sin6_addr)) continue;
        (void)ra6_parse(m, (size_t)r, now_s, &R.in);
    }
}

/* the box's resolv.conf: what DHCP (or the start) wrote, then ours */
static void resolv(void)
{
    char path[256], buf[4096], want[512], *mark, *ours;
    FILE *f;
    size_t n = 0, wn = 0;
    int i, theirs = 0;
    const char *p = getenv("V90D_RESOLV") ? getenv("V90D_RESOLV") : "/etc/resolv.conf", *q;
    ssize_t l = readlink(p, path, sizeof path - 1);
    if (l > 0) { path[l] = 0; p = path; }       /* the file it links to: written in place */
    f = fopen(p, "r");
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = 0;
    mark = strstr(buf, RA6_MARK);
    ours = mark ? mark : buf + n;               /* buf[..ours) is DHCP's, the rest our last */
    for (q = buf; (q = strstr(q, "nameserver")) != NULL && q < ours; q += 10) theirs++;
    want[0] = 0;
    if (R.in.n_dns && theirs < 3) {             /* the resolver reads three: ours after theirs */
        wn = (size_t)snprintf(want, sizeof want, "%s\n", RA6_MARK);
        for (i = 0; i < R.in.n_dns && theirs + i < 3; i++) {
            char a[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, R.in.dns[i], a, sizeof a);
            wn += (size_t)snprintf(want + wn, sizeof want - wn, "nameserver %s\n", a);
            if (wn >= sizeof want) return;
        }
    }
    if (strcmp(ours, want) == 0) return;        /* as it should be: left alone */
    *ours = 0;
    f = fopen(p, "w");
    if (!f) return;
    fputs(buf, f);
    if (wn && buf[0] && buf[strlen(buf) - 1] != '\n') fputc('\n', f);
    fputs(want, f);
    fclose(f);
}

void ra6_poll(uint32_t now_ms)
{
    uint32_t now_s = now_ms / 1000;
    if (!R.on) return;
    if (R.fd < 0) {
        if ((int32_t)(now_ms - R.next_solicit) < 0) return;
        open_socket();
        if (R.fd < 0) { R.next_solicit = now_ms + SOLICIT_MS; return; }
        R.next_solicit = now_ms;
    }
    hear(now_s);
    ra6_expire(&R.in, now_s);
    if (!R.in.heard && (int32_t)(now_ms - R.next_solicit) >= 0) { solicit(); R.next_solicit = now_ms + SOLICIT_MS; }
    if ((int32_t)(now_ms - R.next_resolv) >= 0) { resolv(); R.next_resolv = now_ms + RESOLV_MS; }
}
