/* Sequence codecs: Jd, MP, DIL descriptor, CP, E.
 *
 * Expected bit positions are taken by hand from Tables 12, 13, 14 and 16 of
 * V.90; the expected CRC comes from a cell-by-cell model of Figure 14/V.34
 * kept in this file, independent of the library's register arithmetic. */
#include <string.h>
#include "test.h"
#include "v90_seq.h"

/* ---- independent helpers ------------------------------------------------ */

/* Figure 14/V.34 as drawn: cells 15..0, data enters at the bit 0 end, the
 * feedback (data XOR cell 0) enters cell 15 and the gaps after cells 11 and 4
 * (x^16 + x^12 + x^5 + 1). Cells preset to 1. Result bit k = cell k. */
typedef struct { int c[16]; } fig14_t;
static void fig14_init(fig14_t *f) { int i; for (i = 0; i < 16; i++) f->c[i] = 1; }
static void fig14_bit(fig14_t *f, int bit)
{
    int fb = (bit ^ f->c[0]) & 1, i;
    for (i = 0; i < 15; i++) f->c[i] = f->c[i + 1];
    f->c[15] = fb;
    f->c[10] ^= fb;
    f->c[3] ^= fb;
}
static unsigned fig14_val(const fig14_t *f)
{
    unsigned v = 0; int i;
    for (i = 0; i < 16; i++) v |= (unsigned)f->c[i] << i;
    return v;
}
static void fig14_range(fig14_t *f, const uint8_t *b, int lsb, int msb)
{
    int i; for (i = lsb; i <= msb; i++) fig14_bit(f, b[i]);
}

/* integer value of the field occupying bits lsb..msb, LSB first */
static unsigned fld(const uint8_t *b, int lsb, int msb)
{
    unsigned v = 0; int i;
    for (i = lsb; i <= msb; i++) v |= (unsigned)(b[i] & 1) << (i - lsb);
    return v;
}

/* compare against a hand-written string: '0' / '1' must match, 'c' is skipped */
static int match(const uint8_t *b, const char *s)
{
    int i, bad = 0;
    for (i = 0; s[i]; i++)
        if (s[i] != 'c' && b[i] != (uint8_t)(s[i] - '0')) bad++;
    return bad;
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 8; }

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }

/* ---- CRC ---------------------------------------------------------------- */

static void test_crc(void)
{
    /* Reflected CRC-CCITT, preset 0xFFFF, no final XOR, is the catalogued
     * CRC-16/MCRF4XX whose check value over "123456789" (each octet LSB
     * first) is 0x6F91. */
    static const char msg[] = "123456789";
    uint8_t bits[72 + 16];
    fig14_t f;
    int i, k;
    uint16_t c;
    for (i = 0; i < 9; i++)
        for (k = 0; k < 8; k++) bits[i * 8 + k] = (uint8_t)((msg[i] >> k) & 1);
    c = v90_crc(bits, 72);
    CHECK_EQ_I(c, 0x6F91);
    /* the Figure 14 model agrees */
    fig14_init(&f);
    fig14_range(&f, bits, 0, 71);
    CHECK_EQ_I(fig14_val(&f), 0x6F91);
    /* appended bit 0 first, the receiver's register ends at 0 */
    for (k = 0; k < 16; k++) bits[72 + k] = (uint8_t)((c >> k) & 1);
    CHECK_EQ_I(v90_crc(bits, 88), 0);
    /* streaming form */
    c = 0xFFFF;
    for (i = 0; i < 72; i++) c = v90_crc_step(c, bits[i]);
    CHECK_EQ_I(c, 0x6F91);
    CHECK_EQ_I(v90_crc(bits, 0), 0xFFFF);
}

/* ---- frame sync search and E -------------------------------------------- */

static void test_sync_e(void)
{
    uint8_t b[80];
    int i;
    memset(b, 0, sizeof b);
    for (i = 10; i < 27; i++) b[i] = 1;          /* 17 ones at 10..26, 0 at 27 */
    CHECK_EQ_I(v90_find_sync(b, 80, 0), 10);
    CHECK_EQ_I(v90_find_sync(b, 80, 10), 10);
    CHECK_EQ_I(v90_find_sync(b, 80, 11), -1);
    CHECK_EQ_I(v90_find_sync(b, 27, 0), -1);     /* start bit not yet available */
    CHECK_EQ_I(v90_find_sync(b, 28, 0), 10);
    b[9] = 1;                                    /* 18 ones: not a sync */
    CHECK_EQ_I(v90_find_sync(b, 80, 0), -1);
    memset(b, 0, sizeof b);
    for (i = 0; i < 17; i++) b[i] = 1;           /* at index 0 */
    CHECK_EQ_I(v90_find_sync(b, 80, 0), 0);
    b[16] = 0;                                   /* only 16 ones */
    CHECK_EQ_I(v90_find_sync(b, 80, 0), -1);

    /* E: 20 ones; its tail must not look like a frame sync */
    memset(b, 0, sizeof b);
    CHECK_EQ_I(v90_e_build(b + 5), 20);
    for (i = 0; i < 5; i++) CHECK_EQ_I(b[i], 0);
    for (i = 5; i < 25; i++) CHECK_EQ_I(b[i], 1);
    CHECK_EQ_I(b[25], 0);
    CHECK_EQ_I(v90_e_match(b + 5, 20), 1);
    CHECK_EQ_I(v90_e_match(b + 5, 19), 0);
    CHECK_EQ_I(v90_e_match(b + 6, 20), 0);
    CHECK_EQ_I(v90_find_sync(b, 80, 0), -1);
}

