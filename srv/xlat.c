/* The callers' IPv4 over IPv6 (xlat.h). Ours; written for this project. */
#include "xlat.h"
#include "bbsdial.h"
#include <string.h>

#define MSS_MAX (XLAT_MTU6 - 40 - 20)       /* what a TCP segment may carry in 1280 octets of IPv6 */
#define LAN_MTU 1500

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---------------------------------------------------------- checksums */
static uint32_t sum(uint32_t s, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xffff) + (s >> 16); return (uint16_t)s; }
/* RFC 1624: a checksum after the octets of old (n) became those of new */
static uint16_t adjust(uint16_t ck, const uint8_t *old, const uint8_t *nw, size_t n)
{
    uint32_t s = (uint16_t)~ck;
    s += (uint16_t)~fold(sum(0, old, n));
    s += fold(sum(0, nw, n));
    return (uint16_t)~fold(s);
}

/* ------------------------------------------------------------ addresses */
static int plc(int len) { return len == 32 ? 0 : len == 40 ? 1 : len == 48 ? 2 : len == 56 ? 3 : len == 64 ? 4 : len == 96 ? 5 : -1; }
static const int AT[6][4] = { { 4, 5, 6, 7 }, { 5, 6, 7, 9 }, { 6, 7, 9, 10 }, { 7, 9, 10, 11 }, { 9, 10, 11, 12 }, { 12, 13, 14, 15 } };

int xlat_from_nat64(const uint8_t v6[16], const uint8_t *p, int len, uint32_t *v4)
{
    int k = plc(len);
    if (k < 0 || memcmp(v6, p, (size_t)len / 8) != 0) return 0;
    if (k < 5 && v6[8] != 0) return 0;          /* RFC 6052: octet 8 is zero */
    *v4 = (uint32_t)v6[AT[k][0]] << 24 | (uint32_t)v6[AT[k][1]] << 16 | (uint32_t)v6[AT[k][2]] << 8 | v6[AT[k][3]];
    return 1;
}

void xlat_to_nat64(uint8_t v6[16], const uint8_t *p, int len, uint32_t v4)
{
    int k = plc(len);
    memset(v6, 0, 16);
    if (k < 0) return;
    memcpy(v6, p, (size_t)len / 8);
    v6[AT[k][0]] = (uint8_t)(v4 >> 24); v6[AT[k][1]] = (uint8_t)(v4 >> 16); v6[AT[k][2]] = (uint8_t)(v4 >> 8); v6[AT[k][3]] = (uint8_t)v4;
}

void xlat_init(xlat_t *x, const uint8_t clat96[16])
{
    memset(x, 0, sizeof *x);
    memcpy(x->clat, clat96, 12);
}

void xlat_set_pref64(xlat_t *x, const uint8_t p[16], int len)
{
    x->pref64_len = plc(len) >= 0 ? len : 0;
    memset(x->pref64, 0, 16);
    if (x->pref64_len) memcpy(x->pref64, p, (size_t)x->pref64_len / 8);
}

int xlat_is_pool(uint32_t v4) { return (v4 >> (32 - XLAT_POOL_BITS)) == (XLAT_POOL_NET >> (32 - XLAT_POOL_BITS)); }

uint32_t xlat_pool_get(xlat_t *x, const uint8_t v6[16], uint32_t now)
{
    int i, old = 0;
    for (i = 0; i < x->n_pool; i++) {
        if (memcmp(x->pool[i].v6, v6, 16) == 0) { x->pool[i].used = now; return x->pool[i].v4; }
        if ((int32_t)(x->pool[i].used - x->pool[old].used) < 0) old = i;
    }
    if (x->n_pool < XLAT_POOL_MAX) {
        i = x->n_pool++;
        x->pool[i].v4 = XLAT_POOL_NET + 1 + x->next++;     /* 198.18.0.1 on; the pool is far larger than the table */
    } else i = old;                                         /* full: the least used gives its address up */
    memcpy(x->pool[i].v6, v6, 16);
    x->pool[i].used = now;
    return x->pool[i].v4;
}

