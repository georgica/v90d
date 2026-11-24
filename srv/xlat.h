/* The callers' IPv4 over IPv6 (#7): a translator between the callers' IPv4
 * packets and IPv6 ones, as RFC 7915 (SIIT) has it, for a box whose network
 * has IPv6 alone (or for a site that has IPv6 alone):
 *
 *   caller 10.64.64.2       <->  clat ::/96 + 10.64.64.2 (a prefix of the
 *                                 box's own; the kernel's IPv6 masquerade
 *                                 then puts the box's address on it)
 *   a site's IPv4 X         <->  the NAT64 prefix + X (RFC 6052, any length),
 *                                 X on the internet at large only
 *                                 (bbsdial_is_public), both ways
 *   a stand-in 198.18.x.y   <->  the IPv6 address of a site that has no
 *                                 IPv4 (the callers' relay hands these out,
 *                                 xlat_pool_*)
 *
 * Pure: no I/O, no clock of its own. TCP, UDP and ICMP (echo, and the
 * errors with the packet they quote); TCP's MSS on a SYN is brought down
 * to what fits 1280 octets of IPv6; an IPv4 packet with DF that would not
 * fit 1280 is answered "fragmentation needed" instead. Fragments are
 * translated as fragments. Ours; written for this project. */
#ifndef V90D_XLAT_H
#define V90D_XLAT_H
#include <stddef.h>
#include <stdint.h>

#define XLAT_MTU6        1280   /* what every IPv6 path carries */
#define XLAT_MTU4        (XLAT_MTU6 - 20)
#define XLAT_POOL_NET    0xc6120000u    /* 198.18.0.0/15, the stand-ins */
#define XLAT_POOL_BITS   15
#define XLAT_POOL_MAX    1024   /* stand-ins in use at once (the oldest gives way) */

typedef struct {
    uint32_t v4;                /* 198.18.x.y */
    uint8_t  v6[16];
    uint32_t used;              /* seconds, the caller's clock: the last time it was handed out or used */
} xlat_entry;

typedef struct {
    uint8_t  clat[16];          /* the callers' /96 */
    uint8_t  pref64[16];        /* the NAT64 prefix; pref64_len 0: none (only stand-ins go) */
    int      pref64_len;
    xlat_entry pool[XLAT_POOL_MAX];
    int      n_pool;
    uint32_t next;              /* the next stand-in to hand out */
} xlat_t;

void xlat_init(xlat_t *x, const uint8_t clat96[16]);
/* the NAT64 prefix (32, 40, 48, 56, 64 or 96 bits; 0 = none) */
void xlat_set_pref64(xlat_t *x, const uint8_t p[16], int len);

/* a stand-in IPv4 address for IPv6 address v6 (the same one again while it
 * is kept); 0 if the pool is not usable */
uint32_t xlat_pool_get(xlat_t *x, const uint8_t v6[16], uint32_t now_s);
/* the IPv6 address of a stand-in, NULL if it is none of ours */
const uint8_t *xlat_pool_find(xlat_t *x, uint32_t v4, uint32_t now_s);
/* is v4 in the stand-ins' range? */
int xlat_is_pool(uint32_t v4);

/* what became of a packet */
enum { XLAT_DROP = 0,           /* nothing to send */
       XLAT_OUT,                /* out: the translated packet, to the other side */
       XLAT_BACK };             /* out: an answer to the sender (ICMP), on its own side */

/* a caller's IPv4 packet: *out_n octets in out (cap at least 1300) */
int xlat_4to6(xlat_t *x, const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *out_n, uint32_t now_s);
/* an IPv6 packet for the callers (to the clat /96) */
int xlat_6to4(xlat_t *x, const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *out_n, uint32_t now_s);

/* the IPv4 address in v6 by the NAT64 prefix p/len (RFC 6052), 1 if it is in it */
int xlat_from_nat64(const uint8_t v6[16], const uint8_t *p, int len, uint32_t *v4);
/* v6 = the NAT64 prefix p/len with v4 in it */
void xlat_to_nat64(uint8_t v6[16], const uint8_t *p, int len, uint32_t v4);

#endif
