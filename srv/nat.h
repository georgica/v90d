/* Network address translation for the dial-in peer, set up through the
 * kernel's nftables netlink interface (no iptables or nft program on the
 * box): one table "v90d" with one rule per dial-in peer, "packets from the
 * peer's address leave masqueraded". Linux only. */
#ifndef V90D_SRV_NAT_H
#define V90D_SRV_NAT_H
#include <stdint.h>

/* Install (or replace) the rule for peer_ip (host byte order). Returns 0,
 * or -1 with errno set (EPERM without CAP_NET_ADMIN, ENOENT / EOPNOTSUPP
 * when the kernel has no nf_tables or no masquerade). */
int nat_masquerade(uint32_t peer_ip);
/* The same for two peers (two telephone ports); peer2_ip 0 = only one. */
int nat_masquerade2(uint32_t peer_ip, uint32_t peer2_ip);
/* Everything at once. With archive[i] set, what peer i sends to TCP port 80,
 * TCP port 21, TCP ports pasv_lo..pasv_hi and UDP port 53, whatever the
 * destination address, is redirected to the gateway (arch/) on this box at
 * http_port, ftp_port, the same port, and dns_port. Needs the kernel's
 * redirect expression (CONFIG_NFT_REDIR). */
/* For a peer that is a game console (console[i] set):
 *  - rewrite: what it sends to address `from` (TCP or UDP as `proto` says,
 *    to port `port`, or to any port when that is 0) goes to address `to`
 *    instead, the port unchanged;
 *  - open: what arrives on the network interface `wan` for this box on the
 *    ports lo..hi of protocol `proto` is passed on to the console. Only one
 *    console can have them: the first port that is one. */
typedef struct { uint32_t from, to; uint16_t port; uint8_t proto; } nat_rewrite;
/* For a peer in a WebTV call (webtv[i] set, srv.h srv_webtv): what it sends
 * to NAT_WEBTV_ADDR (TCP or UDP, any port) goes to webtv_to, the port
 * unchanged (no rule while webtv_to is 0); its DNS (UDP and TCP port 53) to
 * NAT_WEBTV_DNS goes to webtv_dns. A WebTV / MSN TV box talks to these
 * fixed addresses of its old network. */
#define NAT_WEBTV_ADDR 0x0A000001u      /* 10.0.0.1 */
#define NAT_WEBTV_DNS  0x9D39DD33u      /* 157.57.221.51 */
typedef struct { uint8_t proto; uint16_t lo, hi; } nat_open;
#define NAT_REWRITE_MAX 32
#define NAT_OPEN_MAX    40

typedef struct {
    uint32_t peer[2];           /* host byte order; peer[1] 0 = one port only */
    int      masquerade;
    int      archive[2];        /* per peer */
    uint16_t http_port, ftp_port, dns_port, pasv_lo, pasv_hi;
    int      console[2];        /* per peer */
    int      lan[2];            /* per peer: a host on the network itself, not masqueraded, no ports opened */
    int      webtv[2];          /* per peer: in a WebTV call */
    uint32_t webtv_to, webtv_dns;   /* WebTV Redialed (0 = not known yet), the port's name server; host order */
    const nat_rewrite *rewrite; int n_rewrite;
    const nat_open *open; int n_open;
    char     wan[16];           /* interface name; "" = no ports opened */
    char        vpn[16];        /* not empty: whatever leaves by this device is masqueraded (the tunnel to the
                                   IWANGO games' network), also for a console that is a host on the network */
} nat_plan;
int nat_apply(const nat_plan *p);
/* Remove our table. 0, or -1 with errno (ENOENT if it was not there). */
int nat_remove(void);
/* The callers' IPv6 (#7, srv/clat.c): a table "v90d6" of its own, in which
 * what leaves from prefix96::/96 (the first 12 octets) is masqueraded to
 * the box's own IPv6 address; off removes it. 0, or -1 with errno
 * (EOPNOTSUPP under a kernel without nf_tables, which has no IPv6 here). */
#include <stdint.h>
int nat_clat6(int on, const uint8_t prefix96[16]);
/* Write 1 or 0 to /proc/sys/net/ipv4/ip_forward. 0 or -1 with errno. */
int nat_ip_forward(int on);

#endif