const uint8_t *xlat_pool_find(xlat_t *x, uint32_t v4, uint32_t now)
{
    int i;
    for (i = 0; i < x->n_pool; i++)
        if (x->pool[i].v4 == v4) { x->pool[i].used = now; return x->pool[i].v6; }
    return NULL;
}

/* a site's IPv4 address as IPv6: a stand-in's own, or in the NAT64 prefix */
static int site_4to6(xlat_t *x, uint32_t v4, uint8_t v6[16], uint32_t now)
{
    if (xlat_is_pool(v4)) {
        const uint8_t *a = xlat_pool_find(x, v4, now);
        if (!a) return 0;
        memcpy(v6, a, 16);
        return 1;
    }
    /* through the NAT64 gateway only to the internet at large: not its own
     * network, loopback, link-local, CGNAT, multicast or reserved space
     * (the caller's own LAN, where there is IPv4, is reached over IPv4) */
    if (!x->pref64_len || !bbsdial_is_public(v4)) return 0;
    xlat_to_nat64(v6, x->pref64, x->pref64_len, v4);
    return 1;
}
/* a site's IPv6 address as IPv4: in the NAT64 prefix, or a stand-in's */
static int site_6to4(xlat_t *x, const uint8_t v6[16], uint32_t *v4, uint32_t now)
{
    int i;
    if (x->pref64_len && xlat_from_nat64(v6, x->pref64, x->pref64_len, v4)) return bbsdial_is_public(*v4);
    for (i = 0; i < x->n_pool; i++)
        if (memcmp(x->pool[i].v6, v6, 16) == 0) { x->pool[i].used = now; *v4 = x->pool[i].v4; return 1; }
    return 0;
}
static void caller_4to6(xlat_t *x, uint32_t v4, uint8_t v6[16]) { memcpy(v6, x->clat, 12); wr32(v6 + 12, v4); }
static int caller_6to4(xlat_t *x, const uint8_t v6[16], uint32_t *v4) { if (memcmp(v6, x->clat, 12)) return 0; *v4 = rd32(v6 + 12); return 1; }

/* -------------------------------------------------------------- TCP, UDP */
/* the TCP or UDP checksum at l4 + at, after the pseudo header's addresses
 * old (n octets) became new; UDP's 0 (none, IPv4 only) becomes a whole one */
static int fix_l4(uint8_t *l4, size_t len, int proto, const uint8_t *olda, size_t on, const uint8_t *newa, size_t nn,
                  const uint8_t *newsrc, const uint8_t *newdst, int to6, int whole)
{
    size_t at = proto == 6 ? 16 : 6;
    uint16_t ck;
    if (len < at + 2) return whole ? 0 : 1;       /* a later fragment holds no header */
    if (!whole) return 1;
    ck = rd16(l4 + at);
    if (proto == 17 && ck == 0) {
        uint32_t s;
        uint8_t ph[8];
        if (!to6) return 1;                       /* IPv4 may go without */
        /* compute it whole: the IPv6 pseudo header and all of the datagram */
        s = sum(0, newsrc, 16); s = sum(s, newdst, 16);
        wr32(ph, (uint32_t)len); ph[4] = ph[5] = ph[6] = 0; ph[7] = 17;
        s = sum(s, ph, 8);
        s = sum(s, l4, len);
        ck = (uint16_t)~fold(s);
        wr16(l4 + at, ck ? ck : 0xffff);
        return 1;
    }
    {   /* the addresses differ, the length and protocol do not */
        uint32_t s = (uint16_t)~ck;
        s += (uint16_t)~fold(sum(0, olda, on));
        s += fold(sum(0, newa, nn));
        ck = (uint16_t)~fold(s);
        if (proto == 17 && ck == 0) ck = 0xffff;
        wr16(l4 + at, ck);
    }
    return 1;
}

