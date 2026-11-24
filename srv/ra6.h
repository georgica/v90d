/* What the network's IPv6 router says (#7): the box's address the kernel
 * takes itself from router advertisements; the name servers (RDNSS,
 * RFC 8106) and the NAT64 prefix (PREF64, RFC 8781) it does not, and an
 * IPv6-only network gives them no other way. They are listened for here,
 * on the LAN port only, from a link-local router at hop limit 255 (as RFC
 * 4861 has a host take an advertisement); the name servers go into the
 * box's resolv.conf after the ones DHCP put there, and the relay for the
 * callers (dnsrelay.h) reads them from there. The parsing is pure, for the
 * tests; the socket is the daemon's (ra6.c). Ours; written for this project. */
#ifndef V90D_RA6_H
#define V90D_RA6_H
#include <stddef.h>
#include <stdint.h>

#define RA6_DNS_MAX 3

typedef struct {
    uint8_t  dns[RA6_DNS_MAX][16];
    uint32_t dns_until[RA6_DNS_MAX];    /* seconds (the caller's clock); 0 = gone */
    int      n_dns;
    uint8_t  pref64[16];                /* the NAT64 prefix, its length in bits (32..96) */
    int      pref64_len;
    uint32_t pref64_until;
    uint32_t heard;                     /* when the last advertisement came; 0 = never */
} ra6_info;

/* Take one router advertisement (the ICMPv6 message: type 134 first) heard
 * at now_s into *in: its name servers replace the ones it had (a lifetime
 * of 0 withdraws them), its PREF64 the prefix. Returns 0, or -1 if it is
 * no well-formed advertisement (then *in is as it was). */
int  ra6_parse(const uint8_t *m, size_t n, uint32_t now_s, ra6_info *in);
/* forget what has run out by now_s */
void ra6_expire(ra6_info *in, uint32_t now_s);

/* --- the daemon's side --- */
/* listen on ifname (NULL or off: stop) */
void ra6_want(const char *ifname, int on);
/* every block: the advertisements, a solicitation now and then, the box's resolv.conf */
void ra6_poll(uint32_t now_ms);
const ra6_info *ra6_now(void);
/* the resolv.conf lines for ours: after the marker line, which ra6 rewrites */
#define RA6_MARK "# v90d: IPv6 name servers from the router (ra6)"

#endif
