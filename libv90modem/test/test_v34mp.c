/* V.34 J / J' patterns and MP sequences against Tables 18 to 21, and the
 * rate decision of 11.4.1.2.3. The expected bit strings are written out
 * from the tables; the CRC is checked with an independent bit-serial model
 * of Figure 14 (the register of modem/info.c is another implementation of
 * the same reading). */
#include <string.h>
#include "test.h"
#include "../modem/v34mp.h"
#include "../modem/info.h"

/* compare with a pattern: '0' / '1' must match, anything else is skipped */
static int match(const uint8_t *b, const char *pat)
{
    int i, bad = 0;
    for (i = 0; pat[i]; i++)
        if ((pat[i] == '0' || pat[i] == '1') && b[i] != (uint8_t)(pat[i] - '0')) bad++;
    return bad;
}

static unsigned fld(const uint8_t *b, int lo, int hi)
{
    unsigned v = 0; int i;
    for (i = lo; i <= hi; i++) v |= (unsigned)b[i] << (i - lo);
    return v;
}

/* Figure 14 as a shift register of 16 cells, cell 0 at the data end */
typedef struct { uint8_t c[16]; } fig14_t;
static void fig14_init(fig14_t *f) { memset(f->c, 1, 16); }
static void fig14_bit(fig14_t *f, int bit)
{
    int fb = (bit ^ f->c[0]) & 1, i;
    for (i = 0; i < 15; i++) f->c[i] = f->c[i + 1];
    f->c[15] = 0;
    if (fb) { f->c[15] ^= 1; f->c[10] ^= 1; f->c[3] ^= 1; }
}
static void fig14_range(fig14_t *f, const uint8_t *b, int lo, int hi)
{
    int i;
    for (i = lo; i <= hi; i++) fig14_bit(f, b[i]);
}
static unsigned fig14_val(const fig14_t *f)
{
    unsigned v = 0; int i;
    for (i = 0; i < 16; i++) v |= (unsigned)f->c[i] << i;
    return v;
}

