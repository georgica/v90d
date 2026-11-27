/* Van Jacobson TCP/IP header compression (RFC 1144); see ppp_vj.h.
 * Written from RFC 1144 (sections 3.2 and 4, and its appendix A's
 * algorithm); integer only, no allocation. */
#include <string.h>
#include "ppp_vj.h"

/* the changes octet (RFC 1144 3.2.2) */
#define VJ_C        0x40    /* the slot number follows */
#define VJ_I        0x20    /* IP identification changed by other than 1 */
#define VJ_P        0x10    /* TCP PSH */
#define VJ_S        0x08    /* sequence number delta */
#define VJ_A        0x04    /* acknowledgement delta */
#define VJ_W        0x02    /* window delta */
#define VJ_U        0x01    /* urgent pointer */
#define VJ_SPECIAL_I (VJ_S | VJ_W | VJ_U)       /* echoed typing: seq and ack both by the data */
#define VJ_SPECIAL_D (VJ_S | VJ_A | VJ_W | VJ_U) /* unidirectional data: seq by the data */
#define VJ_SPECIALS  (VJ_S | VJ_A | VJ_W | VJ_U)

#define TH_FIN 0x01
#define TH_SYN 0x02
#define TH_RST 0x04
#define TH_PSH 0x08
#define TH_ACK 0x10
#define TH_URG 0x20

static uint16_t g16(const uint8_t *p) { return (uint16_t)((unsigned)p[0] << 8 | p[1]); }
static uint32_t g32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void p16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void p32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint16_t ip_cksum(const uint8_t *h, size_t n)
{
    uint32_t s = 0; size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)h[i] << 8 | h[i + 1];
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (uint16_t)~s;
}

void ppp_vj_tx_init(ppp_vj_t *v, unsigned slots, int cid_comp)
{
    unsigned i;
    memset(v->tx, 0, sizeof v->tx);
    v->tx_slots = (uint8_t)(slots > PPP_VJ_SLOTS ? PPP_VJ_SLOTS : slots);
    v->tx_cid_comp = (uint8_t)(cid_comp != 0);
    for (i = 0; i < PPP_VJ_SLOTS; i++) v->tx_lru[i] = (uint8_t)i;
    v->tx_last = -1;
}

void ppp_vj_rx_init(ppp_vj_t *v, unsigned slots)
{
    memset(v->rx, 0, sizeof v->rx);
    v->rx_slots = (uint8_t)(slots > PPP_VJ_SLOTS ? PPP_VJ_SLOTS : slots);
    v->rx_last = -1;
    v->rx_toss = 0;
}

void ppp_vj_rx_lost(ppp_vj_t *v)
{
    v->rx_toss = 1;
}

/* the delta encodings (RFC 1144 3.2.3): one octet 1..255, or 0 and two
 * octets; "z" also sends 0 in the long form, where 0 is a real value */
static uint8_t *enc(uint8_t *cp, uint32_t n)
{
    if (n >= 256 || n == 0) { *cp++ = 0; *cp++ = (uint8_t)(n >> 8); *cp++ = (uint8_t)n; }
    else *cp++ = (uint8_t)n;
    return cp;
}
static uint8_t *encz(uint8_t *cp, uint32_t n)
{
    if (n >= 256 || n == 0) { *cp++ = 0; *cp++ = (uint8_t)(n >> 8); *cp++ = (uint8_t)n; }
    else *cp++ = (uint8_t)n;
    return cp;
}

/* the header sizes of an IPv4 TCP datagram that may be compressed: IP and
 * TCP header lengths, or 0 when it may not (RFC 1144 3.2.1) */
static size_t tcp_hlen(const uint8_t *ip, size_t len, size_t *iphl)
{
    size_t ihl, thl;
    if (len < 40 || (ip[0] >> 4) != 4) return 0;
    ihl = (size_t)(ip[0] & 0x0f) * 4;
    if (ihl < 20 || ip[9] != 6) return 0;
    if (g16(ip + 6) & 0x3fff) return 0;             /* a fragment (MF or an offset) */
    if (g16(ip + 2) != len) return 0;               /* the length must be the datagram's */
    if (len < ihl + 20) return 0;
    thl = (size_t)(ip[ihl + 12] >> 4) * 4;
    if (thl < 20 || len < ihl + thl || ihl + thl > PPP_VJ_HDR_MAX) return 0;
    *iphl = ihl;
    return ihl + thl;
}

/* slot `s` to the front of the use order */
static void touch(ppp_vj_t *v, int s)
{
    int i, j;
    for (i = 0; i < v->tx_slots && v->tx_lru[i] != s; i++) {}
    if (i >= v->tx_slots) return;
    for (j = i; j > 0; j--) v->tx_lru[j] = v->tx_lru[j - 1];
    v->tx_lru[0] = (uint8_t)s;
}