/* ---- Jd ------------------------------------------------------------------ */

static void test_jd(void)
{
    v90_jd_t jd, out;
    uint8_t b[V90_JD_BITS + 8];
    fig14_t f;
    int n, i;

    /* 28000 (r=0), 30666 (r=2), 48000 (r=15), 49333 (r=16), 56000 (r=21);
     * 4-point CP in training, 16-point in renegotiation, max lookahead 2. */
    memset(&jd, 0, sizeof jd);
    jd.rates = (1u << 0) | (1u << 2) | (1u << 15) | (1u << 16) | (1u << 21);
    jd.cp16_train = 0; jd.cp16_reneg = 1; jd.max_ld = 2;
    memset(b, 9, sizeof b);
    n = v90_jd_build(&jd, b);
    CHECK_EQ_I(n, 72);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "1010000000000001"       /* 18:33 28000 .. 48000 */
        "0"                      /* 34    start bit */
        "100001"                 /* 35:40 49333 .. 56000 */
        "000000"                 /* 41:46 reserved */
        "0"                      /* 47    4-point in training */
        "1"                      /* 48    16-point in renegotiation */
        "01"                     /* 49:50 lookahead 2, LSB first */
        "0"                      /* 51    start bit */
        "cccccccccccccccc"       /* 52:67 CRC */
        "0000"), 0);             /* 68:71 fill */
    fig14_init(&f);
    fig14_range(&f, b, 18, 33);
    fig14_range(&f, b, 35, 50);
    CHECK_EQ_I(fld(b, 52, 67), fig14_val(&f));
    CHECK_EQ_I(b[72], 9);                        /* nothing written past the end */

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v90_jd_parse(b, 72, &out), 72);
    CHECK_EQ_I(out.rates, jd.rates);
    CHECK_EQ_I(out.cp16_train, 0); CHECK_EQ_I(out.cp16_reneg, 1); CHECK_EQ_I(out.max_ld, 2);

    CHECK_EQ_I(v90_jd_parse(b, 71, &out), V90_ERR_SHORT);
    b[20] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_CRC);  b[20] ^= 1;
    b[41] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_CRC);  b[41] ^= 1;  /* reserved bits are covered */
    b[60] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_CRC);  b[60] ^= 1;
    b[3]  ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_SYNC); b[3]  ^= 1;
    b[17] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_SYNC); b[17] ^= 1;
    b[34] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_SYNC); b[34] ^= 1;
    b[51] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_SYNC); b[51] ^= 1;
    b[70] ^= 1; CHECK_EQ_I(v90_jd_parse(b, 72, &out), V90_ERR_SYNC); b[70] ^= 1;
    CHECK_EQ_I(v90_jd_parse(b, 72, &out), 72);

    /* all rates, all options */
    jd.rates = 0x3FFFFF; jd.cp16_train = 1; jd.cp16_reneg = 0; jd.max_ld = 3;
    CHECK_EQ_I(v90_jd_build(&jd, b), 72);
    for (i = 18; i <= 33; i++) CHECK_EQ_I(b[i], 1);
    for (i = 35; i <= 40; i++) CHECK_EQ_I(b[i], 1);
    CHECK_EQ_I(b[47], 1); CHECK_EQ_I(b[48], 0); CHECK_EQ_I(fld(b, 49, 50), 3);
    CHECK_EQ_I(v90_jd_parse(b, 72, &out), 72);
    CHECK_EQ_I(out.rates, 0x3FFFFF); CHECK_EQ_I(out.max_ld, 3);
    /* no run of 17 ones inside the body: only one sync in a repeated pair */
    CHECK_EQ_I(v90_find_sync(b, 72, 1), -1);

    jd.rates = 1u << 22; CHECK_EQ_I(v90_jd_build(&jd, b), V90_ERR_RANGE);
    jd.rates = 1; jd.max_ld = 4; CHECK_EQ_I(v90_jd_build(&jd, b), V90_ERR_RANGE);
}

/* ---- MP ------------------------------------------------------------------ */

