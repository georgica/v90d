/* The callers' IPv4 over IPv6 (srv/xlat.h): RFC 7915 translation both ways,
 * every checksum checked whole against its pseudo header. */
#include "../xlat.h"
#include "../../test/test.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static uint32_t sum(uint32_t s, const uint8_t *p, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n; i += 2) s += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (uint32_t)p[n - 1] << 8;
    return s;
}
static uint16_t fold(uint32_t s) { while (s >> 16) s = (s & 0xffff) + (s >> 16); return (uint16_t)s; }
static uint32_t ip4(const char *t) { struct in_addr a; inet_pton(AF_INET, t, &a); return ntohl(a.s_addr); }
static void ip6(uint8_t *a, const char *t) { inet_pton(AF_INET6, t, a); }
static int is6(const uint8_t *a, const char *t) { uint8_t b[16]; inet_pton(AF_INET6, t, b); return memcmp(a, b, 16) == 0; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned rd16(const uint8_t *p) { return (unsigned)(p[0] << 8 | p[1]); }

/* is the upper-layer checksum of this IPv6 packet right? (no extension headers but a fragment one) */
static int ok6(const uint8_t *p, size_t n)
{
    if (n < 48) return 0;
    size_t l4 = p[6] == 44 ? 48 : 40;
    int nh = p[6] == 44 ? p[40] : p[6];
    uint8_t ph[8];
    size_t len = n - l4;
    wr32(ph, (uint32_t)len); ph[4] = ph[5] = ph[6] = 0; ph[7] = (uint8_t)nh;
    return fold(sum(sum(sum(0, p + 8, 32), ph, 8), p + l4, len)) == 0xffff;
}
/* is this IPv4 packet's header and upper checksum right? */
static int ok4(const uint8_t *p, size_t n)
{
    size_t ihl = (size_t)(p[0] & 15) * 4, len = n - ihl;
    if (n < 20 || n < ihl) return 0;
    uint8_t ph[4] = { 0, p[9], (uint8_t)(len >> 8), (uint8_t)len };
    if (fold(sum(0, p, ihl)) != 0xffff) return 0;
    if (p[9] == 1) return fold(sum(0, p + ihl, len)) == 0xffff;
    if (p[9] == 17 && rd16(p + ihl + 6) == 0) return 1;
    return fold(sum(sum(sum(0, p + 12, 8), ph, 4), p + ihl, len)) == 0xffff;
}
/* an IPv4 packet: proto, payload; returns its length */
static size_t pkt4(uint8_t *p, uint32_t s, uint32_t d, int proto, const uint8_t *pl, size_t n, int df)
{
    memset(p, 0, 20);
    p[0] = 0x45; p[1] = 0x28; p[2] = (uint8_t)((20 + n) >> 8); p[3] = (uint8_t)(20 + n);
    p[4] = 0x12; p[5] = 0x34; p[6] = df ? 0x40 : 0; p[8] = 64; p[9] = (uint8_t)proto;
    wr32(p + 12, s); wr32(p + 16, d);
    memcpy(p + 20, pl, n);
    { uint16_t c = (uint16_t)~fold(sum(0, p, 20)); p[10] = (uint8_t)(c >> 8); p[11] = (uint8_t)c; }
    if (proto == 1) { uint16_t c; p[22] = p[23] = 0; c = (uint16_t)~fold(sum(0, p + 20, n)); p[22] = (uint8_t)(c >> 8); p[23] = (uint8_t)c; }
    if (proto == 6 || proto == 17) {
        size_t at = proto == 6 ? 16 : 6;
        uint8_t ph[4] = { 0, (uint8_t)proto, (uint8_t)(n >> 8), (uint8_t)n };
        uint16_t c;
        if (proto == 17 && pl[6] == 0 && pl[7] == 0 && pl[0] == 0xee) return 20 + n;   /* a UDP datagram with no checksum */
        p[20 + at] = p[20 + at + 1] = 0;
        c = (uint16_t)~fold(sum(sum(sum(0, p + 12, 8), ph, 4), p + 20, n));
        if (proto == 17 && c == 0) c = 0xffff;
        p[20 + at] = (uint8_t)(c >> 8); p[20 + at + 1] = (uint8_t)c;
    }
    return 20 + n;
}
/* an IPv6 packet, the checksum made */
static size_t pkt6(uint8_t *p, const char *s, const char *d, int nh, const uint8_t *pl, size_t n)
{
    memset(p, 0, 40);
    p[0] = 0x60; p[4] = (uint8_t)(n >> 8); p[5] = (uint8_t)n; p[6] = (uint8_t)nh; p[7] = 60;
    ip6(p + 8, s); ip6(p + 24, d);
    memcpy(p + 40, pl, n);
    {
        size_t at = nh == 6 ? 16 : nh == 17 ? 6 : 2;
        uint8_t ph[8]; uint16_t c;
        p[40 + at] = p[40 + at + 1] = 0;
        wr32(ph, (uint32_t)n); ph[4] = ph[5] = ph[6] = 0; ph[7] = (uint8_t)nh;
        c = (uint16_t)~fold(sum(sum(sum(0, p + 8, 32), ph, 8), p + 40, n));
        p[40 + at] = (uint8_t)(c >> 8); p[40 + at + 1] = (uint8_t)c;
    }
    return 40 + n;
}

int main(void)
{
    static xlat_t x;
    static uint8_t in[2000], out[2000], back[2000];
    uint8_t clat[16], pf[16], a6[16];
    size_t n, m, k;
    uint32_t caller = ip4("10.64.64.2"), site = ip4("93.184.216.34"), v4;
    int r, i;

    ip6(clat, "fd90:d::");
    xlat_init(&x, clat);

    /* NAT64 addresses at every length (RFC 6052 2.4's examples) */
    {
        static const struct { const char *pre; int len; const char *want; } t[] = {
            { "2001:db8::", 32, "2001:db8:c000:221::" }, { "2001:db8:100::", 40, "2001:db8:1c0:2:21::" },
            { "2001:db8:122::", 48, "2001:db8:122:c000:2:2100::" }, { "2001:db8:122:300::", 56, "2001:db8:122:3c0:0:221::" },
            { "2001:db8:122:344::", 64, "2001:db8:122:344:c0:2:2100:0" }, { "2001:db8:122:344::", 96, "2001:db8:122:344::192.0.2.33" },
        };
        for (i = 0; i < 6; i++) {
            ip6(pf, t[i].pre);
            xlat_to_nat64(a6, pf, t[i].len, ip4("192.0.2.33"));
            if (!is6(a6, t[i].want)) { char b[64]; inet_ntop(AF_INET6, a6, b, sizeof b); printf("  /%d: %s\n", t[i].len, b); }
            CHECK(is6(a6, t[i].want));
            CHECK(xlat_from_nat64(a6, pf, t[i].len, &v4) && v4 == ip4("192.0.2.33"));
        }
        ip6(pf, "2001:db8:122:344::");
        ip6(a6, "2001:db8:122:344:ffc0:2:2100:0");          /* octet 8 not zero: none */
        CHECK(!xlat_from_nat64(a6, pf, 64, &v4));
        ip6(a6, "2001:db8:122:345::1");
        CHECK(!xlat_from_nat64(a6, pf, 96, &v4));
    }

    /* without a NAT64 prefix nothing but stand-ins goes */
    {
        static const uint8_t u[12] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'h', 'e', 'y', '!' };
        n = pkt4(in, caller, site, 17, u, sizeof u, 0);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_DROP);
    }
    ip6(pf, "64:ff9b::");
    xlat_set_pref64(&x, pf, 96);

    /* UDP out and its answer back */
    {
        static const uint8_t u[12] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'h', 'e', 'y', '!' };
        n = pkt4(in, caller, site, 17, u, sizeof u, 1);
        r = xlat_4to6(&x, in, n, out, sizeof out, &m, 1);
        CHECK_EQ_I(r, XLAT_OUT);
        CHECK_EQ_I((int)m, 40 + 12);
        CHECK(is6(out + 8, "fd90:d::10.64.64.2") && is6(out + 24, "64:ff9b::93.184.216.34"));
        CHECK_EQ_I(out[6], 17); CHECK_EQ_I(out[7], 63);                   /* TTL 64, one hop */
        CHECK_EQ_I(out[0], 0x62); CHECK_EQ_I(out[1], 0x80);               /* traffic class 0x28 */
        CHECK(ok6(out, m));
        CHECK(memcmp(out + 48, "hey!", 4) == 0);
        /* the answer: from the site's NAT64 address to the caller's */
        {
            static const uint8_t ua[12] = { 0x00, 0x35, 0x04, 0x00, 0x00, 0x0c, 0, 0, 'y', 'o', 'u', '!' };
            n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 17, ua, sizeof ua);
            r = xlat_6to4(&x, in, n, back, sizeof back, &k, 1);
            CHECK_EQ_I(r, XLAT_OUT);
            CHECK_EQ_I((int)k, 32);
            CHECK(rd32(back + 12) == site && rd32(back + 16) == caller);
            CHECK_EQ_I(back[8], 59); CHECK_EQ_I(back[9], 17);
            CHECK(rd16(back + 6) == 0x4000);                             /* DF */
            CHECK(ok4(back, k));
        }
    }
    /* UDP without a checksum (IPv4 allows it): one is made for IPv6 */
    {
        static const uint8_t u[12] = { 0xee, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'n', 'o', 'c', 'k' };
        n = pkt4(in, caller, site, 17, u, sizeof u, 0);
        CHECK(in[26] == 0 && in[27] == 0);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK(rd16(out + 46) != 0);
        CHECK(ok6(out, m));
    }
    /* a TCP SYN: its MSS brought down to what fits 1280, the checksum still right */
    {
        uint8_t t[24];
        memset(t, 0, sizeof t);
        t[0] = 0x04; t[1] = 0x01; t[2] = 0; t[3] = 80; t[7] = 1; t[12] = 6 << 4; t[13] = 0x02; t[14] = 0xff; t[15] = 0xff;
        t[20] = 2; t[21] = 4; t[22] = 1460 >> 8; t[23] = 1460 & 0xff;
        n = pkt4(in, caller, site, 6, t, sizeof t, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK_EQ_I((int)rd16(out + 40 + 22), 1220);
        CHECK(ok6(out, m));
        t[22] = 536 >> 8; t[23] = 536 & 0xff;                              /* smaller: as it is */
        n = pkt4(in, caller, site, 6, t, sizeof t, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK_EQ_I((int)rd16(out + 40 + 22), 536);
        CHECK(ok6(out, m));
        /* and back: an answer's checksum */
        n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 6, t, sizeof t);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_OUT);
        CHECK(ok4(back, k));
    }
    /* ping, and its answer */
    {
        uint8_t e[40];
        memset(e, 0x5a, sizeof e);
        e[0] = 8; e[1] = 0; e[4] = 0x12; e[5] = 0x34; e[6] = 0; e[7] = 1;
        n = pkt4(in, caller, site, 1, e, sizeof e, 0);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK_EQ_I(out[6], 58); CHECK_EQ_I(out[40], 128);
        CHECK(ok6(out, m));
        CHECK(rd16(out + 44) == 0x1234);
        e[0] = 129;
        n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 58, e, sizeof e);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_OUT);
        CHECK_EQ_I(back[9], 1); CHECK_EQ_I(back[20], 0);
        CHECK(ok4(back, k));
    }
    /* too big for 1280 with DF: "fragmentation needed", MTU 1260, back to the caller */
    {
        static uint8_t big[1400];
        memset(big, 1, sizeof big);
        big[0] = 0x04; big[2] = 0; big[3] = 53; big[4] = (uint8_t)(sizeof big >> 8); big[5] = (uint8_t)sizeof big;
        n = pkt4(in, caller, site, 17, big, sizeof big, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_BACK);
        CHECK(rd32(out + 12) == site && rd32(out + 16) == caller);
        CHECK_EQ_I(out[20], 3); CHECK_EQ_I(out[21], 4); CHECK_EQ_I((int)rd16(out + 26), 1260);
        CHECK(ok4(out, m));
        CHECK(memcmp(out + 28, in, 28) == 0);                              /* the header and 8 octets */
        /* without DF and within the LAN's 1500: it goes */
        n = pkt4(in, caller, site, 17, big, sizeof big, 0);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK(ok6(out, m));
    }
    /* a fragment: a fragment header with the id and offset */
    {
        uint8_t f[16];
        memset(f, 7, sizeof f);
        n = pkt4(in, caller, site, 17, f, sizeof f, 0);
        in[6] = 0x00; in[7] = 0x10;                                       /* offset 128, last piece */
        in[10] = in[11] = 0;
        { uint16_t c = (uint16_t)~fold(sum(0, in, 20)); in[10] = (uint8_t)(c >> 8); in[11] = (uint8_t)c; }
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);
        CHECK_EQ_I(out[6], 44); CHECK_EQ_I(out[40], 17);
        CHECK_EQ_I((int)rd16(out + 42), 16 << 3);
        CHECK(rd32(out + 44) == 0x1234);
        CHECK(memcmp(out + 48, in + 20, sizeof f) == 0);                   /* not touched */
    }
    /* an error from the far side: Packet Too Big, quoting what the caller sent */
    {
        static const uint8_t u[12] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'h', 'e', 'y', '!' };
        uint8_t err[8 + 52];
        n = pkt4(in, caller, site, 17, u, sizeof u, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_OUT);  /* what went out */
        memset(err, 0, 8); err[0] = 2; wr32(err + 4, 1400);
        memcpy(err + 8, out, 52);
        n = pkt6(in, "2001:db8::1", "fd90:d::10.64.64.2", 58, err, sizeof err);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);   /* from a router not in the prefix */
        n = pkt6(in, "64:ff9b::8.8.4.4", "fd90:d::10.64.64.2", 58, err, sizeof err);    /* a router on the way */
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_OUT);
        CHECK_EQ_I(back[20], 3); CHECK_EQ_I(back[21], 4); CHECK_EQ_I((int)rd16(back + 26), 1380);
        CHECK(ok4(back, k));
        CHECK(rd32(back + 28 + 12) == caller && rd32(back + 28 + 16) == site);  /* the quoted packet, in IPv4 */
        CHECK_EQ_I(back[28 + 9], 17);
        /* port unreachable */
        err[0] = 1; err[1] = 4; memset(err + 4, 0, 4);
        n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 58, err, sizeof err);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_OUT);
        CHECK_EQ_I(back[20], 3); CHECK_EQ_I(back[21], 3);
        CHECK(ok4(back, k));
    }
    /* stand-ins for a site that has IPv6 alone */
    {
        static const uint8_t u[12] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'h', 'e', 'y', '!' };
        uint32_t s1, s2;
        ip6(a6, "2001:db8:5::80");
        s1 = xlat_pool_get(&x, a6, 10);
        CHECK(xlat_is_pool(s1));
        CHECK_EQ_I((long)xlat_pool_get(&x, a6, 11), (long)s1);           /* the same again */
        ip6(a6, "2001:db8:5::81");
        s2 = xlat_pool_get(&x, a6, 12);
        CHECK(s2 != s1 && xlat_is_pool(s2));
        n = pkt4(in, caller, s1, 17, u, sizeof u, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 13), XLAT_OUT);
        CHECK(is6(out + 24, "2001:db8:5::80"));
        CHECK(ok6(out, m));
        n = pkt6(in, "2001:db8:5::80", "fd90:d::10.64.64.2", 17, u, sizeof u);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 14), XLAT_OUT);
        CHECK((long)rd32(back + 12) == (long)s1);
        CHECK(ok4(back, k));
        n = pkt4(in, caller, s2 + 100, 17, u, sizeof u, 1);              /* a stand-in never handed out */
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 15), XLAT_DROP);
        /* full: the least used gives its address up */
        for (i = 0; i < XLAT_POOL_MAX; i++) { ip6(a6, "2001:db8:6::"); a6[14] = (uint8_t)(i >> 8); a6[15] = (uint8_t)i; xlat_pool_get(&x, a6, 100 + (uint32_t)i); }
        CHECK_EQ_I(x.n_pool, XLAT_POOL_MAX);
        ip6(a6, "2001:db8:5::81");                                         /* s2 went (used at 12) */
        CHECK(xlat_pool_find(&x, s2, 2000) == NULL || memcmp(xlat_pool_find(&x, s2, 2000), a6, 16) != 0);
        /* every stand-in used within the hold: none is taken from another
         * address, the new one gets none */
        {
            static xlat_t y; uint32_t v;
            xlat_init(&y, clat);
            for (i = 0; i < XLAT_POOL_MAX; i++) { ip6(a6, "2001:db8:7::"); a6[14] = (uint8_t)(i >> 8); a6[15] = (uint8_t)i; xlat_pool_get(&y, a6, 5000); }
            ip6(a6, "2001:db8:8::1");
            CHECK_EQ_I((long)xlat_pool_get(&y, a6, 5000 + XLAT_HOLD_S - 1), 0);
            v = xlat_pool_get(&y, a6, 5000 + XLAT_HOLD_S);                   /* the hold is over */
            CHECK(xlat_is_pool(v));
            /* a flow keeps its stand-in fresh: in use, it is never the one given up */
            ip6(a6, "2001:db8:7::"); a6[15] = 5;
            v = xlat_pool_get(&y, a6, 9000);                                 /* refreshed at 9000 */
            for (i = 0; i < XLAT_POOL_MAX - 1; i++) { uint8_t b6[16]; ip6(b6, "2001:db8:9::"); b6[14] = (uint8_t)(i >> 8); b6[15] = (uint8_t)i; xlat_pool_get(&y, b6, 9000 + XLAT_HOLD_S); }
            CHECK(xlat_pool_find(&y, v, 9001 + XLAT_HOLD_S) != NULL && memcmp(xlat_pool_find(&y, v, 9001 + XLAT_HOLD_S), a6, 16) == 0);
        }
    }
    /* what is not translated */
    {
        static const uint8_t u[12] = { 0x04, 0x00, 0x00, 0x35, 0x00, 0x0c, 0, 0, 'h', 'e', 'y', '!' };
        n = pkt4(in, caller, site, 17, u, sizeof u, 1);
        in[8] = 1; in[10] = in[11] = 0;
        { uint16_t c = (uint16_t)~fold(sum(0, in, 20)); in[10] = (uint8_t)(c >> 8); in[11] = (uint8_t)c; }
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_DROP);       /* no hops left */
        n = pkt4(in, caller, site, 17, u, sizeof u, 1);
        in[11] ^= 1;
        CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_DROP);       /* a damaged header */
        n = pkt4(in, caller, site, 17, u, sizeof u, 1);
        CHECK_EQ_I(xlat_4to6(&x, in, n - 1, out, sizeof out, &m, 1), XLAT_DROP);   /* cut short */
        n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:e::10.64.64.2", 17, u, sizeof u);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);     /* not to a caller */
        n = pkt6(in, "2001:db8:7::1", "fd90:d::10.64.64.2", 17, u, sizeof u);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);     /* from nowhere we know */
        {
            uint8_t ns[24]; memset(ns, 0, sizeof ns); ns[0] = 135;
            n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 58, ns, sizeof ns);
            CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP); /* neighbour discovery */
        }
        n = pkt6(in, "64:ff9b::93.184.216.34", "fd90:d::10.64.64.2", 17, u, sizeof u);
        in[7] = 1;
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);     /* hop limit 1 */
        n = pkt6(in, "64:ff9b::198.18.0.1", "fd90:d::10.64.64.2", 17, u, sizeof u);
        CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);     /* a stand-in through NAT64: no */
        /* NAT64 only to the internet at large, either way */
        {
            static const char *const not_out[] = { "10.0.0.1", "192.168.1.1", "172.16.5.5", "127.0.0.1", "169.254.1.1",
                                                    "100.64.0.1", "224.0.0.251", "255.255.255.255", "0.0.0.0", "192.0.2.1" };
            for (i = 0; i < 10; i++) {
                n = pkt4(in, caller, ip4(not_out[i]), 17, u, sizeof u, 1);
                if (xlat_4to6(&x, in, n, out, sizeof out, &m, 1) != XLAT_DROP) printf("  %s went out\n", not_out[i]);
                CHECK_EQ_I(xlat_4to6(&x, in, n, out, sizeof out, &m, 1), XLAT_DROP);
            }
            n = pkt6(in, "64:ff9b::192.168.1.1", "fd90:d::10.64.64.2", 17, u, sizeof u);
            CHECK_EQ_I(xlat_6to4(&x, in, n, back, sizeof back, &k, 1), XLAT_DROP);
        }
    }
    T_REPORT("xlat");
}
