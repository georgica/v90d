/* See nat.h. The messages are built by hand from the kernel's own interface
 * headers; the layout is netlink (16-byte header, 4-byte nfgenmsg, then
 * type-length-value attributes aligned to 4). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include <stdio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sched.h>
#include "nat.h"

#define TABLE "v90d"
#define CHAIN "postrouting"
#define CHAIN_PRE "prerouting"

typedef struct { uint8_t b[65536]; size_t n; int bad; uint32_t seq; } nlbuf;

static size_t al(size_t n) { return (n + 3u) & ~(size_t)3u; }

static void *grow(nlbuf *m, size_t n)
{
    void *p = m->b + m->n;
    if (m->n + al(n) > sizeof m->b) { m->bad = 1; return m->b; }    /* scribble at the start, report later */
    memset(p, 0, al(n));
    m->n += al(n);
    return p;
}

/* start a message; returns its offset for msg_end() */
static size_t msg_begin(nlbuf *m, uint16_t type, uint16_t flags, uint8_t family, uint16_t res_id)
{
    size_t at = m->n;
    struct nlmsghdr *h = grow(m, sizeof *h);
    struct nfgenmsg *g;
    h->nlmsg_type = type; h->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags); h->nlmsg_seq = ++m->seq;
    g = grow(m, sizeof *g);
    g->nfgen_family = family; g->version = NFNETLINK_V0; g->res_id = htons(res_id);
    return at;
}
static void msg_end(nlbuf *m, size_t at)
{
    struct nlmsghdr *h = (struct nlmsghdr *)(void *)(m->b + at);
    if (!m->bad) h->nlmsg_len = (uint32_t)(m->n - at);
}
static void attr(nlbuf *m, uint16_t type, const void *data, size_t len)
{
    struct nlattr *a = grow(m, sizeof *a);
    a->nla_type = type; a->nla_len = (uint16_t)(sizeof *a + len);
    if (len) memcpy(grow(m, len), data, len);
}
static void attr_str(nlbuf *m, uint16_t type, const char *s) { attr(m, type, s, strlen(s) + 1); }
static void attr_u32(nlbuf *m, uint16_t type, uint32_t v) { uint32_t be = htonl(v); attr(m, type, &be, 4); }
static size_t nest_begin(nlbuf *m, uint16_t type)
{
    size_t at = m->n;
    struct nlattr *a = grow(m, sizeof *a);
    a->nla_type = (uint16_t)(type | NLA_F_NESTED);
    return at;
}
static void nest_end(nlbuf *m, size_t at)
{
    struct nlattr *a = (struct nlattr *)(void *)(m->b + at);
    if (!m->bad) a->nla_len = (uint16_t)(m->n - at);
}

static uint16_t nft(int msg) { return (uint16_t)((NFNL_SUBSYS_NFTABLES << 8) | msg); }

static void table_msg(nlbuf *m, int msg, uint16_t flags)
{
    size_t at = msg_begin(m, nft(msg), flags, NFPROTO_IPV4, 0);
    attr_str(m, NFTA_TABLE_NAME, TABLE);
    msg_end(m, at);
}

/* one expression of a rule: name, then its attributes inside NFTA_EXPR_DATA */
static size_t expr_begin(nlbuf *m, const char *name, size_t *data)
{
    size_t el = nest_begin(m, NFTA_LIST_ELEM);
    attr_str(m, NFTA_EXPR_NAME, name);
    *data = nest_begin(m, NFTA_EXPR_DATA);
    return el;
}
static void expr_end(nlbuf *m, size_t el, size_t data) { nest_end(m, data); nest_end(m, el); }