/* a SYN's MSS no larger than fits 1280 octets of IPv6 */
static void clamp_mss(uint8_t *tcp, size_t len)
{
    size_t off, i;
    if (len < 20 || !(tcp[13] & 0x02)) return;
    off = (size_t)(tcp[12] >> 4) * 4;
    if (off < 20 || off > len) return;
    for (i = 20; i + 1 < off; ) {
        if (tcp[i] == 0) break;
        if (tcp[i] == 1) { i++; continue; }
        if (tcp[i + 1] < 2 || i + tcp[i + 1] > off) break;
        if (tcp[i] == 2 && tcp[i + 1] == 4 && rd16(tcp + i + 2) > MSS_MAX) {
            uint8_t old[2] = { tcp[i + 2], tcp[i + 3] }, nw[2] = { MSS_MAX >> 8, MSS_MAX & 0xff };
            uint16_t ck = rd16(tcp + 16);
            /* the option sits at an even offset (i is, as TCP's options are
             * laid out by every stack we meet; odd: left alone) */
            if (i & 1) return;
            tcp[i + 2] = nw[0]; tcp[i + 3] = nw[1];
            wr16(tcp + 16, adjust(ck, old, nw, 2));
            return;
        }
        i += tcp[i + 1];
    }
}

/* -------------------------------------------------------------- 4 -> 6 */
static size_t icmp4_body_to6(xlat_t *x, const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t now);

/* the IPv6 header for an IPv4 one at p (ihl octets) and plen octets of
 * payload; returns its length (40, or 48 with a fragment header) or 0 */
static size_t head_4to6(const uint8_t *p, size_t ihl, size_t plen, int nh, int hlim,
                        const uint8_t src[16], const uint8_t dst[16], uint8_t *o)
{
    unsigned fo = rd16(p + 6);
    int mf = (fo & 0x2000) != 0, off = (int)(fo & 0x1fff);
    int frag = mf || off;
    (void)ihl;
    memset(o, 0, 40);
    o[0] = (uint8_t)(0x60 | p[1] >> 4); o[1] = (uint8_t)(p[1] << 4);
    wr16(o + 4, (unsigned)(plen + (frag ? 8 : 0)));
    o[6] = (uint8_t)(frag ? 44 : nh); o[7] = (uint8_t)hlim;
    memcpy(o + 8, src, 16); memcpy(o + 24, dst, 16);
    if (!frag) return 40;
    o[40] = (uint8_t)nh; o[41] = 0;
    wr16(o + 42, (unsigned)(off << 3 | mf));
    wr32(o + 44, rd16(p + 4));
    return 48;
}

/* an ICMPv4 error's type and code as ICMPv6, and its MTU; 0 = none such */
static int icmp_map_4to6(int t, int c, int *t6, int *c6, uint32_t *ptr)
{
    *ptr = 0;
    if (t == 8 || t == 0) { *t6 = t == 8 ? 128 : 129; *c6 = 0; return 1; }
    if (t == 11) { *t6 = 3; *c6 = c; return 1; }
    if (t != 3) return 0;
    switch (c) {
    case 0: case 1: case 5: case 6: case 7: case 8: case 11: case 12: *t6 = 1; *c6 = 0; return 1;
    case 9: case 10: case 13: case 15: *t6 = 1; *c6 = 1; return 1;
    case 3: *t6 = 1; *c6 = 4; return 1;
    case 2: *t6 = 4; *c6 = 1; *ptr = 6; return 1;      /* the next header field */
    case 4: *t6 = 2; *c6 = 0; return 1;
    default: return 0;
    }
}

