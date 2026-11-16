/* Tests for the synthetic DNS: the name table (stable addresses, least
 * recently used replacement, no address handed to two live names) and the
 * packet responder (A, other types, malformed input). */
#include <string.h>
#include <stdint.h>
#include "test.h"
#include "dns.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))
#define CHECK_EQ_S(got, want) do { if (strcmp((got), (want)) != 0) printf("    got:  %s\n    want: %s\n", (got), (want)); CHECK(strcmp((got), (want)) == 0); } while (0)

static dns_t d;

/* build a query: id, flags, one question */
static size_t mkq(uint8_t *p, unsigned id, unsigned flags, const char *name, unsigned qtype, unsigned qclass)
{
    size_t n = 12;
    memset(p, 0, 12);
    p[0] = (uint8_t)(id >> 8); p[1] = (uint8_t)id;
    p[2] = (uint8_t)(flags >> 8); p[3] = (uint8_t)flags;
    p[5] = 1;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t l = dot ? (size_t)(dot - name) : strlen(name);
        p[n++] = (uint8_t)l;
        memcpy(p + n, name, l); n += l;
        name += l;
        if (*name == '.') name++;
    }
    p[n++] = 0;
    p[n++] = (uint8_t)(qtype >> 8); p[n++] = (uint8_t)qtype;
    p[n++] = (uint8_t)(qclass >> 8); p[n++] = (uint8_t)qclass;
    return n;
}

static void test_table(void)
{
    uint32_t a, b, c;
    char name[32];
    int i;

    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 15), 0);
    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 24), -1);   /* smaller than the table */
    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 33), -1);
    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 15), 0);

    a = dns_assign(&d, "www.inode.com");
    b = dns_assign(&d, "ftp.inode.com");
    CHECK(dns_in_pool(&d, a));
    CHECK(dns_in_pool(&d, b));
    CHECK(a != b);
    CHECK((a & 0xff) != 0 && (a & 0xff) != 255);
    CHECK(dns_assign(&d, "www.inode.com") == a);
    CHECK(dns_assign(&d, "WWW.Inode.COM.") == a);          /* case and the final dot */
    CHECK_EQ_S(dns_name_of(&d, a), "www.inode.com");
    CHECK_EQ_S(dns_name_of(&d, b), "ftp.inode.com");
    CHECK(dns_name_of(&d, IP(198, 19, 9, 9)) == NULL);
    CHECK(dns_name_of(&d, IP(10, 0, 0, 1)) == NULL);
    CHECK(!dns_in_pool(&d, IP(198, 20, 0, 1)));
    CHECK(dns_in_pool(&d, IP(198, 19, 255, 254)));
    /* unusable names */
    CHECK(dns_assign(&d, "") == 0);
    CHECK(dns_assign(&d, "bad name.com") == 0);
    {
        char longn[300];
        memset(longn, 'a', sizeof longn - 1);
        longn[sizeof longn - 1] = '\0';
        CHECK(dns_assign(&d, longn) == 0);
    }

    /* fill the table: the two above stay while they are used, the oldest
     * unused one goes */
    c = dns_assign(&d, "victim.example");
    for (i = 0; i < DNS_TAB - 3; i++) {
        uint32_t x;
        snprintf(name, sizeof name, "h%d.example", i);
        x = dns_assign(&d, name);
        CHECK(x != 0 && (x & 0xff) != 0 && (x & 0xff) != 255);
    }
    CHECK_EQ_S(dns_name_of(&d, c), "victim.example");      /* table exactly full, all there */
    CHECK(dns_assign(&d, "www.inode.com") == a);           /* refresh */
    CHECK_EQ_S(dns_name_of(&d, b), "ftp.inode.com");       /* use by address refreshes too */
    /* the next new names push out h0, h1, ... (victim was just used) */
    {
        uint32_t h0 = dns_assign(&d, "h0.example"), n1, n2;
        n1 = dns_assign(&d, "new1.example");               /* replaces h1 */
        n2 = dns_assign(&d, "new2.example");               /* replaces h2 */
        CHECK(n1 != n2 && n1 != a && n1 != b && n1 != c && n1 != h0);
        CHECK(dns_assign(&d, "www.inode.com") == a);
        CHECK(dns_assign(&d, "ftp.inode.com") == b);
        CHECK(dns_assign(&d, "h0.example") == h0);
        CHECK(dns_name_of(&d, n1) != NULL);
    }
    /* many more names than the table: every live address is unique */
    for (i = 0; i < 3000; i++) {
        snprintf(name, sizeof name, "m%d.example", i);
        CHECK(dns_assign(&d, name) != 0);
    }
    {
        int dup = 0, j, live = 0;
        for (i = 0; i < DNS_TAB; i++) {
            if (d.e[i].addr) live++;
            for (j = i + 1; j < DNS_TAB; j++)
                if (d.e[i].addr && d.e[i].addr == d.e[j].addr) dup++;
        }
        CHECK_EQ_I(dup, 0);
        CHECK_EQ_I(live, DNS_TAB);
    }
    CHECK(dns_name_of(&d, a) == NULL);                     /* long gone */
    /* a replaced name's address is not handed out again right away */
    CHECK(dns_assign(&d, "again.example") != a);
}