/* Send the batch and collect the kernel's verdict on every message. */
static int transact(nlbuf *m, uint32_t first_seq)
{
    struct sockaddr_nl sa;
    uint8_t rb[4096];
    int fd, err = 0, pending;
    uint32_t last = m->seq;
    if (m->bad) { errno = ENOBUFS; return -1; }
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
    if (fd < 0) return -1;
    memset(&sa, 0, sizeof sa); sa.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 ||
        sendto(fd, m->b, m->n, 0, (struct sockaddr *)&sa, sizeof sa) < 0) {
        err = errno; close(fd); errno = err; return -1;
    }
    /* every message between the batch markers asked for an acknowledgement
     * (first_seq .. last - 1); a failing one may be the only answer */
    pending = (int)(last - first_seq);
    while (pending > 0) {
        struct timeval tv = { 2, 0 };
        ssize_t n; size_t off = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        n = recv(fd, rb, sizeof rb, 0);
        if (n <= 0) { if (!err) err = n < 0 ? errno : EIO; break; }
        while (off + sizeof(struct nlmsghdr) <= (size_t)n) {
            struct nlmsghdr *h = (struct nlmsghdr *)(void *)(rb + off);
            if (h->nlmsg_len < sizeof *h || off + h->nlmsg_len > (size_t)n) break;
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)(void *)(rb + off + sizeof *h);
                if (e->error && !err) err = -e->error;
                if (e->error) pending = 0; else pending--;
            }
            off += al(h->nlmsg_len);
        }
    }
    close(fd);
    if (err) { errno = err; return -1; }
    return 0;
}

/* ip saddr <peer> masquerade */
static void rule_msg(nlbuf *mp, uint32_t peer_ip)
{
    size_t at, exprs, el, data, val;
    uint32_t be = htonl(peer_ip);
    at = msg_begin(mp, nft(NFT_MSG_NEWRULE), NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_IPV4, 0);
    attr_str(mp, NFTA_RULE_TABLE, TABLE);
    attr_str(mp, NFTA_RULE_CHAIN, CHAIN);
    exprs = nest_begin(mp, NFTA_RULE_EXPRESSIONS);
    el = expr_begin(mp, "payload", &data);              /* source address into register 1 */
    attr_u32(mp, NFTA_PAYLOAD_DREG, NFT_REG_1);
    attr_u32(mp, NFTA_PAYLOAD_BASE, NFT_PAYLOAD_NETWORK_HEADER);
    attr_u32(mp, NFTA_PAYLOAD_OFFSET, 12);
    attr_u32(mp, NFTA_PAYLOAD_LEN, 4);
    expr_end(mp, el, data);
    el = expr_begin(mp, "cmp", &data);                  /* equal to the peer's */
    attr_u32(mp, NFTA_CMP_SREG, NFT_REG_1);
    attr_u32(mp, NFTA_CMP_OP, NFT_CMP_EQ);
    val = nest_begin(mp, NFTA_CMP_DATA);
    attr(mp, NFTA_DATA_VALUE, &be, 4);
    nest_end(mp, val);
    expr_end(mp, el, data);
    el = expr_begin(mp, "masq", &data);
    expr_end(mp, el, data);
    nest_end(mp, exprs);
    msg_end(mp, at);
}

/* oifname <ifn> masquerade */
static void masq_if_msg(nlbuf *mp, const char *ifn)
{
    size_t at, exprs, el, data, val, k; char name[IFNAMSIZ];
    memset(name, 0, sizeof name);
    for (k = 0; k + 1 < sizeof name && ifn[k]; k++) name[k] = ifn[k];
    at = msg_begin(mp, nft(NFT_MSG_NEWRULE), NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_IPV4, 0);
    attr_str(mp, NFTA_RULE_TABLE, TABLE);
    attr_str(mp, NFTA_RULE_CHAIN, CHAIN);
    exprs = nest_begin(mp, NFTA_RULE_EXPRESSIONS);
    el = expr_begin(mp, "meta", &data);                 /* the name of the interface it leaves by */
    attr_u32(mp, NFTA_META_KEY, NFT_META_OIFNAME);
    attr_u32(mp, NFTA_META_DREG, NFT_REG_1);
    expr_end(mp, el, data);
    el = expr_begin(mp, "cmp", &data);
    attr_u32(mp, NFTA_CMP_SREG, NFT_REG_1);
    attr_u32(mp, NFTA_CMP_OP, NFT_CMP_EQ);
    val = nest_begin(mp, NFTA_CMP_DATA);
    attr(mp, NFTA_DATA_VALUE, name, sizeof name);
    nest_end(mp, val);
    expr_end(mp, el, data);
    el = expr_begin(mp, "masq", &data);
    expr_end(mp, el, data);
    nest_end(mp, exprs);
    msg_end(mp, at);
}