uint16_t ppp_vj_compress(ppp_vj_t *v, const uint8_t *ip, size_t len,
                         uint8_t *out, size_t max, size_t *outlen)
{
    size_t ihl = 0, hlen, i;
    const uint8_t *th, *oip, *oth;
    uint8_t deltas[16], *cp = deltas;
    unsigned changes = 0;
    uint32_t d;
    int s = -1;
    ppp_vj_slot_t *cs;

    *outlen = 0;
    if (max < len) return 0;
    hlen = v->tx_slots ? tcp_hlen(ip, len, &ihl) : 0;
    th = ip + ihl;
    if (!hlen || (th[13] & (TH_SYN | TH_FIN | TH_RST | TH_ACK)) != TH_ACK) {
        memcpy(out, ip, len);
        *outlen = len;
        return 0x0021;
    }
    /* the connection's slot: same addresses and ports (most recent first);
     * none, the least recently used one is taken over */
    for (i = 0; i < v->tx_slots; i++) {
        const ppp_vj_slot_t *c = &v->tx[v->tx_lru[i]];
        size_t cih;
        if (!c->hlen) continue;
        cih = (size_t)(c->hdr[0] & 0x0f) * 4;
        if (memcmp(c->hdr + 12, ip + 12, 8) == 0 && memcmp(c->hdr + cih, th, 4) == 0) { s = v->tx_lru[i]; break; }
    }
    if (s < 0) {
        s = v->tx_lru[v->tx_slots - 1];
        goto uncompressed;
    }
    cs = &v->tx[s];
    oip = cs->hdr;
    oth = cs->hdr + (size_t)(oip[0] & 0x0f) * 4;
    /* what must be the same for a compressed packet: version, header length
     * and TOS; fragment field; TTL and protocol; the TCP header length;
     * IP and TCP options */
    if (cs->hlen != hlen || memcmp(oip, ip, 2) != 0 || memcmp(oip + 6, ip + 6, 4) != 0 ||
        (oth[12] >> 4) != (th[12] >> 4) ||
        memcmp(oip + 20, ip + 20, ihl - 20) != 0 || memcmp(oth + 20, th + 20, hlen - ihl - 20) != 0)
        goto uncompressed;

    /* the deltas, in the order RFC 1144 sends them: U W A S (I after) */
    if (th[13] & TH_URG) {
        cp = encz(cp, g16(th + 18));
        changes |= VJ_U;
    } else if (g16(th + 18) != g16(oth + 18))
        goto uncompressed;
    d = (uint16_t)(g16(th + 14) - g16(oth + 14));
    if (d) { cp = enc(cp, d); changes |= VJ_W; }
    d = g32(th + 8) - g32(oth + 8);
    if (d) {
        if (d > 0xffff) goto uncompressed;
        cp = enc(cp, d); changes |= VJ_A;
    }
    d = g32(th + 4) - g32(oth + 4);
    if (d) {
        if (d > 0xffff) goto uncompressed;
        cp = enc(cp, d); changes |= VJ_S;
    }
    switch (changes) {
    case 0:
        /* nothing changed: data after a bare ack (typing) goes compressed;
         * anything else is probably a resend, and goes uncompressed so a
         * receiver that lost step gets it again */
        if (g16(ip + 2) != g16(oip + 2) && g16(oip + 2) == hlen) break;
        goto uncompressed;
    case VJ_SPECIAL_I:
    case VJ_SPECIAL_D:
        /* the real changes look like a special case: say it in full */
        goto uncompressed;
    case VJ_S | VJ_A:
        d = g32(th + 4) - g32(oth + 4);
        if (d == g32(th + 8) - g32(oth + 8) && d == (uint32_t)(g16(oip + 2) - hlen)) {
            changes = VJ_SPECIAL_I; cp = deltas;
        }
        break;
    case VJ_S:
        d = g32(th + 4) - g32(oth + 4);
        if (d == (uint32_t)(g16(oip + 2) - hlen)) { changes = VJ_SPECIAL_D; cp = deltas; }
        break;
    default:
        break;
    }
    d = (uint16_t)(g16(ip + 4) - g16(oip + 4));
    if (d != 1) { cp = encz(cp, d); changes |= VJ_I; }
    if (th[13] & TH_PSH) changes |= VJ_P;
    /* the new header is the slot's from here on */
    memcpy(cs->hdr, ip, hlen);
    touch(v, s);
    {
        size_t dl = (size_t)(cp - deltas), at = 0, data = len - hlen;
        if (1 + 1 + 2 + dl + data > max) return 0;
        if (!v->tx_cid_comp || s != v->tx_last) {
            out[at++] = (uint8_t)(changes | VJ_C);
            out[at++] = (uint8_t)s;
        } else
            out[at++] = (uint8_t)changes;
        out[at++] = th[16]; out[at++] = th[17];     /* the TCP checksum as it is */
        memcpy(out + at, deltas, dl); at += dl;
        memcpy(out + at, ip + hlen, data); at += data;
        v->tx_last = s;
        *outlen = at;
        return PPP_PROTO_VJC;
    }

uncompressed:
    /* the whole datagram, the slot number in place of the protocol, and
     * the header kept for the packets that follow */
    cs = &v->tx[s];
    memcpy(cs->hdr, ip, hlen);
    cs->hlen = (uint16_t)hlen;
    touch(v, s);
    memcpy(out, ip, len);
    out[9] = (uint8_t)s;
    v->tx_last = s;
    *outlen = len;
    return PPP_PROTO_VJUC;
}