static void test_mp(void)
{
    v90_mp_t mp, out;
    uint8_t b[V90_MP_MAX_BITS + 8];
    fig14_t f;
    int n, i;
    static const int start1[] = { 17, 34, 51, 68, 85, 102, 119, 136, 153, 170 };

    /* Type 0: max 28800 (drn 12), 32-state trellis, nonlinear on, minimum
     * shaping, acknowledge set, 4800..28800 enabled. */
    memset(&mp, 0, sizeof mp);
    mp.type = 0; mp.max_rate = 12; mp.trellis = 1; mp.nonlinear = 1; mp.shaping = 0; mp.ack = 1;
    mp.rates = 0x07FF;
    memset(b, 9, sizeof b);
    n = v90_mp_build(&mp, b);
    CHECK_EQ_I(n, 86);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start */
        "0"                      /* 18    type 0 */
        "00000"                  /* 19:23 reserved */
        "0011"                   /* 24:27 drn = 12, LSB first */
        "0"                      /* 28    reserved */
        "10"                     /* 29:30 trellis = 1 */
        "1"                      /* 31    nonlinear */
        "0"                      /* 32    shaping */
        "1"                      /* 33    acknowledge */
        "0"                      /* 34    start */
        "0"                      /* 35    reserved */
        "1111111111100"          /* 36:48 4800 .. 28800 set, 31200 and 33600 clear */
        "0"                      /* 49    reserved */
        "0"                      /* 50    reserved */
        "0"                      /* 51    start */
        "0000000000000000"       /* 52:67 reserved */
        "0"                      /* 68    start */
        "cccccccccccccccc"       /* 69:84 CRC */
        "0"), 0);                /* 85    fill */
    fig14_init(&f);
    fig14_range(&f, b, 18, 33); fig14_range(&f, b, 35, 50); fig14_range(&f, b, 52, 67);
    CHECK_EQ_I(fld(b, 69, 84), fig14_val(&f));
    CHECK_EQ_I(b[86], 9);

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v90_mp_parse(b, 86, &out), 86);
    CHECK_EQ_I(out.type, 0); CHECK_EQ_I(out.max_rate, 12); CHECK_EQ_I(out.trellis, 1);
    CHECK_EQ_I(out.nonlinear, 1); CHECK_EQ_I(out.shaping, 0); CHECK_EQ_I(out.ack, 1);
    CHECK_EQ_I(out.rates, 0x07FF);
    for (i = 0; i < 3; i++) { CHECK_EQ_I(out.h[i][0], 0); CHECK_EQ_I(out.h[i][1], 0); }
    CHECK_EQ_I(v90_mp_parse(b, 85, &out), V90_ERR_SHORT);
    b[33] ^= 1; CHECK_EQ_I(v90_mp_parse(b, 86, &out), V90_ERR_CRC);  b[33] ^= 1;
    b[55] ^= 1; CHECK_EQ_I(v90_mp_parse(b, 86, &out), V90_ERR_CRC);  b[55] ^= 1;
    b[68] ^= 1; CHECK_EQ_I(v90_mp_parse(b, 86, &out), V90_ERR_SYNC); b[68] ^= 1;
    b[85] ^= 1; CHECK_EQ_I(v90_mp_parse(b, 86, &out), V90_ERR_SYNC); b[85] ^= 1;

    /* MP and MP' differ only in bit 33 and the CRC */
    {
        uint8_t b2[V90_MP_MAX_BITS];
        int d = 0;
        mp.ack = 0;
        CHECK_EQ_I(v90_mp_build(&mp, b2), 86);
        for (i = 0; i < 69; i++) if (b[i] != b2[i]) { d++; CHECK_EQ_I(i, 33); }
        CHECK_EQ_I(d, 1);
        CHECK(fld(b, 69, 84) != fld(b2, 69, 84));
    }

    /* Type 1 with precoder coefficients */
    memset(&mp, 0, sizeof mp);
    mp.type = 1; mp.max_rate = 14; mp.trellis = 2; mp.nonlinear = 0; mp.shaping = 1; mp.ack = 0;
    mp.rates = 0x1FFF;
    mp.h[0][0] = 0x1234;  mp.h[0][1] = -2;
    mp.h[1][0] = -32768;  mp.h[1][1] = 32767;
    mp.h[2][0] = 0x0001;  mp.h[2][1] = (int16_t)0x8001;
    memset(b, 9, sizeof b);
    n = v90_mp_build(&mp, b);
    CHECK_EQ_I(n, 188);
    for (i = 0; i <= 16; i++) CHECK_EQ_I(b[i], 1);
    for (i = 0; i < 10; i++) CHECK_EQ_I(b[start1[i]], 0);
    CHECK_EQ_I(b[18], 1);
    CHECK_EQ_I(fld(b, 19, 23), 0);
    CHECK_EQ_I(fld(b, 24, 27), 14);
    CHECK_EQ_I(b[28], 0);
    CHECK_EQ_I(fld(b, 29, 30), 2);
    CHECK_EQ_I(b[31], 0); CHECK_EQ_I(b[32], 1); CHECK_EQ_I(b[33], 0);
    CHECK_EQ_I(b[35], 0);
    CHECK_EQ_I(fld(b, 36, 48), 0x1FFF);
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
    CHECK_EQ_I(v90_mp_parse(b, 188, &out), 188);
    CHECK_EQ_I(out.type, 1); CHECK_EQ_I(out.max_rate, 14); CHECK_EQ_I(out.trellis, 2);
    CHECK_EQ_I(out.shaping, 1); CHECK_EQ_I(out.ack, 0); CHECK_EQ_I(out.rates, 0x1FFF);
    CHECK_EQ_I(out.h[0][0], 0x1234); CHECK_EQ_I(out.h[0][1], -2);
    CHECK_EQ_I(out.h[1][0], -32768); CHECK_EQ_I(out.h[1][1], 32767);
    CHECK_EQ_I(out.h[2][0], 1);      CHECK_EQ_I(out.h[2][1], -32767);
    CHECK_EQ_I(v90_mp_parse(b, 187, &out), V90_ERR_SHORT);
    CHECK_EQ_I(v90_mp_parse(b, 86, &out), V90_ERR_SHORT);
    b[140] ^= 1; CHECK_EQ_I(v90_mp_parse(b, 188, &out), V90_ERR_CRC); b[140] ^= 1;

    mp.max_rate = 16; CHECK_EQ_I(v90_mp_build(&mp, b), V90_ERR_RANGE); mp.max_rate = 14;
    mp.trellis = 4;   CHECK_EQ_I(v90_mp_build(&mp, b), V90_ERR_RANGE); mp.trellis = 0;
    mp.rates = 0x2000; CHECK_EQ_I(v90_mp_build(&mp, b), V90_ERR_RANGE);
}