/* register 1 = len octets of the packet at off in the given header; compare with v */
static void match(nlbuf *mp, uint32_t base, uint32_t off, const void *v, uint32_t len, uint32_t op)
{
    size_t el, data, val;
    el = expr_begin(mp, "payload", &data);
    attr_u32(mp, NFTA_PAYLOAD_DREG, NFT_REG_1);
    attr_u32(mp, NFTA_PAYLOAD_BASE, base);
    attr_u32(mp, NFTA_PAYLOAD_OFFSET, off);
    attr_u32(mp, NFTA_PAYLOAD_LEN, len);
    expr_end(mp, el, data);
    el = expr_begin(mp, "cmp", &data);
    attr_u32(mp, NFTA_CMP_SREG, NFT_REG_1);
    attr_u32(mp, NFTA_CMP_OP, op);
    val = nest_begin(mp, NFTA_CMP_DATA);
    attr(mp, NFTA_DATA_VALUE, v, len);
    nest_end(mp, val);
    expr_end(mp, el, data);
}

/* ip saddr <peer> <proto> dport lo-hi redirect [to :to] */
static void redirect_msg(nlbuf *mp, uint32_t peer_ip, uint8_t proto, uint16_t lo, uint16_t hi, uint16_t to)
{
    size_t at, exprs, el, data, val;
    uint32_t be = htonl(peer_ip);
    uint16_t blo = htons(lo), bhi = htons(hi), bto = htons(to);
    at = msg_begin(mp, nft(NFT_MSG_NEWRULE), NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_IPV4, 0);
    attr_str(mp, NFTA_RULE_TABLE, TABLE);
    attr_str(mp, NFTA_RULE_CHAIN, CHAIN_PRE);
    exprs = nest_begin(mp, NFTA_RULE_EXPRESSIONS);
    match(mp, NFT_PAYLOAD_NETWORK_HEADER, 12, &be, 4, NFT_CMP_EQ);
    match(mp, NFT_PAYLOAD_NETWORK_HEADER, 9, &proto, 1, NFT_CMP_EQ);
    if (lo == hi) {
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &blo, 2, NFT_CMP_EQ);
    } else {
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &blo, 2, NFT_CMP_GTE);
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &bhi, 2, NFT_CMP_LTE);
    }
    if (to) {
        el = expr_begin(mp, "immediate", &data);        /* the new port into register 1 */
        attr_u32(mp, NFTA_IMMEDIATE_DREG, NFT_REG_1);
        val = nest_begin(mp, NFTA_IMMEDIATE_DATA);
        attr(mp, NFTA_DATA_VALUE, &bto, 2);
        nest_end(mp, val);
        expr_end(mp, el, data);
    }
    el = expr_begin(mp, "redir", &data);
    if (to) attr_u32(mp, NFTA_REDIR_REG_PROTO_MIN, NFT_REG_1);
    expr_end(mp, el, data);
    nest_end(mp, exprs);
    msg_end(mp, at);
}

/* [ip saddr <src>] [iifname <ifn>] [ip daddr <dst>] <proto> [dport lo-hi] dnat to <to>
 * (src, dst 0 = any; ifn null = any; lo 0 = any port). The port is kept. */
