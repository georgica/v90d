/* The DNS relay's message handling (srv/dnsrelay.h): what is a question,
 * and the answer "none" the box gives to AAAA. The relay itself is in
 * ns_call (18). */
#include "../dnsrelay.h"
#include "../../test/test.h"
#include <stdio.h>
#include <string.h>

/* a query for name (labels) of type t: its length */
static size_t query(uint8_t *m, const char *labels, int t)
{
    size_t n = 12, l = strlen(labels) + 1;
    memset(m, 0, 12);
    m[0] = 0xab; m[1] = 0xcd; m[2] = 0x01; m[5] = 1;
    memcpy(m + n, labels, l); n += l;
    m[n++] = (uint8_t)(t >> 8); m[n++] = (uint8_t)t; m[n++] = 0; m[n++] = 1;
    return n;
}

int main(void)
{
    uint8_t m[300], o[300];
    size_t n, k;

    n = query(m, "\7example\3com", DNS_T_A);
    CHECK_EQ_I(dnsrelay_qtype(m, n), DNS_T_A);
    n = query(m, "\7example\3com", DNS_T_AAAA);
    CHECK_EQ_I(dnsrelay_qtype(m, n), DNS_T_AAAA);
    n = query(m, "", 15);                                   /* the root, MX */
    CHECK_EQ_I(dnsrelay_qtype(m, n), 15);

    /* not questions the relay takes */
    n = query(m, "\7example\3com", DNS_T_A);
    CHECK_EQ_I(dnsrelay_qtype(m, 11), -1);                  /* shorter than a header and a question */
    CHECK_EQ_I(dnsrelay_qtype(m, n - 1), -1);               /* cut in the class */
    CHECK_EQ_I(dnsrelay_qtype(m, 12 + 5), -1);              /* cut in the name */
    m[2] = 0x81; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[2] = 0x01;     /* a response */
    m[2] = 0x29; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[2] = 0x01;     /* opcode 5: an update */
    m[5] = 2; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[5] = 1;           /* two questions */
    m[5] = 0; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[5] = 1;
    m[4] = 1; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[4] = 0;
    m[12] = 0xc0; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[12] = 7;      /* a pointer */
    m[12] = 60; CHECK_EQ_I(dnsrelay_qtype(m, n), -1); m[12] = 7;        /* a label past the end */
    {   /* labels to the very end and no root: not read past it */
        memset(m + 12, 1, sizeof m - 12);
        CHECK_EQ_I(dnsrelay_qtype(m, sizeof m), -1);
    }

    /* the answer "none" to a AAAA question */
    n = query(m, "\3www\7example\3com", DNS_T_AAAA);
    m[2] = 0x01;                                            /* RD */
    k = dnsrelay_none(m, n, o, sizeof o);
    CHECK_EQ_I((int)k, (int)n);                             /* the header and the question */
    CHECK(o[0] == 0xab && o[1] == 0xcd);                    /* the id */
    CHECK_EQ_I(o[2], 0x81);                                 /* a response, RD as asked */
    CHECK_EQ_I(o[3], 0x80);                                 /* RA, NOERROR */
    CHECK(o[4] == 0 && o[5] == 1);                          /* one question */
    CHECK(o[6] == 0 && o[7] == 0 && o[8] == 0 && o[9] == 0 && o[10] == 0 && o[11] == 0);
    CHECK(memcmp(o + 12, m + 12, n - 12) == 0);
    {   /* an EDNS record after the question is left out of the answer */
        uint8_t e[300]; size_t en;
        memcpy(e, m, n); e[11] = 1;
        memcpy(e + n, "\0\0\x29\x10\0\0\0\0\0\0\0", 11); en = n + 11;
        CHECK_EQ_I((int)dnsrelay_none(e, en, o, sizeof o), (int)n);
        CHECK_EQ_I(o[11], 0);
    }
    CHECK_EQ_I((int)dnsrelay_none(m, n, o, n - 1), 0);      /* no room */
    m[2] = 0x81; CHECK_EQ_I((int)dnsrelay_none(m, n, o, sizeof o), 0);  /* not a question */

    /* the first record of a type in an answer: a CNAME first, names
     * compressed, the data and TTL of the AAAA */
    {
        uint8_t r[300], rd[16]; size_t rn; uint32_t ttl = 0;
        static const uint8_t want6[16] = { 0x20, 0x01, 0x0d, 0xb8, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x80 };
        n = query(m, "\3www\7example\3com", DNS_T_AAAA);
        memcpy(r, m, n); rn = n;
        r[2] = 0x81; r[3] = 0x80; r[7] = 2;                         /* a response, two answers */
        {   /* www.example.com CNAME v6.example.com */
            static const uint8_t cn[] = { 0xc0, 0x0c, 0, 5, 0, 1, 0, 0, 1, 0x2c, 0, 5, 2, 'v', '6', 0xc0, 0x10 };
            memcpy(r + rn, cn, sizeof cn); rn += sizeof cn;
        }
        {   /* v6.example.com AAAA 2001:db8:5::80, TTL 300; its name a pointer to the CNAME's data */
            uint8_t aa[12 + 16] = { 0xc0, (uint8_t)(n + 12), 0, 28, 0, 1, 0, 0, 1, 0x2c, 0, 16 };
            memcpy(aa + 12, want6, 16);
            memcpy(r + rn, aa, sizeof aa); rn += sizeof aa;
        }
        CHECK_EQ_I((int)dnsrelay_first(r, rn, DNS_T_AAAA, rd, sizeof rd, &ttl), 16);
        CHECK(memcmp(rd, want6, 16) == 0);
        CHECK_EQ_I((int)ttl, 300);
        CHECK_EQ_I((int)dnsrelay_first(r, rn, DNS_T_A, rd, sizeof rd, &ttl), 0);    /* no A */
        CHECK_EQ_I((int)dnsrelay_first(r, rn - 1, DNS_T_AAAA, rd, sizeof rd, &ttl), 0);  /* cut short */
        CHECK_EQ_I((int)dnsrelay_first(r, rn, DNS_T_AAAA, rd, 8, &ttl), 0);        /* no room */
        r[rn - 16 - 12 + 4] = 0; r[rn - 16 - 12 + 5] = 3;                         /* class CH: not IN */
        CHECK_EQ_I((int)dnsrelay_first(r, rn, DNS_T_AAAA, rd, sizeof rd, &ttl), 0);
        CHECK_EQ_I((int)dnsrelay_first(m, n, DNS_T_AAAA, rd, sizeof rd, &ttl), 0); /* a question, not an answer */
        {   /* a name that is a pointer to itself: not followed for ever */
            uint8_t z[40]; memset(z, 0, sizeof z);
            z[2] = 0x81; z[5] = 0; z[7] = 1; z[12] = 0xc0; z[13] = 12;
            CHECK_EQ_I((int)dnsrelay_first(z, 30, DNS_T_A, rd, sizeof rd, &ttl), 0);
        }
    }
    /* the stand-in answer: the question back, one A record */
    {
        uint8_t q2[300], o2[300]; size_t qn2, on2;
        qn2 = query(q2, "\6v6only\7example\3com", DNS_T_A);
        on2 = dnsrelay_answer_a(q2, qn2, 0xc6120001u, 60, o2, sizeof o2);
        CHECK_EQ_I((int)on2, (int)qn2 + 16);
        CHECK(o2[0] == 0xab && o2[1] == 0xcd);
        CHECK_EQ_I(o2[2], 0x81); CHECK_EQ_I(o2[3], 0x80);
        CHECK(o2[5] == 1 && o2[7] == 1 && o2[9] == 0 && o2[11] == 0);
        CHECK(memcmp(o2 + 12, q2 + 12, qn2 - 12) == 0);
        CHECK(o2[qn2] == 0xc0 && o2[qn2 + 1] == 0x0c);                         /* the question's name */
        CHECK(o2[qn2 + 3] == 1 && o2[qn2 + 5] == 1);                           /* A, IN */
        CHECK_EQ_I(o2[qn2 + 9], 60);
        CHECK(o2[qn2 + 11] == 4 && o2[qn2 + 12] == 198 && o2[qn2 + 13] == 18 && o2[qn2 + 15] == 1);
        {   uint8_t rd[4]; uint32_t ttl;                                       /* and it reads back as one */
            CHECK_EQ_I((int)dnsrelay_first(o2, on2, DNS_T_A, rd, sizeof rd, &ttl), 4); }
        CHECK_EQ_I((int)dnsrelay_answer_a(q2, qn2, 1, 60, o2, qn2 + 10), 0);     /* no room */
    }
    T_REPORT("dnsrelay");
}