/* ---- DIL descriptor ------------------------------------------------------ */

static void test_dil(void)
{
    static v90_dil_t d, out;
    static uint8_t b[V90_DIL_MAX_BITS + 8];
    fig14_t f;
    int n, i, k;

    /* LSP = 20, LTP = 16, N = 3:
     *   alpha = ceil(20/16)*17 = 34, beta = alpha + ceil(16/16)*17 = 51
     *   length = 205 + beta + ceil(3/2)*17 = 290 (even, no extra fill) */
    memset(&d, 0, sizeof d);
    d.n = 3; d.lsp = 20; d.ltp = 16;
    d.sp[0] = 0xA5; d.sp[1] = 0x3C; d.sp[2] = 0x09;      /* SP = 0x93CA5 (20 bits) */
    d.tp[0] = 0x0F; d.tp[1] = 0xF0;                      /* TP = 0xF00F */
    for (i = 0; i < 8; i++) { d.h[i] = (uint8_t)(10 + i); d.ref[i] = (uint8_t)(100 + i); }
    d.train[0] = 5; d.train[1] = 77; d.train[2] = 127;
    memset(b, 9, sizeof b);
    n = v90_dil_build(&d, b);
    CHECK_EQ_I(n, 290);
    for (i = 0; i <= 16; i++) CHECK_EQ_I(b[i], 1);
    CHECK_EQ_I(b[17], 0);
    CHECK_EQ_I(fld(b, 18, 25), 3);                       /* N */
    CHECK_EQ_I(fld(b, 26, 33), 0);
    CHECK_EQ_I(b[34], 0);
    CHECK_EQ_I(fld(b, 35, 41), 19);                      /* LSP - 1 */
    CHECK_EQ_I(b[42], 0);
    CHECK_EQ_I(fld(b, 43, 49), 15);                      /* LTP - 1 */
    CHECK_EQ_I(b[50], 0);
    CHECK_EQ_I(b[51], 0);
    CHECK_EQ_I(fld(b, 52, 67), 0x3CA5);                  /* SP bits 0..15 */
    CHECK_EQ_I(b[68], 0);
    CHECK_EQ_I(fld(b, 69, 84), 0x0009);                  /* SP bits 16..19, zero padded */
    CHECK_EQ_I(b[51 + 34], 0);                           /* 51 + alpha */
    CHECK_EQ_I(fld(b, 52 + 34, 67 + 34), 0xF00F);        /* TP */
    CHECK_EQ_I(b[51 + 51], 0);                           /* 51 + beta */
    CHECK_EQ_I(fld(b, 52 + 51, 58 + 51), 10);            /* H1 */
    CHECK_EQ_I(b[59 + 51], 0);
    CHECK_EQ_I(fld(b, 60 + 51, 66 + 51), 11);            /* H2 */
    CHECK_EQ_I(b[67 + 51], 0);
    CHECK_EQ_I(b[68 + 51], 0);
    CHECK_EQ_I(fld(b, 69 + 51, 75 + 51), 12);            /* H3 */
    CHECK_EQ_I(fld(b, 77 + 51, 83 + 51), 13);            /* H4 */
    CHECK_EQ_I(b[85 + 51], 0);
    CHECK_EQ_I(fld(b, 86 + 51, 92 + 51), 14);            /* H5 */
    CHECK_EQ_I(fld(b, 94 + 51, 100 + 51), 15);           /* H6 */
    CHECK_EQ_I(b[102 + 51], 0);
    CHECK_EQ_I(fld(b, 103 + 51, 109 + 51), 16);          /* H7 */
    CHECK_EQ_I(b[110 + 51], 0);
    CHECK_EQ_I(fld(b, 111 + 51, 117 + 51), 17);          /* H8 */
    CHECK_EQ_I(b[118 + 51], 0);
    CHECK_EQ_I(b[119 + 51], 0);
    CHECK_EQ_I(fld(b, 120 + 51, 126 + 51), 100);         /* REF1 */
    CHECK_EQ_I(b[127 + 51], 0);
    CHECK_EQ_I(fld(b, 128 + 51, 134 + 51), 101);         /* REF2 */
    CHECK_EQ_I(b[136 + 51], 0);
    CHECK_EQ_I(fld(b, 137 + 51, 143 + 51), 102);         /* REF3 */
    CHECK_EQ_I(fld(b, 145 + 51, 151 + 51), 103);         /* REF4 */
    CHECK_EQ_I(b[153 + 51], 0);
    CHECK_EQ_I(fld(b, 154 + 51, 160 + 51), 104);         /* REF5 */
    CHECK_EQ_I(fld(b, 162 + 51, 168 + 51), 105);         /* REF6 */
    CHECK_EQ_I(b[170 + 51], 0);
    CHECK_EQ_I(fld(b, 171 + 51, 177 + 51), 106);         /* REF7 */
    CHECK_EQ_I(fld(b, 179 + 51, 185 + 51), 107);         /* REF8 */
    CHECK_EQ_I(b[186 + 51], 0);
    CHECK_EQ_I(b[187 + 51], 0);
    CHECK_EQ_I(fld(b, 188 + 51, 194 + 51), 5);           /* 1st training Ucode */
    CHECK_EQ_I(b[195 + 51], 0);
    CHECK_EQ_I(fld(b, 196 + 51, 202 + 51), 77);          /* 2nd */
    CHECK_EQ_I(b[203 + 51], 0);
    CHECK_EQ_I(b[204 + 51], 0);                          /* start */
    CHECK_EQ_I(fld(b, 205 + 51, 211 + 51), 127);         /* 3rd */
    CHECK_EQ_I(fld(b, 212 + 51, 220 + 51), 0);           /* 9 reserved bits, N odd */
    CHECK_EQ_I(b[187 + 51 + 34], 0);                     /* start before CRC */
    fig14_init(&f);
    for (k = 18; k < 187 + 51 + 34; k += 17) fig14_range(&f, b, k, k + 15);
    CHECK_EQ_I(fld(b, 188 + 51 + 34, 203 + 51 + 34), fig14_val(&f));
    CHECK_EQ_I(b[204 + 51 + 34], 0);                     /* fill */
    CHECK_EQ_I(b[290], 9);

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v90_dil_parse(b, 290, &out), 290);
    CHECK_EQ_I(out.n, 3); CHECK_EQ_I(out.lsp, 20); CHECK_EQ_I(out.ltp, 16);
    CHECK_EQ_I(out.sp[0], 0xA5); CHECK_EQ_I(out.sp[1], 0x3C); CHECK_EQ_I(out.sp[2], 0x09);
    CHECK_EQ_I(out.sp[3], 0);
    CHECK_EQ_I(out.tp[0], 0x0F); CHECK_EQ_I(out.tp[1], 0xF0); CHECK_EQ_I(out.tp[2], 0);
    for (i = 0; i < 8; i++) { CHECK_EQ_I(out.h[i], 10 + i); CHECK_EQ_I(out.ref[i], 100 + i); }
    CHECK_EQ_I(out.train[0], 5); CHECK_EQ_I(out.train[1], 77); CHECK_EQ_I(out.train[2], 127);
    CHECK_EQ_I(v90_dil_parse(b, 289, &out), V90_ERR_SHORT);
    CHECK_EQ_I(v90_dil_parse(b, 40, &out), V90_ERR_SHORT);
    b[70] ^= 1;  CHECK_EQ_I(v90_dil_parse(b, 290, &out), V90_ERR_CRC);  b[70] ^= 1;
    b[265] ^= 1; CHECK_EQ_I(v90_dil_parse(b, 290, &out), V90_ERR_CRC);  b[265] ^= 1;  /* reserved */
    b[85] ^= 1;  CHECK_EQ_I(v90_dil_parse(b, 290, &out), V90_ERR_SYNC); b[85] ^= 1;
    b[289] ^= 1; CHECK_EQ_I(v90_dil_parse(b, 290, &out), V90_ERR_SYNC); b[289] ^= 1;
    CHECK_EQ_I(v90_find_sync(b, 290, 1), -1);            /* no false sync inside */

    /* N = 0: LSP = LTP = 1, alpha = 17, beta = 34, 205 + 34 = 239 bits, odd,
     * so one more fill bit: 240. Start bit before the CRC at 187 + 34 = 221. */
    memset(&d, 0, sizeof d);
    d.n = 0; d.lsp = 1; d.ltp = 1;
    memset(b, 9, sizeof b);
    n = v90_dil_build(&d, b);
    CHECK_EQ_I(n, 240);
    CHECK_EQ_I(fld(b, 18, 25), 0);
    CHECK_EQ_I(fld(b, 35, 41), 0); CHECK_EQ_I(fld(b, 43, 49), 0);
    CHECK_EQ_I(b[221], 0);
    fig14_init(&f);
    for (k = 18; k < 221; k += 17) fig14_range(&f, b, k, k + 15);
    CHECK_EQ_I(fld(b, 222, 237), fig14_val(&f));
    CHECK_EQ_I(b[238], 0); CHECK_EQ_I(b[239], 0); CHECK_EQ_I(b[240], 9);
    CHECK_EQ_I(v90_dil_parse(b, 240, &out), 240);
    CHECK_EQ_I(out.n, 0); CHECK_EQ_I(out.lsp, 1); CHECK_EQ_I(out.ltp, 1);
    CHECK_EQ_I(v90_dil_parse(b, 239, &out), V90_ERR_SHORT);

    /* largest: N = 255, LSP = LTP = 128: beta = 272,
     * 205 + 272 + 128*17 = 2653, odd -> 2654 */
    memset(&d, 0, sizeof d);
    d.n = 255; d.lsp = 128; d.ltp = 128;
    for (i = 0; i < 16; i++) { d.sp[i] = (uint8_t)(0x11 * i + 1); d.tp[i] = (uint8_t)(0xFF - 3 * i); }
    for (i = 0; i < 8; i++) { d.h[i] = 127; d.ref[i] = (uint8_t)(16 * i + 15); }
    for (i = 0; i < 255; i++) d.train[i] = (uint8_t)((i * 37) & 127);
    memset(b, 9, sizeof b);
    n = v90_dil_build(&d, b);
    CHECK_EQ_I(n, 2654);
    CHECK_EQ_I(n, V90_DIL_MAX_BITS);
    CHECK_EQ_I(b[2654], 9);
    CHECK_EQ_I(v90_dil_parse(b, n, &out), 2654);
    CHECK_EQ_I(memcmp(&d, &out, sizeof d), 0);
    CHECK_EQ_I(v90_find_sync(b, n, 1), -1);

    /* random round trips */
    {
        uint32_t seed = 99;
        int trial;
        for (trial = 0; trial < 200; trial++) {
            int bad;
            memset(&d, 0, sizeof d);
            d.n = (uint8_t)(lcg(&seed) % 256);
            d.lsp = (uint8_t)(1 + lcg(&seed) % 128);
            d.ltp = (uint8_t)(1 + lcg(&seed) % 128);
            for (i = 0; i < d.lsp; i++) if (lcg(&seed) & 1) setbit(d.sp, i);
            for (i = 0; i < d.ltp; i++) if (lcg(&seed) & 1) setbit(d.tp, i);
            for (i = 0; i < 8; i++) { d.h[i] = (uint8_t)(lcg(&seed) & 127); d.ref[i] = (uint8_t)(lcg(&seed) & 127); }
            for (i = 0; i < d.n; i++) d.train[i] = (uint8_t)(lcg(&seed) & 127);
            n = v90_dil_build(&d, b);
            /* length per Table 12 */
            k = 205 + ((d.lsp + 15) / 16 + (d.ltp + 15) / 16) * 17 + ((d.n + 1) / 2) * 17;
            if (k & 1) k++;
            bad = (n != k);
            bad |= (v90_dil_parse(b, n, &out) != n);
            bad |= (memcmp(&d, &out, sizeof d) != 0);
            /* a single bit error anywhere in the body is caught */
            i = 18 + (int)(lcg(&seed) % (unsigned)(n - 20));
            b[i] ^= 1;
            bad |= (v90_dil_parse(b, n, &out) >= 0);
            CHECK_EQ_I(bad, 0);
        }
    }
    d.lsp = 0;   CHECK_EQ_I(v90_dil_build(&d, b), V90_ERR_RANGE); d.lsp = 1;
    d.ltp = 129; CHECK_EQ_I(v90_dil_build(&d, b), V90_ERR_RANGE); d.ltp = 1;
    d.h[2] = 128; CHECK_EQ_I(v90_dil_build(&d, b), V90_ERR_RANGE);
}