static void dnat_msg(nlbuf *mp, uint32_t src, const char *ifn, uint32_t dst, uint8_t proto, uint16_t lo, uint16_t hi, uint32_t to)
{
    size_t at, exprs, el, data, val;
    uint32_t bsrc = htonl(src), bdst = htonl(dst), bto = htonl(to);
    uint16_t blo = htons(lo), bhi = htons(hi);
    at = msg_begin(mp, nft(NFT_MSG_NEWRULE), NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_IPV4, 0);
    attr_str(mp, NFTA_RULE_TABLE, TABLE);
    attr_str(mp, NFTA_RULE_CHAIN, CHAIN_PRE);
    exprs = nest_begin(mp, NFTA_RULE_EXPRESSIONS);
    if (ifn) {
        char name[IFNAMSIZ];
        memset(name, 0, sizeof name); strncpy(name, ifn, sizeof name - 1);
        el = expr_begin(mp, "meta", &data);             /* the name of the interface it came in on */
        attr_u32(mp, NFTA_META_KEY, NFT_META_IIFNAME);
        attr_u32(mp, NFTA_META_DREG, NFT_REG_1);
        expr_end(mp, el, data);
        el = expr_begin(mp, "cmp", &data);
        attr_u32(mp, NFTA_CMP_SREG, NFT_REG_1);
        attr_u32(mp, NFTA_CMP_OP, NFT_CMP_EQ);
        val = nest_begin(mp, NFTA_CMP_DATA);
        attr(mp, NFTA_DATA_VALUE, name, sizeof name);
        nest_end(mp, val);
        expr_end(mp, el, data);
    }
    if (src) match(mp, NFT_PAYLOAD_NETWORK_HEADER, 12, &bsrc, 4, NFT_CMP_EQ);
    if (dst) match(mp, NFT_PAYLOAD_NETWORK_HEADER, 16, &bdst, 4, NFT_CMP_EQ);
    match(mp, NFT_PAYLOAD_NETWORK_HEADER, 9, &proto, 1, NFT_CMP_EQ);
    if (lo && lo == hi) {
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &blo, 2, NFT_CMP_EQ);
    } else if (lo) {
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &blo, 2, NFT_CMP_GTE);
        match(mp, NFT_PAYLOAD_TRANSPORT_HEADER, 2, &bhi, 2, NFT_CMP_LTE);
    }
    el = expr_begin(mp, "immediate", &data);            /* the new address into register 1 */
    attr_u32(mp, NFTA_IMMEDIATE_DREG, NFT_REG_1);
    val = nest_begin(mp, NFTA_IMMEDIATE_DATA);
    attr(mp, NFTA_DATA_VALUE, &bto, 4);
    nest_end(mp, val);
    expr_end(mp, el, data);
    el = expr_begin(mp, "nat", &data);
    attr_u32(mp, NFTA_NAT_TYPE, NFT_NAT_DNAT);
    attr_u32(mp, NFTA_NAT_FAMILY, NFPROTO_IPV4);
    attr_u32(mp, NFTA_NAT_REG_ADDR_MIN, NFT_REG_1);
    expr_end(mp, el, data);
    nest_end(mp, exprs);
    msg_end(mp, at);
}

int nat_masquerade(uint32_t peer_ip) { return nat_masquerade2(peer_ip, 0); }

int nat_masquerade2(uint32_t peer_ip, uint32_t peer2_ip)
{
    nat_plan p;
    memset(&p, 0, sizeof p);
    p.peer[0] = peer_ip; p.peer[1] = peer2_ip; p.masquerade = 1;
    return nat_apply(&p);
}

/* ---- the iptables way, for kernels before nftables (3.13) ----
 *
 * The same plan as two chains of the nat table, v90d_post and v90d_pre,
 * put first in POSTROUTING and PREROUTING, through the box's own iptables
 * program (the OBi200's Linux 2.6.30 has iptables and no nf_tables). Taken
 * on such a kernel only (ipt_kernel). */

#define IPT_POST "v90d_post"
#define IPT_PRE  "v90d_pre"

/* Linux before 3.13 has no nf_tables: decided once, from the kernel's
 * version, so a refused rule on a kernel that has nf_tables stays an error
 * of its own and never turns into iptables rules. */
