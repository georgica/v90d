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

    T_REPORT("dnsrelay");
}
