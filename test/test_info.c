/* Tests for the Phase 2 INFO frame codec. */
#include <string.h>
#include "test.h"
#include "../modem/info.h"

static const uint8_t SYNC[8] = {0,1,1,1,0,0,1,0};

static unsigned field(const uint8_t *b, int lo, int hi)
{
    unsigned v = 0; int k;
    for (k = hi; k >= lo; k--) v = (v << 1) | b[k];
    return v;
}

static void check_frame(const uint8_t *b, int n)
{
    int k;
    for (k = 0; k < 4; k++) CHECK_EQ_I(b[k], 1);
    CHECK(memcmp(b + 4, SYNC, 8) == 0);
    for (k = n - 4; k < n; k++) CHECK_EQ_I(b[k], 1);
    CHECK(info_check(b + 12, INFO_PAYLOAD(n)));
}

int main(void)
{
    uint8_t b[INFO_MAX_BITS + 8];
    int n, k;

    /* CRC: the reflected CCITT polynomial preset to ones, no inversion, is the
     * catalogued CRC-16/MCRF4XX: "123456789" (bytes LSB first) -> 0x6F91 */
    {
        const char *s = "123456789";
        uint8_t m[72 + 16]; uint16_t c;
        for (k = 0; k < 72; k++) m[k] = (uint8_t)((s[k / 8] >> (k % 8)) & 1);
        c = info_crc(m, 72);
        CHECK_EQ_I(c, 0x6F91);
        for (k = 0; k < 16; k++) m[72 + k] = (uint8_t)((c >> k) & 1);
        CHECK(info_check(m, 88));
        m[5] ^= 1;
        CHECK(!info_check(m, 88));
    }

    /* INFO0d */
    {
        info0_t a, r;
        memset(&a, 0, sizeof a);
        a.sr2743 = 1; a.sr3429 = 1; a.hi3000 = 1; a.lo3200 = 1; a.allow3429 = 1;
        a.pwr_reduce = 1; a.max_sr_diff = 5; a.c1664 = 1; a.ack = 1;
        a.tx_power = 6; a.max_tx_power = 23; a.pwr_at_codec = 1; a.up3429 = 1;
        n = info0d_build(&a, b);
        CHECK_EQ_I(n, INFO0D_BITS);
        check_frame(b, n);
        CHECK_EQ_I(b[12], 1); CHECK_EQ_I(b[13], 0); CHECK_EQ_I(b[14], 1);
        CHECK_EQ_I(b[15], 0); CHECK_EQ_I(b[16], 1); CHECK_EQ_I(b[17], 1); CHECK_EQ_I(b[18], 0);
        CHECK_EQ_I(b[19], 1); CHECK_EQ_I(b[20], 1);
        CHECK_EQ_I(field(b, 21, 23), 5);
        CHECK_EQ_I(b[24], 0); CHECK_EQ_I(b[25], 1);
        CHECK_EQ_I(field(b, 26, 27), 0);
        CHECK_EQ_I(b[28], 1);
        CHECK_EQ_I(field(b, 29, 32), 6);
        CHECK_EQ_I(field(b, 33, 37), 23);
        CHECK_EQ_I(b[38], 1); CHECK_EQ_I(b[39], 0); CHECK_EQ_I(b[40], 1); CHECK_EQ_I(b[41], 0);
        memset(&r, 0xff, sizeof r);
        info0d_parse(b + 12, &r);
        CHECK(memcmp(&a, &r, sizeof a) == 0);
    }

    /* INFO0a */
    {
        info0_t a, r;
        memset(&a, 0, sizeof a);
        a.sr2800 = 1; a.lo3000 = 1; a.hi3200 = 1; a.max_sr_diff = 2; a.cme = 1;
        n = info0a_build(&a, b);
        CHECK_EQ_I(n, INFO0A_BITS);
        check_frame(b, n);
        CHECK_EQ_I(b[13], 1); CHECK_EQ_I(b[15], 1); CHECK_EQ_I(b[18], 1);
        CHECK_EQ_I(field(b, 21, 23), 2); CHECK_EQ_I(b[24], 1); CHECK_EQ_I(b[28], 0);
        memset(&r, 0xff, sizeof r);
        info0a_parse(b + 12, &r);
        CHECK_EQ_I(r.sr2800, 1); CHECK_EQ_I(r.sr2743, 0); CHECK_EQ_I(r.hi3200, 1);
        CHECK_EQ_I(r.max_sr_diff, 2); CHECK_EQ_I(r.cme, 1); CHECK_EQ_I(r.ack, 0);
        CHECK_EQ_I(r.tx_power, 0);   /* digital-only fields cleared */
    }

    /* INFO1d */
    {
        info1d_t a, r;
        memset(&a, 0, sizeof a);
        a.min_pwr_red = 3; a.add_pwr_red = 5; a.md_len = 77;
        for (k = 0; k < 6; k++) {
            a.sr[k].hi_carrier = (uint8_t)(k & 1);
            a.sr[k].preemph = (uint8_t)(k + 2);
            a.sr[k].max_rate = (uint8_t)(14 - k);
        }
        a.freq_off = -37;
        n = info1d_build(&a, b);
        CHECK_EQ_I(n, INFO1D_BITS);
        check_frame(b, n);
        CHECK_EQ_I(field(b, 12, 14), 3);
        CHECK_EQ_I(field(b, 15, 17), 5);
        CHECK_EQ_I(field(b, 18, 24), 77);
        CHECK_EQ_I(b[25], 0); CHECK_EQ_I(field(b, 26, 29), 2); CHECK_EQ_I(field(b, 30, 33), 14);
        CHECK_EQ_I(b[34], 1); CHECK_EQ_I(field(b, 35, 38), 3); CHECK_EQ_I(field(b, 39, 42), 13);
        CHECK_EQ_I(b[70], 1); CHECK_EQ_I(field(b, 71, 74), 7); CHECK_EQ_I(field(b, 75, 78), 9);
        CHECK_EQ_I(field(b, 79, 88), (unsigned)(-37) & 0x3ff);
        memset(&r, 0, sizeof r);     /* (struct has a padding byte) */
        r.freq_off = 99; r.md_len = 1; r.sr[5].max_rate = 0;
        info1d_parse(b + 12, &r);
        CHECK(memcmp(&a, &r, sizeof a) == 0);
        a.freq_off = -512;
        info1d_build(&a, b); info1d_parse(b + 12, &r);
        CHECK_EQ_I(r.freq_off, -512);
    }

    /* INFO1a, V.90 form (Table 10): bits 37:39 carry the integer 6 */
    {
        info1a_t a, r;
        memset(&a, 0, sizeof a);
        a.v90 = 1; a.md_len = 9; a.uinfo = 93; a.up_sr = 4; a.freq_off = 211;
        n = info1a_build(&a, b);
        CHECK_EQ_I(n, INFO1A_BITS);
        check_frame(b, n);
        CHECK_EQ_I(field(b, 12, 17), 0);
        CHECK_EQ_I(field(b, 18, 24), 9);
        CHECK_EQ_I(field(b, 25, 31), 93);
        CHECK_EQ_I(field(b, 32, 33), 0);
        CHECK_EQ_I(field(b, 34, 36), 4);
        CHECK_EQ_I(field(b, 37, 39), 6);
        CHECK_EQ_I(field(b, 40, 49), 211);
        memset(&r, 0xff, sizeof r);
        info1a_parse(b + 12, &r);
        CHECK_EQ_I(r.v90, 1); CHECK_EQ_I(r.md_len, 9); CHECK_EQ_I(r.uinfo, 93);
        CHECK_EQ_I(r.up_sr, 4); CHECK_EQ_I(r.freq_off, 211);
    }

    /* INFO1a, V.34 form (Table 11) */
    {
        info1a_t a, r;
        memset(&a, 0, sizeof a);
        a.v90 = 0; a.min_pwr_red = 2; a.add_pwr_red = 1; a.md_len = 3; a.hi_carrier = 1;
        a.preemph = 7; a.max_rate = 11; a.up_sr = 5; a.down_sr = 3; a.freq_off = -1;
        n = info1a_build(&a, b);
        check_frame(b, n);
        CHECK_EQ_I(field(b, 12, 14), 2); CHECK_EQ_I(field(b, 15, 17), 1);
        CHECK_EQ_I(b[25], 1); CHECK_EQ_I(field(b, 26, 29), 7); CHECK_EQ_I(field(b, 30, 33), 11);
        CHECK_EQ_I(field(b, 34, 36), 5); CHECK_EQ_I(field(b, 37, 39), 3);
        memset(&r, 0xff, sizeof r);
        info1a_parse(b + 12, &r);
        CHECK_EQ_I(r.v90, 0); CHECK_EQ_I(r.down_sr, 3); CHECK_EQ_I(r.max_rate, 11);
        CHECK_EQ_I(r.preemph, 7); CHECK_EQ_I(r.freq_off, -1); CHECK_EQ_I(r.uinfo, 0);
    }

    T_REPORT("info");
}