static int ipt_kernel(void)
{
    static int known = -1;
    struct utsname u;
    if (known < 0) {
        int major = 0, minor = 0;
        known = uname(&u) == 0 && sscanf(u.release, "%d.%d", &major, &minor) == 2 && (major < 3 || (major == 3 && minor < 13));
    }
    return known;
}

static const char *ipt_path(void)
{
    static const char *const where[] = { "/usr/sbin/iptables", "/sbin/iptables", "/usr/bin/iptables", NULL };
    int i;
    for (i = 0; where[i]; i++) if (access(where[i], X_OK) == 0) return where[i];
    return NULL;
}

/* run iptables -t nat with these arguments (NULL-terminated); its exit status, or -1 */
static int ipt(const char *const *args)
{
    const char *argv[32];
    const char *path = ipt_path();
    int n = 0, st;
    pid_t pid;
    if (!path) { errno = ENOENT; return -1; }
    argv[n++] = path; argv[n++] = "-t"; argv[n++] = "nat";
    while (*args && n < 31) argv[n++] = *args++;
    argv[n] = NULL;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        struct sched_param sp;
        int fd = open("/dev/null", O_RDWR);
        memset(&sp, 0, sizeof sp);
        (void)sched_setscheduler(0, SCHED_OTHER, &sp);     /* not the daemon's real-time priority */
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execv(path, (char *const *)argv);
        _exit(127);
    }
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void ipt_unhook(void)
{
    static const char *const dpost[] = { "-D", "POSTROUTING", "-j", IPT_POST, NULL };
    static const char *const dpre[] = { "-D", "PREROUTING", "-j", IPT_PRE, NULL };
    static const char *const fpost[] = { "-F", IPT_POST, NULL }, *const xpost[] = { "-X", IPT_POST, NULL };
    static const char *const fpre[] = { "-F", IPT_PRE, NULL }, *const xpre[] = { "-X", IPT_PRE, NULL };
    int i;
    /* no -C in older iptables: delete the jump until there is none */
    for (i = 0; i < 8 && ipt(dpost) == 0; i++) {}
    for (i = 0; i < 8 && ipt(dpre) == 0; i++) {}
    ipt(fpost); ipt(xpost); ipt(fpre); ipt(xpre);
}