int ppp_vj_uncompress(ppp_vj_t *v, uint16_t proto, const uint8_t *in, size_t n,
                      uint8_t *out, size_t max)
{
    ppp_vj_slot_t *cs;
    const uint8_t *cp, *end = in + n;
    uint8_t *ip;
    unsigned changes;
    size_t ihl, hlen, data;
    int s;

    if (!v->rx_slots) return -1;
    if (proto == PPP_PROTO_VJUC) {
        /* the whole datagram with the slot in the protocol field: kept as
         * the slot's header, delivered with the protocol TCP again (which
         * makes its IP checksum right again too) */
        size_t thl;
        if (n < 40) { v->rx_toss = 1; return -1; }
        s = in[9];
        ihl = (size_t)(in[0] & 0x0f) * 4;
        if (s >= v->rx_slots || (in[0] >> 4) != 4 || ihl < 20 || g16(in + 2) != n || n < ihl + 20) { v->rx_toss = 1; return -1; }
        thl = (size_t)(in[ihl + 12] >> 4) * 4;
        hlen = ihl + thl;
        if (thl < 20 || hlen > n || hlen > PPP_VJ_HDR_MAX || n > max) { v->rx_toss = 1; return -1; }
        memcpy(out, in, n);
        out[9] = 6;
        cs = &v->rx[s];
        memcpy(cs->hdr, out, hlen);
        cs->hlen = (uint16_t)hlen;
        v->rx_last = s;
        v->rx_toss = 0;
        return (int)n;
    }
    if (proto != PPP_PROTO_VJC || n < 3) { v->rx_toss = 1; return -1; }
    cp = in;
    changes = *cp++;
    if (changes & VJ_C) {
        s = *cp++;
        if (s >= v->rx_slots) { v->rx_toss = 1; return -1; }
        v->rx_toss = 0;
        v->rx_last = s;
    } else {
        if (v->rx_toss) return -1;
        s = v->rx_last;
    }
    if (s < 0 || !v->rx[s].hlen) { v->rx_toss = 1; return -1; }
    cs = &v->rx[s];
    ip = cs->hdr;
    ihl = (size_t)(ip[0] & 0x0f) * 4;
    hlen = cs->hlen;

    if (end - cp < 2) goto bad;
    {
        uint8_t ck0 = cp[0], ck1 = cp[1];
        uint16_t olen = g16(ip + 2);
        uint8_t h[PPP_VJ_HDR_MAX];
        uint8_t *nth;
        cp += 2;
        memcpy(h, ip, hlen);
        nth = h + ihl;
        nth[16] = ck0; nth[17] = ck1;
        if (changes & VJ_P) nth[13] |= TH_PSH; else nth[13] &= (uint8_t)~TH_PSH;

/* a delta: one octet, or 0 and two */
#define DEC(dst) do { uint32_t x_; if (cp >= end) goto bad; x_ = *cp++; \
        if (x_ == 0) { if (end - cp < 2) goto bad; x_ = (uint32_t)cp[0] << 8 | cp[1]; cp += 2; } (dst) = x_; } while (0)

        switch (changes & VJ_SPECIALS) {
        case VJ_SPECIAL_I: {
            uint32_t i = (uint32_t)(olen - hlen);
            p32(nth + 8, g32(nth + 8) + i);
            p32(nth + 4, g32(nth + 4) + i);
            break; }
        case VJ_SPECIAL_D:
            p32(nth + 4, g32(nth + 4) + (uint32_t)(olen - hlen));
            break;
        default: {
            uint32_t x;
            if (changes & VJ_U) { nth[13] |= TH_URG; DEC(x); p16(nth + 18, (uint16_t)x); }
            else nth[13] &= (uint8_t)~TH_URG;
            if (changes & VJ_W) { DEC(x); p16(nth + 14, (uint16_t)(g16(nth + 14) + x)); }
            if (changes & VJ_A) { DEC(x); p32(nth + 8, g32(nth + 8) + x); }
            if (changes & VJ_S) { DEC(x); p32(nth + 4, g32(nth + 4) + x); }
            break; }
        }
        if (changes & VJ_I) { uint32_t x; DEC(x); p16(h + 4, (uint16_t)(g16(h + 4) + x)); }
        else p16(h + 4, (uint16_t)(g16(h + 4) + 1));
#undef DEC
        data = (size_t)(end - cp);
        if (hlen + data > 0xffff || hlen + data > max) goto bad;
        p16(h + 2, (uint16_t)(hlen + data));
        h[10] = h[11] = 0;
        p16(h + 10, ip_cksum(h, ihl));
        memcpy(cs->hdr, h, hlen);
        memcpy(out, h, hlen);
        memcpy(out + hlen, cp, data);
        return (int)(hlen + data);
    }
bad:
    v->rx_toss = 1;
    return -1;
}