/* a small pool wraps around and still never doubles an address */
static void test_small_pool(void)
{
    char name[32];
    int i, j, dup = 0;
    uint32_t keep;

    CHECK_EQ_I(dns_init(&d, IP(10, 9, 0, 0), 22), 0);      /* 1024 addresses, 1016 usable */
    /* one name stays in use while the pool goes around several times: its
     * address must not be handed to anybody else */
    keep = dns_assign(&d, "keep.example");
    for (i = 0; i < 5000; i++) {
        uint32_t x;
        snprintf(name, sizeof name, "s%d.example", i);
        x = dns_assign(&d, name);
        CHECK(dns_in_pool(&d, x));
        CHECK(x != keep);
        CHECK(dns_assign(&d, "keep.example") == keep);
    }
    for (i = 0; i < DNS_TAB; i++)
        for (j = i + 1; j < DNS_TAB; j++)
            if (d.e[i].addr == d.e[j].addr) dup++;
    CHECK_EQ_I(dup, 0);
}

static void test_packets(void)
{
    uint8_t q[512], r[512];
    size_t qn, rn;
    uint32_t a;

    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 15), 0);

    /* A query, recursion desired */
    qn = mkq(q, 0x1234, 0x0100, "www.inode.com", 1, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn + 16);
    CHECK(r[0] == 0x12 && r[1] == 0x34);
    CHECK(r[2] & 0x80);                 /* a response */
    CHECK(r[2] & 0x01);                 /* RD copied */
    CHECK_EQ_I(r[3] & 0x0f, 0);         /* NOERROR */
    CHECK_EQ_I(r[5], 1);                /* question echoed */
    CHECK_EQ_I(r[7], 1);                /* one answer */
    CHECK(memcmp(r + 12, q + 12, qn - 12) == 0);
    CHECK(r[qn] == 0xc0 && r[qn + 1] == 0x0c);             /* name: pointer to the question */
    CHECK(r[qn + 2] == 0 && r[qn + 3] == 1 && r[qn + 4] == 0 && r[qn + 5] == 1);
    CHECK(r[qn + 6] == 0 && r[qn + 7] == 0 && r[qn + 8] == 0 && r[qn + 9] == DNS_TTL);
    CHECK(r[qn + 10] == 0 && r[qn + 11] == 4);
    a = IP(r[qn + 12], r[qn + 13], r[qn + 14], r[qn + 15]);
    CHECK(dns_in_pool(&d, a));
    CHECK_EQ_S(dns_name_of(&d, a), "www.inode.com");
    /* the same name in another case: the same address */
    qn = mkq(q, 0x1235, 0x0100, "WWW.INODE.COM", 1, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn + 16);
    CHECK(IP(r[qn + 12], r[qn + 13], r[qn + 14], r[qn + 15]) == a);

    /* AAAA, MX, PTR: an empty NOERROR answer, and no table entry */
    qn = mkq(q, 7, 0x0100, "v6.example", 28, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn);
    CHECK_EQ_I(r[3] & 0x0f, 0);
    CHECK_EQ_I(r[7], 0);
    CHECK(r[2] & 0x80);
    qn = mkq(q, 8, 0, "1.0.18.198.in-addr.arpa", 12, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn);
    CHECK_EQ_I(r[7], 0);
    /* class CHAOS: nothing to say */
    qn = mkq(q, 9, 0, "version.bind", 1, 3);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn);
    CHECK_EQ_I(r[7], 0);
    {
        int i, n = 0;
        for (i = 0; i < DNS_TAB; i++) if (d.e[i].addr) n++;
        CHECK_EQ_I(n, 1);
    }
    /* a name the table cannot hold: NXDOMAIN, not silence */
    qn = mkq(q, 10, 0, "bad name.example", 1, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn);
    CHECK_EQ_I(r[3] & 0x0f, 3);
    /* an opcode we do not do: NOTIMP */
    qn = mkq(q, 11, 0x2800, "www.inode.com", 1, 1);
    rn = dns_answer(&d, q, qn, r, sizeof r);
    CHECK_EQ_I(rn, qn);
    CHECK_EQ_I(r[3] & 0x0f, 4);

    /* an EDNS OPT record after the question is not echoed */
    qn = mkq(q, 12, 0x0100, "www.inode.com", 1, 1);
    memset(q + qn, 0, 11); q[qn + 2] = 41; q[11] = 1;
    rn = dns_answer(&d, q, qn + 11, r, sizeof r);
    CHECK_EQ_I(rn, qn + 16);
    CHECK_EQ_I(r[11], 0);

    /* ignored: short, a response, no question, two questions, truncated
     * name, compression pointer in the question, missing type/class,
     * overlong label, no room for the answer */
    qn = mkq(q, 1, 0x0100, "www.inode.com", 1, 1);
    CHECK_EQ_I(dns_answer(&d, q, 0, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, 11, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, 12, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, qn - 1, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, qn - 4, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, 20, r, sizeof r), 0);
    CHECK_EQ_I(dns_answer(&d, q, qn, r, qn + 15), 0);
    q[2] |= 0x80;
    CHECK_EQ_I(dns_answer(&d, q, qn, r, sizeof r), 0);
    q[2] &= 0x7f; q[5] = 0;
    CHECK_EQ_I(dns_answer(&d, q, qn, r, sizeof r), 0);
    q[5] = 2;
    CHECK_EQ_I(dns_answer(&d, q, qn, r, sizeof r), 0);
    q[5] = 1; q[12] = 0xc0;
    CHECK_EQ_I(dns_answer(&d, q, qn, r, sizeof r), 0);
    q[12] = 70;
    CHECK_EQ_I(dns_answer(&d, q, qn, r, sizeof r), 0);
    /* every prefix of a good query and random bytes: no crash, no answer
     * longer than the buffer */
    qn = mkq(q, 1, 0x0100, "a.b.c.d.e.f.example.org", 1, 1);
    {
        size_t i;
        unsigned s = 12345;
        for (i = 0; i < qn; i++) CHECK_EQ_I(dns_answer(&d, q, i, r, sizeof r), 0);
        for (i = 0; i < 20000; i++) {
            size_t j, n;
            s = s * 1103515245u + 12345u;
            n = (s >> 16) % 80;
            for (j = 0; j < n; j++) { s = s * 1103515245u + 12345u; q[j] = (uint8_t)(s >> 16); }
            if (i & 1) { q[2] &= 0x07; q[4] = 0; q[5] = 1; }
            CHECK(dns_answer(&d, q, n, r, 100) <= 100);
        }
    }
}