static const char *ip_str(uint32_t ip, char *b)
{
    snprintf(b, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

static int ipt_apply(const nat_plan *p)
{
    /* our chains first in the nat table: the system's own rules must not see the packets first */
    static const char *const npost[] = { "-N", IPT_POST, NULL }, *const hpost[] = { "-I", "POSTROUTING", "1", "-j", IPT_POST, NULL };
    static const char *const npre[] = { "-N", IPT_PRE, NULL }, *const hpre[] = { "-I", "PREROUTING", "1", "-j", IPT_PRE, NULL };
    char a[16], b[16], c[16], ports[16], to[8];
    int i, j, bad = 0;
    if (!ipt_path()) { errno = ENOENT; return -1; }
    ipt_unhook();
    if (ipt(npost) || ipt(hpost)) { ipt_unhook(); errno = EIO; return -1; }
    for (i = 0; i < 2; i++) if (p->masquerade && p->peer[i] && !p->lan[i]) {
        const char *r[] = { "-A", IPT_POST, "-s", ip_str(p->peer[i], a), "-j", "MASQUERADE", NULL };
        bad |= ipt(r);
    }
    if (p->vpn[0]) {
        const char *r[] = { "-A", IPT_POST, "-o", p->vpn, "-j", "MASQUERADE", NULL };
        bad |= ipt(r);
    }
    if ((p->peer[0] && (p->archive[0] || p->console[0])) || (p->peer[1] && (p->archive[1] || p->console[1]))) {
        if (ipt(npre) || ipt(hpre)) { ipt_unhook(); errno = EIO; return -1; }
        for (i = 0; i < 2; i++) if (p->peer[i] && p->archive[i]) {
            static const struct { const char *proto, *port; int which; } R[] = {
                { "udp", "53", 0 }, { "tcp", "80", 1 }, { "tcp", "21", 2 } };
            for (j = 0; j < 3; j++) {
                uint16_t t = R[j].which == 0 ? p->dns_port : R[j].which == 1 ? p->http_port : p->ftp_port;
                const char *r[] = { "-A", IPT_PRE, "-s", ip_str(p->peer[i], a), "-p", R[j].proto, "--dport", R[j].port,
                                    "-j", "REDIRECT", "--to-ports", to, NULL };
                snprintf(to, sizeof to, "%u", t);
                bad |= ipt(r);
            }
            if (p->pasv_lo && p->pasv_hi >= p->pasv_lo) {
                const char *r[] = { "-A", IPT_PRE, "-s", ip_str(p->peer[i], a), "-p", "tcp", "--dport", ports, "-j", "REDIRECT", NULL };
                snprintf(ports, sizeof ports, "%u:%u", p->pasv_lo, p->pasv_hi);
                bad |= ipt(r);
            }
        }
        {
            int opened = 0;
            for (i = 0; i < 2; i++) if (p->peer[i] && p->console[i]) {
                for (j = 0; j < p->n_rewrite && j < NAT_REWRITE_MAX; j++) {
                    const nat_rewrite *w = &p->rewrite[j];
                    const char *r[16]; int n = 0;
                    r[n++] = "-A"; r[n++] = IPT_PRE; r[n++] = "-s"; r[n++] = ip_str(p->peer[i], a);
                    r[n++] = "-d"; r[n++] = ip_str(w->from, b); r[n++] = "-p"; r[n++] = w->proto == 17 ? "udp" : "tcp";
                    if (w->port) { snprintf(ports, sizeof ports, "%u", w->port); r[n++] = "--dport"; r[n++] = ports; }
                    r[n++] = "-j"; r[n++] = "DNAT"; r[n++] = "--to-destination"; r[n++] = ip_str(w->to, c); r[n] = NULL;
                    bad |= ipt(r);
                }
                if (!opened && p->wan[0] && !p->lan[i]) {
                    opened = 1;
                    for (j = 0; j < p->n_open && j < NAT_OPEN_MAX; j++) {
                        const nat_open *o = &p->open[j];
                        const char *r[16]; int m = 0;
                        r[m++] = "-A"; r[m++] = IPT_PRE; r[m++] = "-i"; r[m++] = p->wan;
                        r[m++] = "-p"; r[m++] = o->proto == 17 ? "udp" : "tcp";
                        if (o->lo) {                    /* lo 0: any port, as dnat_msg has it */
                            if (o->lo == o->hi) snprintf(ports, sizeof ports, "%u", o->lo);
                            else snprintf(ports, sizeof ports, "%u:%u", o->lo, o->hi);
                            r[m++] = "--dport"; r[m++] = ports;
                        }
                        r[m++] = "-j"; r[m++] = "DNAT"; r[m++] = "--to-destination"; r[m++] = ip_str(p->peer[i], a); r[m] = NULL;
                        bad |= ipt(r);
                    }
                }
            }
        }
    }
    /* all or nothing, as the nftables batch is */
    if (bad) { ipt_unhook(); errno = EIO; return -1; }
    return 0;
}

static int nft_apply(const nat_plan *p);

int nat_apply(const nat_plan *p)
{
    if (ipt_kernel()) return ipt_apply(p);
    return nft_apply(p);
}

static int nft_apply(const nat_plan *p)
{
    static nlbuf m;                 /* not on the stack: 64 kB */
    size_t at, hook;
    uint32_t first;
    int i;
    memset(&m, 0, sizeof m);
    m.seq = (uint32_t)getpid() << 8;

    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_BEGIN, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    first = m.seq + 1;
    /* create if absent, delete, create: leaves a fresh table either way */
    table_msg(&m, NFT_MSG_NEWTABLE, NLM_F_CREATE | NLM_F_ACK);
    table_msg(&m, NFT_MSG_DELTABLE, NLM_F_ACK);
    table_msg(&m, NFT_MSG_NEWTABLE, NLM_F_CREATE | NLM_F_ACK);

    at = msg_begin(&m, nft(NFT_MSG_NEWCHAIN), NLM_F_CREATE | NLM_F_ACK, NFPROTO_IPV4, 0);
    attr_str(&m, NFTA_CHAIN_TABLE, TABLE);
    attr_str(&m, NFTA_CHAIN_NAME, CHAIN);
    hook = nest_begin(&m, NFTA_CHAIN_HOOK);
    attr_u32(&m, NFTA_HOOK_HOOKNUM, NF_INET_POST_ROUTING);
    attr_u32(&m, NFTA_HOOK_PRIORITY, 100);              /* the usual source NAT priority */
    nest_end(&m, hook);
    attr_str(&m, NFTA_CHAIN_TYPE, "nat");
    msg_end(&m, at);

    for (i = 0; i < 2; i++) if (p->masquerade && p->peer[i] && !p->lan[i]) rule_msg(&m, p->peer[i]);
    if (p->vpn[0]) masq_if_msg(&m, p->vpn);

    if ((p->peer[0] && (p->archive[0] || p->console[0])) || (p->peer[1] && (p->archive[1] || p->console[1]))) {
        at = msg_begin(&m, nft(NFT_MSG_NEWCHAIN), NLM_F_CREATE | NLM_F_ACK, NFPROTO_IPV4, 0);
        attr_str(&m, NFTA_CHAIN_TABLE, TABLE);
        attr_str(&m, NFTA_CHAIN_NAME, CHAIN_PRE);
        hook = nest_begin(&m, NFTA_CHAIN_HOOK);
        attr_u32(&m, NFTA_HOOK_HOOKNUM, NF_INET_PRE_ROUTING);
        attr_u32(&m, NFTA_HOOK_PRIORITY, (uint32_t)-100);   /* the usual destination NAT priority */
        nest_end(&m, hook);
        attr_str(&m, NFTA_CHAIN_TYPE, "nat");
        msg_end(&m, at);
        for (i = 0; i < 2; i++) if (p->peer[i] && p->archive[i]) {
            redirect_msg(&m, p->peer[i], 17, 53, 53, p->dns_port);
            redirect_msg(&m, p->peer[i], 6, 80, 80, p->http_port);
            redirect_msg(&m, p->peer[i], 6, 21, 21, p->ftp_port);
            if (p->pasv_lo && p->pasv_hi >= p->pasv_lo) redirect_msg(&m, p->peer[i], 6, p->pasv_lo, p->pasv_hi, 0);
        }
        {
            int opened = 0, j;
            for (i = 0; i < 2; i++) if (p->peer[i] && p->console[i]) {
                for (j = 0; j < p->n_rewrite && j < NAT_REWRITE_MAX; j++)
                    dnat_msg(&m, p->peer[i], NULL, p->rewrite[j].from, p->rewrite[j].proto,
                             p->rewrite[j].port, p->rewrite[j].port, p->rewrite[j].to);
                if (!opened && p->wan[0] && !p->lan[i]) {
                    opened = 1;
                    for (j = 0; j < p->n_open && j < NAT_OPEN_MAX; j++)
                        dnat_msg(&m, 0, p->wan, 0, p->open[j].proto, p->open[j].lo, p->open[j].hi, p->peer[i]);
                }
            }
        }
    }

    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_END, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    return transact(&m, first);
}

static int nft_remove(void);

int nat_remove(void)
{
    if (ipt_kernel()) {
        if (!ipt_path()) { errno = ENOENT; return -1; }
        ipt_unhook();
        return 0;
    }
    return nft_remove();
}

static int nft_remove(void)
{
    nlbuf m; uint32_t first;
    memset(&m, 0, sizeof m);
    m.seq = (uint32_t)getpid() << 8;
    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_BEGIN, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    first = m.seq + 1;
    table_msg(&m, NFT_MSG_DELTABLE, NLM_F_ACK);
    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_END, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    return transact(&m, first);
}

int nat_ip_forward(int on)
{
    int fd = open("/proc/sys/net/ipv4/ip_forward", O_WRONLY | O_CLOEXEC), err;
    if (fd < 0) return -1;
    if (write(fd, on ? "1\n" : "0\n", 2) != 2) { err = errno; close(fd); errno = err; return -1; }
    return close(fd);
}

/* ------------------------------------------------- the callers' IPv6 (#7) */
#define TABLE6 "v90d6"

static void table6_msg(nlbuf *m, int msg, uint16_t flags)
{
    size_t at = msg_begin(m, nft(msg), flags, NFPROTO_IPV6, 0);
    attr_str(m, NFTA_TABLE_NAME, TABLE6);
    msg_end(m, at);
}

int nat_clat6(int on, const uint8_t prefix96[16])
{
    static nlbuf m;                 /* not on the stack: 64 kB */
    size_t at, hook, exprs, el, data, val;
    uint32_t first;
    if (ipt_kernel()) { errno = EOPNOTSUPP; return -1; }   /* the maker's kernel: no IPv6 */
    memset(&m, 0, sizeof m);
    m.seq = (uint32_t)getpid() << 8 | 0x80;
    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_BEGIN, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    first = m.seq + 1;
    table6_msg(&m, NFT_MSG_NEWTABLE, NLM_F_CREATE | NLM_F_ACK);    /* a fresh table, or none */
    table6_msg(&m, NFT_MSG_DELTABLE, NLM_F_ACK);
    if (on) {
        table6_msg(&m, NFT_MSG_NEWTABLE, NLM_F_CREATE | NLM_F_ACK);
        at = msg_begin(&m, nft(NFT_MSG_NEWCHAIN), NLM_F_CREATE | NLM_F_ACK, NFPROTO_IPV6, 0);
        attr_str(&m, NFTA_CHAIN_TABLE, TABLE6);
        attr_str(&m, NFTA_CHAIN_NAME, CHAIN);
        hook = nest_begin(&m, NFTA_CHAIN_HOOK);
        attr_u32(&m, NFTA_HOOK_HOOKNUM, NF_INET_POST_ROUTING);
        attr_u32(&m, NFTA_HOOK_PRIORITY, 100);
        nest_end(&m, hook);
        attr_str(&m, NFTA_CHAIN_TYPE, "nat");
        msg_end(&m, at);
        /* ip6 saddr <prefix>::/96 masquerade */
        at = msg_begin(&m, nft(NFT_MSG_NEWRULE), NLM_F_CREATE | NLM_F_APPEND | NLM_F_ACK, NFPROTO_IPV6, 0);
        attr_str(&m, NFTA_RULE_TABLE, TABLE6);
        attr_str(&m, NFTA_RULE_CHAIN, CHAIN);
        exprs = nest_begin(&m, NFTA_RULE_EXPRESSIONS);
        el = expr_begin(&m, "payload", &data);          /* the first 96 bits of the source address */
        attr_u32(&m, NFTA_PAYLOAD_DREG, NFT_REG_1);
        attr_u32(&m, NFTA_PAYLOAD_BASE, NFT_PAYLOAD_NETWORK_HEADER);
        attr_u32(&m, NFTA_PAYLOAD_OFFSET, 8);
        attr_u32(&m, NFTA_PAYLOAD_LEN, 12);
        expr_end(&m, el, data);
        el = expr_begin(&m, "cmp", &data);
        attr_u32(&m, NFTA_CMP_SREG, NFT_REG_1);
        attr_u32(&m, NFTA_CMP_OP, NFT_CMP_EQ);
        val = nest_begin(&m, NFTA_CMP_DATA);
        attr(&m, NFTA_DATA_VALUE, prefix96, 12);
        nest_end(&m, val);
        expr_end(&m, el, data);
        el = expr_begin(&m, "masq", &data);
        expr_end(&m, el, data);
        nest_end(&m, exprs);
        msg_end(&m, at);
    }
    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_END, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    return transact(&m, first);
}
