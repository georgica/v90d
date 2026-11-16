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
#include <linux/netlink.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nf_tables.h>
#include "nat.h"

#define TABLE "v90d"
#define CHAIN "postrouting"
#define CHAIN_PRE "prerouting"

typedef struct { uint8_t b[8192]; size_t n; int bad; uint32_t seq; } nlbuf;

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

int nat_masquerade(uint32_t peer_ip) { return nat_masquerade2(peer_ip, 0); }

int nat_masquerade2(uint32_t peer_ip, uint32_t peer2_ip)
{
    nat_plan p;
    memset(&p, 0, sizeof p);
    p.peer[0] = peer_ip; p.peer[1] = peer2_ip; p.masquerade = 1;
    return nat_apply(&p);
}

int nat_apply(const nat_plan *p)
{
    static nlbuf m;                 /* not on the stack: 8 kB */
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

    for (i = 0; i < 2; i++) if (p->masquerade && p->peer[i]) rule_msg(&m, p->peer[i]);

    if ((p->peer[0] && p->archive[0]) || (p->peer[1] && p->archive[1])) {
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
    }

    msg_end(&m, msg_begin(&m, NFNL_MSG_BATCH_END, 0, AF_UNSPEC, NFNL_SUBSYS_NFTABLES));
    return transact(&m, first);
}

int nat_remove(void)
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
