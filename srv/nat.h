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
typedef struct { uint8_t proto; uint16_t lo, hi; } nat_open;
#define NAT_REWRITE_MAX 32
#define NAT_OPEN_MAX    40

typedef struct {
    uint32_t peer[2];           /* host byte order; peer[1] 0 = one port only */
    int      masquerade;
    int      archive[2];        /* per peer */
    uint16_t http_port, ftp_port, dns_port, pasv_lo, pasv_hi;
    int      console[2];        /* per peer */
    const nat_rewrite *rewrite; int n_rewrite;
    const nat_open *open; int n_open;
    char     wan[16];           /* interface name; "" = no ports opened */
} nat_plan;
int nat_apply(const nat_plan *p);
/* Remove our table. 0, or -1 with errno (ENOENT if it was not there). */
int nat_remove(void);
/* Write 1 or 0 to /proc/sys/net/ipv4/ip_forward. 0 or -1 with errno. */
int nat_ip_forward(int on);

#endif