/* ---- CP ------------------------------------------------------------------ */

static void test_cp(void)
{
    static v90_cp_t cp, out;
    static uint8_t b[V90_CP_MAX_BITS + 8];
    fig14_t f;
    int n, i, k, c;

    /* One constellation, no codec constellations: gamma = 0, delta = 0,
     * length 292. */
    memset(&cp, 0, sizeof cp);
    cp.is_cp = 1; cp.drn = 22; cp.silence = 0; cp.sr = 2; cp.ack = 1; cp.alaw = 1;
    cp.rates = 0x0155; cp.ld = 3; cp.rms_ratio = 0x2000;     /* 1.0 in Q3.13 */
    cp.a1 = 64; cp.a2 = -64; cp.b1 = -1; cp.b2 = 33;
    cp.codec_differs = 0;
    setbit(cp.mask[0], 0); setbit(cp.mask[0], 15);            /* chord 1: 0x8001 */
    setbit(cp.mask[0], 17);                                   /* chord 2: 0x0002 */
    setbit(cp.mask[0], 47);                                   /* chord 3: 0x8000 */
    setbit(cp.mask[0], 48);                                   /* chord 4: 0x0001 */
    setbit(cp.mask[0], 64 + 4);                               /* chord 5: 0x0010 */
    setbit(cp.mask[0], 80 + 8);                               /* chord 6: 0x0100 */
    setbit(cp.mask[0], 96 + 12);                              /* chord 7: 0x1000 */
    setbit(cp.mask[0], 112); setbit(cp.mask[0], 127);         /* chord 8: 0x8001 */
    memset(b, 9, sizeof b);
    n = v90_cp_build(&cp, b);
    CHECK_EQ_I(n, 292);
    CHECK_EQ_I(v90_cp_nconst(&cp), 1);
    for (i = 0; i <= 16; i++) CHECK_EQ_I(b[i], 1);
    CHECK_EQ_I(b[17], 0);
    CHECK_EQ_I(b[18], 0);
    CHECK_EQ_I(b[19], 1);                                /* CP, not CPt */
    CHECK_EQ_I(fld(b, 20, 24), 22);
    CHECK_EQ_I(fld(b, 25, 29), 0);
    CHECK_EQ_I(b[30], 0);
    CHECK_EQ_I(fld(b, 31, 32), 2);
    CHECK_EQ_I(b[33], 1);
    CHECK_EQ_I(b[34], 0);
    CHECK_EQ_I(b[35], 1);                                /* A-law */
    CHECK_EQ_I(fld(b, 36, 48), 0x0155);
    CHECK_EQ_I(fld(b, 49, 50), 3);
    CHECK_EQ_I(b[51], 0);
    CHECK_EQ_I(fld(b, 52, 67), 0x2000);
    CHECK_EQ_I(b[68], 0);
    CHECK_EQ_I(fld(b, 69, 76), 0x40);                    /* a1 = +1.0 */
    CHECK_EQ_I(fld(b, 77, 84), 0xC0);                    /* a2 = -1.0 */
    CHECK_EQ_I(b[85], 0);
    CHECK_EQ_I(fld(b, 86, 93), 0xFF);                    /* b1 = -1/64 */
    CHECK_EQ_I(fld(b, 94, 101), 33);
    CHECK_EQ_I(b[102], 0);
    CHECK_EQ_I(fld(b, 103, 118), 0);                     /* indices 0..3 */
    CHECK_EQ_I(b[119], 0);
    CHECK_EQ_I(fld(b, 120, 127), 0);                     /* indices 4, 5 */
    CHECK_EQ_I(b[128], 0);
    CHECK_EQ_I(fld(b, 129, 135), 0);
    CHECK_EQ_I(b[136], 0);  CHECK_EQ_I(fld(b, 137, 152), 0x8001);
    CHECK_EQ_I(b[153], 0);  CHECK_EQ_I(fld(b, 154, 169), 0x0002);
    CHECK_EQ_I(b[170], 0);  CHECK_EQ_I(fld(b, 171, 186), 0x8000);
    CHECK_EQ_I(b[187], 0);  CHECK_EQ_I(fld(b, 188, 203), 0x0001);
    CHECK_EQ_I(b[204], 0);  CHECK_EQ_I(fld(b, 205, 220), 0x0010);
    CHECK_EQ_I(b[221], 0);  CHECK_EQ_I(fld(b, 222, 237), 0x0100);
    CHECK_EQ_I(b[238], 0);  CHECK_EQ_I(fld(b, 239, 254), 0x1000);
    CHECK_EQ_I(b[255], 0);  CHECK_EQ_I(fld(b, 256, 271), 0x8001);
    CHECK_EQ_I(b[272], 0);                               /* 272 + delta */
    fig14_init(&f);
    for (k = 18; k < 272; k += 17) fig14_range(&f, b, k, k + 15);
    CHECK_EQ_I(fld(b, 273, 288), fig14_val(&f));
    CHECK_EQ_I(fld(b, 289, 291), 0);
    CHECK_EQ_I(b[292], 9);

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v90_cp_parse(b, 292, &out), 292);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);
    CHECK_EQ_I(v90_cp_parse(b, 288, &out), V90_ERR_SHORT);     /* the CRC is not all there */
    CHECK_EQ_I(v90_cp_parse(b, 100, &out), V90_ERR_SHORT);
    {
        /* A CPt as a Conexant CX93001 sends it (recorded on the box,
         * 2026-11-15): 290 bits, ONE fill bit where Table 14 has three, and
         * the next frame sync right behind it. It is taken as it is: the
         * fill bits that are there count, up to three. */
        static const char real[] =
            "11111111111111111000100100000001000011111111111111000000000000000100000000010000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000001000000000001000000010000000000000000000000000000000000000000000011001011011001010"
            "11111111111111111" "0";
        uint8_t rb[320]; int i, n = (int)sizeof real - 1;
        for (i = 0; i < n; i++) rb[i] = (uint8_t)(real[i] - '0');
        CHECK_EQ_I(v90_cp_parse(rb, n, &out), 290);
        CHECK_EQ_I(out.is_cp, 0); CHECK_EQ_I(out.drn, 9); CHECK_EQ_I(out.sr, 1); CHECK_EQ_I(out.ld, 1);
        CHECK_EQ_I(out.rates, 0x1FFF); CHECK_EQ_I(out.rms_ratio, 0x2000); CHECK_EQ_I(out.a1, 64); CHECK_EQ_I(out.a2, 0);
        /* its training constellation: Ucodes 44, 67, 79 and 86 */
        CHECK_EQ_I(out.mask[0][5], 1 << 4); CHECK_EQ_I(out.mask[0][8], 1 << 3); CHECK_EQ_I(out.mask[0][9], 1 << 7); CHECK_EQ_I(out.mask[0][10], 1 << 6);
        CHECK_EQ_I(v90_cp_parse(rb, 289, &out), 289);           /* cut right after the CRC */
        CHECK_EQ_I(v90_cp_parse(b, 291, &out), 291);
    }
    b[19] ^= 1;  CHECK_EQ_I(v90_cp_parse(b, 292, &out), V90_ERR_CRC);  b[19] ^= 1;
    b[130] ^= 1; CHECK_EQ_I(v90_cp_parse(b, 292, &out), V90_ERR_CRC);  b[130] ^= 1;
    b[280] ^= 1; CHECK_EQ_I(v90_cp_parse(b, 292, &out), V90_ERR_CRC);  b[280] ^= 1;
    b[153] ^= 1; CHECK_EQ_I(v90_cp_parse(b, 292, &out), V90_ERR_SYNC); b[153] ^= 1;
    b[290] ^= 1; CHECK_EQ_I(v90_cp_parse(b, 292, &out), 290); b[290] ^= 1;     /* one fill bit only */
    CHECK_EQ_I(v90_find_sync(b, 292, 1), -1);

    /* Three constellations, codec constellations present:
     *   gamma = 136 * 2 = 272, delta = 2*272 + 136 = 680, length 292 + 680 = 972
     *   constellations at 136, 272, 408; codec constellations at 544, 680, 816;
     *   start bit before the CRC at 272 + 680 = 952. */
    memset(&cp, 0, sizeof cp);
    cp.is_cp = 0; cp.drn = 1; cp.silence = 1; cp.sr = 0; cp.ack = 0; cp.alaw = 0;
    cp.rates = 0x1FFF; cp.ld = 0; cp.rms_ratio = 0xFFFF;
    cp.a1 = -128; cp.a2 = 127; cp.b1 = 0; cp.b2 = 1;
    cp.idx[0] = 0; cp.idx[1] = 1; cp.idx[2] = 2; cp.idx[3] = 0; cp.idx[4] = 1; cp.idx[5] = 2;
    cp.codec_differs = 1;
    for (c = 0; c < 3; c++)
        for (i = 0; i < 128; i++) {
            if ((i % (c + 2)) == 0) setbit(cp.mask[c], i);
            if ((i % (c + 5)) == 1) setbit(cp.codec_mask[c], i);
        }
    memset(b, 9, sizeof b);
    n = v90_cp_build(&cp, b);
    CHECK_EQ_I(n, 972);
    CHECK_EQ_I(v90_cp_nconst(&cp), 3);
    CHECK_EQ_I(b[19], 0);
    CHECK_EQ_I(fld(b, 20, 24), 1);
    CHECK_EQ_I(b[30], 1);
    CHECK_EQ_I(fld(b, 69, 76), 0x80);
    CHECK_EQ_I(fld(b, 77, 84), 0x7F);
    CHECK_EQ_I(fld(b, 103, 106), 0); CHECK_EQ_I(fld(b, 107, 110), 1);
    CHECK_EQ_I(fld(b, 111, 114), 2); CHECK_EQ_I(fld(b, 115, 118), 0);
    CHECK_EQ_I(fld(b, 120, 123), 1); CHECK_EQ_I(fld(b, 124, 127), 2);
    CHECK_EQ_I(b[128], 1);
    for (c = 0; c < 6; c++) {
        const uint8_t *m = c < 3 ? cp.mask[c] : cp.codec_mask[c - 3];
        for (k = 0; k < 8; k++) {
            int base = 136 + 136 * c + 17 * k;
            CHECK_EQ_I(b[base], 0);
            CHECK_EQ_I(fld(b, base + 1, base + 16), m[2 * k] | (m[2 * k + 1] << 8));
        }
    }
    CHECK_EQ_I(b[952], 0);
    fig14_init(&f);
    for (k = 18; k < 952; k += 17) fig14_range(&f, b, k, k + 15);
    CHECK_EQ_I(fld(b, 953, 968), fig14_val(&f));
    CHECK_EQ_I(fld(b, 969, 971), 0);
    CHECK_EQ_I(b[972], 9);
    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v90_cp_parse(b, 972, &out), 972);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);
    CHECK_EQ_I(v90_cp_parse(b, 971, &out), 971);
    CHECK_EQ_I(v90_cp_parse(b, 968, &out), V90_ERR_SHORT);
    CHECK_EQ_I(v90_find_sync(b, 972, 1), -1);

    /* largest: six constellations plus codec constellations
     *   gamma = 680, delta = 1496, length 1788 */
    for (i = 0; i < 6; i++) cp.idx[i] = (uint8_t)(5 - i);
    for (c = 3; c < 6; c++) { memset(cp.mask[c], 0xFF, 16); memset(cp.codec_mask[c], 0x81, 16); }
    n = v90_cp_build(&cp, b);
    CHECK_EQ_I(n, 1788);
    CHECK_EQ_I(n, V90_CP_MAX_BITS);
    CHECK_EQ_I(v90_cp_parse(b, n, &out), 1788);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);
    /* without codec constellations: delta = gamma = 680, length 972 */
    cp.codec_differs = 0;
    n = v90_cp_build(&cp, b);
    CHECK_EQ_I(n, 972);
    CHECK_EQ_I(v90_cp_parse(b, n, &out), 972);
    CHECK_EQ_I(out.codec_differs, 0);
    CHECK_EQ_I(memcmp(cp.mask, out.mask, sizeof cp.mask), 0);
    for (i = 0; i < 16; i++) CHECK_EQ_I(out.codec_mask[0][i], 0);   /* not carried */

    /* an index above 5 cannot be sent, and is refused when received */
    cp.idx[3] = 6; CHECK_EQ_I(v90_cp_build(&cp, b), V90_ERR_RANGE); cp.idx[3] = 2;
    cp.drn = 32;   CHECK_EQ_I(v90_cp_build(&cp, b), V90_ERR_RANGE); cp.drn = 1;
    cp.sr = 4;     CHECK_EQ_I(v90_cp_build(&cp, b), V90_ERR_RANGE); cp.sr = 0;
    n = v90_cp_build(&cp, b);
    CHECK_EQ_I(n, 972);
    b[103] = 1; b[104] = 1; b[105] = 1; b[106] = 0;      /* index 7 in interval 0 */
    CHECK_EQ_I(v90_cp_parse(b, n, &out), V90_ERR_RANGE);
}

int main(void)
{
    test_crc();
    test_sync_e();
    test_jd();
    test_mp();
    test_dil();
    test_cp();
    T_REPORT("test_seq");
}