int xlat_4to6(xlat_t *x, const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *out_n, uint32_t now)
{
    size_t ihl, tot, plen, h;
    uint8_t src[16], dst[16];
    unsigned fo;
    int proto, frag, first, df;
    *out_n = 0;
    if (cap < XLAT_MTU6 + 48 || n < 20 || (p[0] >> 4) != 4) return XLAT_DROP;
    ihl = (size_t)(p[0] & 15) * 4; tot = rd16(p + 2);
    if (ihl < 20 || tot < ihl || tot > n) return XLAT_DROP;
    if (fold(sum(0, p, ihl)) != 0xffff) return XLAT_DROP;           /* a damaged header */
    if (p[8] <= 1) return XLAT_DROP;                                /* no hops left */
    fo = rd16(p + 6); df = (fo & 0x4000) != 0;
    frag = (fo & 0x3fff) != 0; first = (fo & 0x1fff) == 0;
    proto = p[9]; plen = tot - ihl;
    caller_4to6(x, rd32(p + 12), src);
    if (!site_4to6(x, rd32(p + 16), dst, now)) return XLAT_DROP;
    if (40 + plen > XLAT_MTU6 && !frag) {
        if (df) {
            /* too big for IPv6's least: "fragmentation needed", MTU 1260,
             * back to the caller from where it was going */
            uint8_t *o = out;
            size_t q = ihl + 8 <= tot ? ihl + 8 : tot;
            memset(o, 0, 28);
            o[0] = 0x45; wr16(o + 2, (unsigned)(28 + q)); o[8] = 64; o[9] = 1;
            memcpy(o + 12, p + 16, 4); memcpy(o + 16, p + 12, 4);
            wr16(o + 10, (uint16_t)~fold(sum(0, o, 20)));
            o[20] = 3; o[21] = 4; wr16(o + 26, XLAT_MTU4);
            memcpy(o + 28, p, q);
            wr16(o + 22, (uint16_t)~fold(sum(0, o + 20, 8 + q)));
            *out_n = 28 + q;
            return XLAT_BACK;
        }
        if (40 + plen > LAN_MTU) return XLAT_DROP;      /* would not leave the box whole */
    }
    if (proto == 1) {
        size_t m;
        if (frag) return XLAT_DROP;                     /* ICMP in pieces: none ever needed here */
        h = head_4to6(p, ihl, 0, 58, p[8] - 1, src, dst, out);
        m = icmp4_body_to6(x, p + ihl, plen, out + h, cap - h, now);
        if (!m) return XLAT_DROP;
        wr16(out + 4, (unsigned)m);
        {   /* ICMPv6's checksum has the pseudo header */
            uint32_t s; uint8_t ph[8];
            wr32(ph, (uint32_t)m); ph[4] = ph[5] = ph[6] = 0; ph[7] = 58;
            out[h + 2] = out[h + 3] = 0;
            s = sum(sum(sum(0, out + 8, 32), ph, 8), out + h, m);
            wr16(out + h + 2, (uint16_t)~fold(s));
        }
        *out_n = h + m;
        return XLAT_OUT;
    }
    h = head_4to6(p, ihl, plen, proto, p[8] - 1, src, dst, out);
    if (h + plen > cap) return XLAT_DROP;
    memcpy(out + h, p + ihl, plen);
    if ((proto == 6 || proto == 17) && first) {
        if (proto == 6 && !frag) clamp_mss(out + h, plen);
        if (!fix_l4(out + h, plen, proto, p + 12, 8, out + 8, 32, out + 8, out + 24, 1, 1)) return XLAT_DROP;
        if (proto == 17 && frag && rd16(p + ihl + 6) == 0) return XLAT_DROP;    /* no checksum to make from a piece */
    }
    *out_n = h + plen;
    return XLAT_OUT;
}

