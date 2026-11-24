/* What the router says (srv/ra6.h): the name servers (RDNSS) and the NAT64
 * prefix (PREF64) of a router advertisement. The socket is in ns_call (19). */
#include "../ra6.h"
#include "../../test/test.h"
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static size_t ra_head(uint8_t *m)
{
    memset(m, 0, 16);
    m[0] = 134; m[4] = 64; m[6] = 0x07; m[7] = 0x08;        /* hop limit 64, router lifetime 1800 */
    return 16;
}
static size_t rdnss(uint8_t *m, uint32_t life, const char *a1, const char *a2)
{
    size_t n = a2 ? 40 : 24;
    memset(m, 0, n);
    m[0] = 25; m[1] = (uint8_t)(n / 8);
    m[4] = (uint8_t)(life >> 24); m[5] = (uint8_t)(life >> 16); m[6] = (uint8_t)(life >> 8); m[7] = (uint8_t)life;
    inet_pton(AF_INET6, a1, m + 8);
    if (a2) inet_pton(AF_INET6, a2, m + 24);
    return n;
}
static size_t pref64(uint8_t *m, unsigned life_s, unsigned plc, const char *p)
{
    uint8_t a[16];
    unsigned sl = (life_s / 8) << 3 | plc;
    memset(m, 0, 16);
    m[0] = 38; m[1] = 2; m[2] = (uint8_t)(sl >> 8); m[3] = (uint8_t)sl;
    inet_pton(AF_INET6, p, a);
    memcpy(m + 4, a, 12);
    return 16;
}
static int is(const uint8_t *a, const char *txt) { uint8_t b[16]; inet_pton(AF_INET6, txt, b); return memcmp(a, b, 16) == 0; }

int main(void)
{
    uint8_t m[400];
    size_t n;
    ra6_info in;

    /* name servers and the NAT64 prefix */
    memset(&in, 0, sizeof in);
    n = ra_head(m);
    n += rdnss(m + n, 600, "2001:db8::53", "2001:db8::54");
    n += pref64(m + n, 1800, 0, "64:ff9b::");
    CHECK_EQ_I(ra6_parse(m, n, 1000, &in), 0);
    CHECK_EQ_I(in.n_dns, 2);
    CHECK(is(in.dns[0], "2001:db8::53") && is(in.dns[1], "2001:db8::54"));
    CHECK_EQ_I((int)in.dns_until[0], 1600);
    CHECK_EQ_I(in.pref64_len, 96);
    CHECK(is(in.pref64, "64:ff9b::"));
    CHECK_EQ_I((int)in.pref64_until, 2800);
    CHECK_EQ_I((int)in.heard, 1000);

    /* the next advertisement's list replaces the last; the prefix stays
     * when it is not given again */
    n = ra_head(m);
    n += rdnss(m + n, 300, "2001:db8::55", NULL);
    CHECK_EQ_I(ra6_parse(m, n, 1100, &in), 0);
    CHECK_EQ_I(in.n_dns, 1);
    CHECK(is(in.dns[0], "2001:db8::55"));
    CHECK_EQ_I(in.pref64_len, 96);
    /* a lifetime of 0 withdraws */
    n = ra_head(m);
    n += rdnss(m + n, 0, "2001:db8::55", NULL);
    n += pref64(m + n, 0, 0, "64:ff9b::");
    CHECK_EQ_I(ra6_parse(m, n, 1200, &in), 0);
    CHECK_EQ_I(in.n_dns, 0);
    CHECK_EQ_I(in.pref64_len, 0);

    /* run out */
    n = ra_head(m);
    n += rdnss(m + n, 60, "2001:db8::53", NULL);
    n += pref64(m + n, 80, 0, "2001:db8:64::");
    CHECK_EQ_I(ra6_parse(m, n, 2000, &in), 0);
    ra6_expire(&in, 2059);
    CHECK_EQ_I(in.n_dns, 1); CHECK_EQ_I(in.pref64_len, 96);
    ra6_expire(&in, 2060);
    CHECK_EQ_I(in.n_dns, 0); CHECK_EQ_I(in.pref64_len, 96);
    ra6_expire(&in, 2080);
    CHECK_EQ_I(in.pref64_len, 0);

    /* a shorter prefix: /64, the bits past it are not taken */
    n = ra_head(m);
    n += pref64(m + n, 600, 1, "2001:db8:1:2:ffff:ffff::");
    CHECK_EQ_I(ra6_parse(m, n, 3000, &in), 0);
    CHECK_EQ_I(in.pref64_len, 64);
    CHECK(is(in.pref64, "2001:db8:1:2::"));
    n = ra_head(m); n += pref64(m + n, 600, 5, "2001:db8:ffff::");     /* /32 */
    CHECK_EQ_I(ra6_parse(m, n, 3000, &in), 0);
    CHECK_EQ_I(in.pref64_len, 32); CHECK(is(in.pref64, "2001:db8::"));
    n = ra_head(m); n += pref64(m + n, 600, 6, "2001:db8::");          /* PLC 6: none such, ignored */
    CHECK_EQ_I(ra6_parse(m, n, 3000, &in), 0);
    CHECK_EQ_I(in.pref64_len, 32);

    /* at most RA6_DNS_MAX */
    memset(&in, 0, sizeof in);
    n = ra_head(m);
    n += rdnss(m + n, 600, "2001:db8::1", "2001:db8::2");
    n += rdnss(m + n, 600, "2001:db8::3", "2001:db8::4");
    CHECK_EQ_I(ra6_parse(m, n, 1, &in), 0);
    CHECK_EQ_I(in.n_dns, RA6_DNS_MAX);

    /* not an advertisement, or not a well-formed one: nothing taken */
    memset(&in, 0, sizeof in);
    n = ra_head(m); n += rdnss(m + n, 600, "2001:db8::53", NULL);
    m[0] = 133; CHECK_EQ_I(ra6_parse(m, n, 1, &in), -1); m[0] = 134;     /* a solicitation */
    m[1] = 1; CHECK_EQ_I(ra6_parse(m, n, 1, &in), -1); m[1] = 0;
    CHECK_EQ_I(ra6_parse(m, 15, 1, &in), -1);
    m[17] = 0; CHECK_EQ_I(ra6_parse(m, n, 1, &in), -1);                 /* an option of length 0 */
    m[17] = 9; CHECK_EQ_I(ra6_parse(m, n, 1, &in), -1);                 /* longer than the message */
    m[17] = 3;
    CHECK_EQ_I(in.n_dns, 0); CHECK_EQ_I((int)in.heard, 0);
    CHECK_EQ_I(ra6_parse(m, n - 8, 1, &in), -1);                        /* cut in the option */
    {   /* RDNSS too short to hold an address: ignored, the rest taken */
        n = ra_head(m);
        m[n] = 25; m[n + 1] = 1; n += 8;
        n += pref64(m + n, 600, 0, "64:ff9b::");
        CHECK_EQ_I(ra6_parse(m, n, 1, &in), 0);
        CHECK_EQ_I(in.n_dns, 0); CHECK_EQ_I(in.pref64_len, 96);
    }
    {   /* unknown options are stepped over */
        n = ra_head(m);
        memset(m + n, 0, 8); m[n] = 1; m[n + 1] = 1; n += 8;           /* source link-layer address */
        n += rdnss(m + n, 600, "2001:db8::9", NULL);
        CHECK_EQ_I(ra6_parse(m, n, 5, &in), 0);
        CHECK_EQ_I(in.n_dns, 1); CHECK(is(in.dns[0], "2001:db8::9"));
    }
    T_REPORT("ra6");
}