int main(void)
{
    v34_mp_t mp, out, a, c;
    uint8_t b[V34_MP_MAX_BITS + 8];
    fig14_t f;
    int n, i, ac, ca;
    static const int start1[] = { 17, 34, 51, 68, 85, 102, 119, 136, 153, 170 };

    /* ---- Tables 18 and 19, left-most bit first in time ---- */
    CHECK_EQ_I(match(v34_j4,     "0000100110010001"), 0);
    CHECK_EQ_I(match(v34_j16,    "0000110110010001"), 0);
    CHECK_EQ_I(match(v34_jprime, "1111100110010001"), 0);

    /* ---- Table 20: Type 0 ---- */
    memset(&mp, 0, sizeof mp);
    mp.type = 0; mp.max_ca = 12; mp.max_ac = 9; mp.aux = 0; mp.trellis = 1;
    mp.nonlinear = 1; mp.shaping = 0; mp.ack = 1; mp.rates = 0x0FFF; mp.asym = 1;
    memset(b, 9, sizeof b);
    n = v34_mp_build(&mp, b);
    CHECK_EQ_I(n, 88);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start */
        "0"                      /* 18    type 0 */
        "0"                      /* 19    reserved */
        "0011"                   /* 20:23 call -> answer 12 * 2400, LSB first */
        "1001"                   /* 24:27 answer -> call 9 * 2400 */
        "0"                      /* 28    auxiliary channel */
        "10"                     /* 29:30 trellis = 1 (32 states) */
        "1"                      /* 31    non-linear */
        "0"                      /* 32    shaping */
        "1"                      /* 33    acknowledge */
        "0"                      /* 34    start */
        "11111111111100"         /* 35:48 2400 .. 28800 set, 31200 and 33600 clear */
        "0"                      /* 49    reserved */
        "1"                      /* 50    asymmetric rates */
        "0"                      /* 51    start */
        "0000000000000000"       /* 52:67 reserved */
        "0"                      /* 68    start */
        "cccccccccccccccc"       /* 69:84 CRC */
        "000"), 0);              /* 85:87 fill */
    fig14_init(&f);
    fig14_range(&f, b, 18, 33); fig14_range(&f, b, 35, 50); fig14_range(&f, b, 52, 67);
    CHECK_EQ_I(fld(b, 69, 84), fig14_val(&f));
    CHECK_EQ_I(b[88], 9);
    /* the same CRC through the INFO sequences' implementation */
    {
        uint8_t info[48]; int k = 0;
        for (i = 18; i <= 33; i++) info[k++] = b[i];
        for (i = 35; i <= 50; i++) info[k++] = b[i];
        for (i = 52; i <= 67; i++) info[k++] = b[i];
        CHECK_EQ_I(fld(b, 69, 84), info_crc(info, 48));
    }

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v34_mp_parse(b, 88, &out), 88);
    CHECK_EQ_I(out.type, 0); CHECK_EQ_I(out.max_ca, 12); CHECK_EQ_I(out.max_ac, 9);
    CHECK_EQ_I(out.aux, 0); CHECK_EQ_I(out.trellis, 1); CHECK_EQ_I(out.nonlinear, 1);
    CHECK_EQ_I(out.shaping, 0); CHECK_EQ_I(out.ack, 1); CHECK_EQ_I(out.rates, 0x0FFF);
    CHECK_EQ_I(out.asym, 1);
    for (i = 0; i < 3; i++) { CHECK_EQ_I(out.h[i][0], 0); CHECK_EQ_I(out.h[i][1], 0); }
    /* the three fill bits are not needed to accept it */
    CHECK_EQ_I(v34_mp_parse(b, 85, &out), 88);
    CHECK_EQ_I(v34_mp_parse(b, 84, &out), V34_MP_SHORT);
    CHECK_EQ_I(v34_mp_parse(b, 10, &out), V34_MP_SHORT);
    b[33] ^= 1; CHECK_EQ_I(v34_mp_parse(b, 88, &out), V34_MP_BAD); b[33] ^= 1;
    b[55] ^= 1; CHECK_EQ_I(v34_mp_parse(b, 88, &out), V34_MP_BAD); b[55] ^= 1;
    b[68] ^= 1; CHECK_EQ_I(v34_mp_parse(b, 88, &out), V34_MP_BAD); b[68] ^= 1;
    b[5] ^= 1;  CHECK_EQ_I(v34_mp_parse(b, 88, &out), V34_MP_BAD); b[5] ^= 1;
    CHECK_EQ_I(v34_mp_parse(b, 88, &out), 88);

    /* MP and MP' differ in bit 33 and the CRC only */
    {
        uint8_t b2[V34_MP_MAX_BITS];
        int d = 0;
        mp.ack = 0;
        CHECK_EQ_I(v34_mp_build(&mp, b2), 88);
        for (i = 0; i < 69; i++) if (b[i] != b2[i]) { d++; CHECK_EQ_I(i, 33); }
        CHECK_EQ_I(d, 1);
        CHECK(fld(b, 69, 84) != fld(b2, 69, 84));
    }

    /* ---- Table 21: Type 1 with precoder coefficients ---- */
    memset(&mp, 0, sizeof mp);
    mp.type = 1; mp.max_ca = 14; mp.max_ac = 13; mp.aux = 1; mp.trellis = 2;
    mp.nonlinear = 0; mp.shaping = 1; mp.ack = 0; mp.rates = 0x3FFF; mp.asym = 0;
    mp.h[0][0] = 0x1234;  mp.h[0][1] = -2;
    mp.h[1][0] = -32768;  mp.h[1][1] = 32767;
    mp.h[2][0] = 0x0001;  mp.h[2][1] = (int16_t)0x8001;
    memset(b, 9, sizeof b);
    n = v34_mp_build(&mp, b);
    CHECK_EQ_I(n, 188);
    for (i = 0; i <= 16; i++) CHECK_EQ_I(b[i], 1);
    for (i = 0; i < 10; i++) CHECK_EQ_I(b[start1[i]], 0);
    CHECK_EQ_I(b[18], 1); CHECK_EQ_I(b[19], 0);
    CHECK_EQ_I(fld(b, 20, 23), 14); CHECK_EQ_I(fld(b, 24, 27), 13);
    CHECK_EQ_I(b[28], 1);
    CHECK_EQ_I(fld(b, 29, 30), 2);
    CHECK_EQ_I(b[31], 0); CHECK_EQ_I(b[32], 1); CHECK_EQ_I(b[33], 0);
    CHECK_EQ_I(fld(b, 35, 48), 0x3FFF);
    CHECK_EQ_I(b[49], 0); CHECK_EQ_I(b[50], 0);
    CHECK_EQ_I(fld(b, 52, 67), 0x1234);
    CHECK_EQ_I(fld(b, 69, 84), 0xFFFE);
    CHECK_EQ_I(fld(b, 86, 101), 0x8000);
    CHECK_EQ_I(fld(b, 103, 118), 0x7FFF);
    CHECK_EQ_I(fld(b, 120, 135), 0x0001);
    CHECK_EQ_I(fld(b, 137, 152), 0x8001);
    CHECK_EQ_I(fld(b, 154, 169), 0);
    fig14_init(&f);
    for (i = 0; i < 9; i++) fig14_range(&f, b, start1[i] + 1, start1[i] + 16);
    CHECK_EQ_I(fld(b, 171, 186), fig14_val(&f));
    CHECK_EQ_I(b[187], 0);
    CHECK_EQ_I(b[188], 9);
    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v34_mp_parse(b, 188, &out), 188);
    CHECK_EQ_I(out.type, 1); CHECK_EQ_I(out.max_ca, 14); CHECK_EQ_I(out.max_ac, 13);
    CHECK_EQ_I(out.aux, 1); CHECK_EQ_I(out.trellis, 2); CHECK_EQ_I(out.shaping, 1);
    CHECK_EQ_I(out.ack, 0); CHECK_EQ_I(out.rates, 0x3FFF); CHECK_EQ_I(out.asym, 0);
    CHECK_EQ_I(out.h[0][0], 0x1234); CHECK_EQ_I(out.h[0][1], -2);
    CHECK_EQ_I(out.h[1][0], -32768); CHECK_EQ_I(out.h[1][1], 32767);
    CHECK_EQ_I(out.h[2][0], 1);      CHECK_EQ_I(out.h[2][1], -32767);
    CHECK_EQ_I(v34_mp_parse(b, 187, &out), 188);
    CHECK_EQ_I(v34_mp_parse(b, 186, &out), V34_MP_SHORT);
    CHECK_EQ_I(v34_mp_parse(b, 88, &out), V34_MP_SHORT);
    b[140] ^= 1; CHECK_EQ_I(v34_mp_parse(b, 188, &out), V34_MP_BAD); b[140] ^= 1;

    /* out of range fields are refused */
    mp.max_ca = 15; CHECK_EQ_I(v34_mp_build(&mp, b), -1); mp.max_ca = 14;
    mp.trellis = 3; CHECK_EQ_I(v34_mp_build(&mp, b), -1); mp.trellis = 0;
    mp.rates = 0x4000; CHECK_EQ_I(v34_mp_build(&mp, b), -1); mp.rates = 0x3FFF;
    mp.type = 2; CHECK_EQ_I(v34_mp_build(&mp, b), -1);

    /* ---- 11.4.1.2.3: the rate decision ---- */
    memset(&a, 0, sizeof a); memset(&c, 0, sizeof c);
    /* symmetric (either side has bit 50 clear): one rate, below all four limits */
    a.max_ca = 12; a.max_ac = 14; a.rates = 0x3FFF; a.asym = 1;
    c.max_ca = 13; c.max_ac = 10; c.rates = 0x3FFF; c.asym = 0;
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 24000); CHECK_EQ_I(ca, 24000);
    /* asymmetric: each direction from its own two limits */
    c.asym = 1;
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 24000); CHECK_EQ_I(ca, 28800);
    /* "enabled in both modems": the masks are intersected */
    a.rates = 0x0AAA;                   /* 4800, 9600, ... 28800 */
    c.rates = 0x3FFF & ~(1u << 9);      /* not 24000 */
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 19200); CHECK_EQ_I(ca, 28800);
    /* nothing in common below the limit */
    a.rates = 0x2000; c.rates = 0x3FFF;
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 0); CHECK_EQ_I(ca, 0);
    /* the lowest rate */
    a.rates = c.rates = 0x3FFF; a.max_ca = 1; a.max_ac = 1;
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 2400); CHECK_EQ_I(ca, 2400);

    /* cleardown: zero in both rate fields (11.7.1.1) */
    CHECK(!v34_mp_is_cleardown(&a));
    a.max_ca = 0; CHECK(!v34_mp_is_cleardown(&a));
    a.max_ac = 0; CHECK(v34_mp_is_cleardown(&a));
    v34_mp_rates(&a, &c, &ac, &ca);
    CHECK_EQ_I(ac, 0); CHECK_EQ_I(ca, 0);

    T_REPORT("v34mp");
}