/* an ICMPv4 message as ICMPv6 (checksum left to the caller); its length or 0 */
static size_t icmp4_body_to6(xlat_t *x, const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t now)
{
    int t6, c6; uint32_t ptr;
    if (n < 8 || !icmp_map_4to6(in[0], in[1], &t6, &c6, &ptr)) return 0;
    if (t6 >= 128) {                                    /* echo: id, sequence and data as they are */
        if (n > cap) return 0;
        memcpy(out, in, n);
        out[0] = (uint8_t)t6; out[1] = (uint8_t)c6;
        return n;
    }
    {   /* an error: the packet it quotes (sent to the caller) in IPv6 too */
        const uint8_t *q = in + 8;
        size_t qn = n - 8, qihl, qplen, h, room;
        uint8_t qs[16], qd[16];
        if (qn < 20 || (q[0] >> 4) != 4) return 0;
        qihl = (size_t)(q[0] & 15) * 4;
        if (qihl < 20 || qihl > qn) return 0;
        if (!site_4to6(x, rd32(q + 12), qs, now)) return 0;
        caller_4to6(x, rd32(q + 16), qd);
        memset(out, 0, 8);
        out[0] = (uint8_t)t6; out[1] = (uint8_t)c6;
        if (t6 == 2) {                                  /* Packet Too Big: the MTU, 20 more for IPv6 */
            uint32_t mtu = rd16(in + 6) + 20u;
            wr32(out + 4, mtu < XLAT_MTU6 ? XLAT_MTU6 : mtu);
        } else if (t6 == 4) wr32(out + 4, ptr);
        qplen = qn - qihl;
        h = head_4to6(q, qihl, rd16(q + 2) > qihl ? rd16(q + 2) - qihl : qplen, q[9] == 1 ? 58 : q[9], q[8], qs, qd, out + 8);
        room = XLAT_MTU6 - 40 - 8 - h;                  /* the whole error within 1280 */
        if (qplen > room) qplen = room;
        if (8 + h + qplen > cap) return 0;
        memcpy(out + 8 + h, q + qihl, qplen);
        return 8 + h + qplen;
    }
}

/* -------------------------------------------------------------- 6 -> 4 */
static size_t icmp6_body_to4(xlat_t *x, const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t now);

/* walk the extension headers: the upper protocol, where its header starts,
 * and the fragment header if there is one; -1 if it is none to translate */
static int walk6(const uint8_t *p, size_t n, size_t *l4, int *proto, const uint8_t **fh)
{
    int nh = p[6];
    size_t at = 40;
    *fh = NULL;
    for (;;) {
        if (nh == 0 || nh == 60) {                      /* hop-by-hop, destination options: skipped */
            if (at + 8 > n) return -1;
            nh = p[at]; at += (size_t)(p[at + 1] + 1) * 8;
        } else if (nh == 43) {                          /* routing: only one with no segments left */
            if (at + 8 > n || p[at + 3] != 0) return -1;
            nh = p[at]; at += (size_t)(p[at + 1] + 1) * 8;
        } else if (nh == 44) {
            if (at + 8 > n || *fh) return -1;
            *fh = p + at; nh = p[at]; at += 8;
        } else break;
        if (at > n) return -1;
    }
    *l4 = at; *proto = nh;
    return 0;
}

int xlat_6to4(xlat_t *x, const uint8_t *p, size_t n, uint8_t *out, size_t cap, size_t *out_n, uint32_t now)
{
    size_t plen, l4, len;
    const uint8_t *fh;
    uint32_t src, dst;
    int proto, first = 1, mf = 0, off = 0;
    uint8_t *o = out;
    *out_n = 0;
    if (n < 40 || (p[0] >> 4) != 6 || cap < LAN_MTU) return XLAT_DROP;
    plen = rd16(p + 4);
    if (40 + plen > n) return XLAT_DROP;
    n = 40 + plen;
    if (p[7] <= 1) return XLAT_DROP;
    if (!caller_6to4(x, p + 24, &dst)) return XLAT_DROP;
    if (!site_6to4(x, p + 8, &src, now)) return XLAT_DROP;
    if (walk6(p, n, &l4, &proto, &fh) < 0) return XLAT_DROP;
    if (fh) { unsigned f = rd16(fh + 2); off = (int)(f >> 3); mf = f & 1; first = off == 0; }
    len = n - l4;
    if (proto == 58) {
        size_t m;
        if (fh) return XLAT_DROP;
        m = icmp6_body_to4(x, p + l4, len, o + 20, cap - 20, now);
        if (!m) return XLAT_DROP;
        proto = 1; len = m;
    } else {
        if (20 + len > cap) return XLAT_DROP;
        memcpy(o + 20, p + l4, len);
    }
    memset(o, 0, 20);
    o[0] = 0x45; o[1] = (uint8_t)(rd16(p) >> 4);
    wr16(o + 2, (unsigned)(20 + len));
    if (fh) { wr16(o + 4, rd16(fh + 6)); wr16(o + 6, (unsigned)((mf ? 0x2000 : 0) | off)); }
    else wr16(o + 6, 0x4000);                           /* DF: the IPv6 side does no fragmenting */
    o[8] = (uint8_t)(p[7] - 1); o[9] = (uint8_t)proto;
    wr32(o + 12, src); wr32(o + 16, dst);
    wr16(o + 10, (uint16_t)~fold(sum(0, o, 20)));
    if ((proto == 6 || proto == 17) && first) {
        if (!fix_l4(o + 20, len, proto, p + 8, 32, o + 12, 8, NULL, NULL, 0, 1)) return XLAT_DROP;
    } else if (proto == 1) {
        o[22] = o[23] = 0;
        wr16(o + 22, (uint16_t)~fold(sum(0, o + 20, len)));
    }
    *out_n = 20 + len;
    return XLAT_OUT;
}