static void test_host_for(void)
{
    char h[64], lit[32];
    uint32_t a;

    CHECK_EQ_I(dns_init(&d, IP(198, 18, 0, 0), 15), 0);
    a = dns_assign(&d, "www.inode.com");
    dns_host_for(&d, "", a, h, sizeof h);
    CHECK_EQ_S(h, "www.inode.com");
    dns_host_for(&d, "other.example:8080", a, h, sizeof h);
    CHECK_EQ_S(h, "other.example:8080");
    dns_host_for(&d, "", IP(198, 18, 7, 7), h, sizeof h);   /* in the pool, not known */
    CHECK_EQ_S(h, "198.18.7.7");
    dns_host_for(&d, "", IP(127, 0, 0, 1), h, sizeof h);
    CHECK_EQ_S(h, "127.0.0.1");
    /* a browser that was given the address puts it into Host */
    dns_dotted(a, lit, sizeof lit);
    dns_host_for(&d, lit, IP(127, 0, 0, 1), h, sizeof h);
    CHECK_EQ_S(h, "www.inode.com");
    strcat(lit, ":81");
    dns_host_for(&d, lit, IP(127, 0, 0, 1), h, sizeof h);
    CHECK_EQ_S(h, "www.inode.com:81");
    dns_host_for(&d, "10.1.2.3", a, h, sizeof h);
    CHECK_EQ_S(h, "10.1.2.3");
    /* does not overrun a small buffer */
    dns_host_for(&d, "", a, h, 5);
    CHECK_EQ_S(h, "www.");
}

int main(void)
{
    test_table();
    test_small_pool();
    test_packets();
    test_host_for();
    T_REPORT("test_dns");
}