static int icmp_map_6to4(int t, int c, int *t4, int *c4)
{
    if (t == 128 || t == 129) { *t4 = t == 128 ? 8 : 0; *c4 = 0; return 1; }
    if (t == 3) { *t4 = 11; *c4 = c; return 1; }
    if (t == 2) { *t4 = 3; *c4 = 4; return 1; }
    if (t == 4 && c == 1) { *t4 = 3; *c4 = 2; return 1; }
    if (t != 1) return 0;
    *t4 = 3;
    switch (c) {
    case 0: case 2: case 3: *c4 = 1; return 1;
    case 1: *c4 = 10; return 1;
    case 4: *c4 = 3; return 1;
    default: return 0;
    }
}

/* an ICMPv6 message as ICMPv4 (checksum left to the caller); its length or 0 */
static size_t icmp6_body_to4(xlat_t *x, const uint8_t *in, size_t n, uint8_t *out, size_t cap, uint32_t now)
{
    int t4, c4;
    if (n < 8 || !icmp_map_6to4(in[0], in[1], &t4, &c4)) return 0;
    if (t4 == 8 || t4 == 0) {
        if (n > cap) return 0;
        memcpy(out, in, n);
        out[0] = (uint8_t)t4; out[1] = (uint8_t)c4;
        return n;
    }
    {   /* an error: the packet it quotes (the caller's, gone out as IPv6) back in IPv4 */
        const uint8_t *q = in + 8, *fh;
        size_t qn = n - 8, l4, ql;
        uint32_t qs, qd;
        int qp;
        uint8_t *qo = out + 8;
        if (qn < 40 || (q[0] >> 4) != 6) return 0;
        if (!caller_6to4(x, q + 8, &qs) || !site_6to4(x, q + 24, &qd, now)) return 0;
        if (walk6(q, qn, &l4, &qp, &fh) < 0) return 0;
        ql = qn - l4;
        if (8 + 20 + ql > cap) ql = cap - 28;
        if (ql > 548) ql = 548;                         /* an ICMPv4 error stays within 576 */
        memset(out, 0, 8);
        out[0] = (uint8_t)t4; out[1] = (uint8_t)c4;
        if (t4 == 3 && c4 == 4) {
            uint32_t mtu = rd32(in + 4);
            mtu = mtu > 20 + 68 ? mtu - 20 : 68;
            wr16(out + 6, mtu > 0xffff ? 0xffff : mtu);
        }
        memset(qo, 0, 20);
        qo[0] = 0x45; wr16(qo + 2, (unsigned)(20 + ((size_t)rd16(q + 4) + 40 > l4 ? (size_t)rd16(q + 4) + 40 - l4 : ql)));
        qo[8] = q[7]; qo[9] = (uint8_t)(qp == 58 ? 1 : qp);
        wr32(qo + 12, qs); wr32(qo + 16, qd);
        wr16(qo + 10, (uint16_t)~fold(sum(0, qo, 20)));
        memcpy(qo + 20, q + l4, ql);
        return 8 + 20 + ql;
    }
}
