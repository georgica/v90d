/* V.92 sequence codecs: Ja, Jd, Jp, CPt / CPu, CPus, SUVu, SUVd, CPd.
 *
 * Expected bit positions are taken by hand from Tables 20 to 24, 27, 30 and
 * 31 of V.92 (and Table 12/V.90 for the body of the DIL descriptor); the
 * expected CRC comes from a cell-by-cell model of Figure 14/V.34 kept in
 * this file, independent of the library's register arithmetic. */
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_seq.h"

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

/* Every sequence here is 17 ones followed by 17-bit groups (a start bit 0
 * and 16 bits), the last group being the CRC whose start bit is bit number
 * `crc_start`. The CRC covers the 16 bits of each earlier group. Checks the
 * frame sync, all start bits and the CRC; returns the number of faults. */
static int frame_ok(const uint8_t *b, int crc_start)
{
    fig14_t f;
    int i, bad = 0;
    fig14_init(&f);
    for (i = 0; i < 17; i++) if (b[i] != 1) bad++;
    if ((crc_start - 17) % 17 != 0) bad++;
    for (i = 17; i < crc_start; i++) {
        if ((i - 17) % 17 == 0) { if (b[i] != 0) bad++; }
        else fig14_bit(&f, b[i]);
    }
    if (b[crc_start] != 0) bad++;
    if (fld(b, crc_start + 1, crc_start + 16) != fig14_val(&f)) bad++;
    return bad;
}

static int count_ones(const uint8_t *b, int from, int to)
{
    int i, n = 0;
    for (i = from; i <= to; i++) n += b[i];
    return n;
}

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }

/* flipping any single bit from `from` to `to` must make parse() fail */
#define CHECK_FLIPS(parse_call, b, from, to) do {                       \
        int fl_, bad_ = 0;                                              \
        for (fl_ = (from); fl_ <= (to); fl_++) {                        \
            (b)[fl_] ^= 1;                                              \
            if ((parse_call) > 0) bad_++;                               \
            (b)[fl_] ^= 1;                                              \
        }                                                               \
        CHECK_EQ_I(bad_, 0);                                            \
    } while (0)

/* ---- frame sync ---------------------------------------------------------- */

static void test_sync(void)
{
    uint8_t b[120];
    int i;
    memset(b, 0, sizeof b);
    for (i = 10; i < 27; i++) b[i] = 1;          /* 17 ones at 10..26, 0 at 27 */
    CHECK_EQ_I(v92_find_sync(b, 120, 0), 10);
    CHECK_EQ_I(v92_find_sync(b, 120, 10), 10);
    CHECK_EQ_I(v92_find_sync(b, 120, 11), -1);
    CHECK_EQ_I(v92_find_sync(b, 27, 0), -1);     /* start bit not yet available */
    CHECK_EQ_I(v92_find_sync(b, 28, 0), 10);
    b[26] = 0;                                   /* only 16 ones */
    CHECK_EQ_I(v92_find_sync(b, 120, 0), -1);

    /* Ja, 8.5.4: 24 ones, then the descriptor's own 17: a run of 41 ones.
     * The sync is the last 17 of them. */
    memset(b, 0, sizeof b);
    for (i = 5; i < 5 + 41; i++) b[i] = 1;
    CHECK_EQ_I(v92_find_sync(b, 120, 0), 5 + 24);
    CHECK_EQ_I(v92_find_sync(b, 120, 20), 5 + 24);
    CHECK_EQ_I(v92_find_sync(b, 120, 5 + 25), -1);
    /* a second sequence right behind the first */
    for (i = 60; i < 77; i++) b[i] = 1;
    CHECK_EQ_I(v92_find_sync(b, 120, 5 + 25), 60);

    /* E1u, E2u, Jp': runs of zeros */
    CHECK_EQ_I(V92_E1U_BITS, 12);
    CHECK_EQ_I(V92_E2U_BITS(2, 0), 24);          /* 12 symbols of the 4-point TRN2u */
    CHECK_EQ_I(V92_E2U_BITS(3, 0), 36);
    CHECK_EQ_I(V92_E2U_BITS(3, 1), 39);          /* CPd bit 29: one more symbol */
    CHECK_EQ_I(v92_e_match(b + 46, 14, 12), 1);  /* zeros at 46..59 */
    CHECK_EQ_I(v92_e_match(b + 46, 14, 14), 1);
    CHECK_EQ_I(v92_e_match(b + 46, 14, 15), 0);  /* not that many available */
    CHECK_EQ_I(v92_e_match(b + 46, 20, 15), 0);  /* a one at 60 */
    CHECK_EQ_I(v92_e_match(b + 45, 14, 12), 0);  /* a one at 45 */
}

/* ---- Ja (Table 20) ------------------------------------------------------- */

static void ja_example(v92_ja_t *ja)
{
    int i;
    memset(ja, 0, sizeof *ja);
    ja->dil.n = 3;
    ja->dil.lsp = 3; ja->dil.sp[0] = 0x05;       /* + - + */
    ja->dil.ltp = 2; ja->dil.tp[0] = 0x01;
    for (i = 0; i < 8; i++) { ja->dil.h[i] = (uint8_t)(i + 1); ja->dil.ref[i] = (uint8_t)(10 * (i + 1)); }
    ja->dil.train[0] = 100; ja->dil.train[1] = 101; ja->dil.train[2] = 102;
    /* 24000 (r = 0), 25333 (r = 1), 44000 (r = 15), 46666 (r = 17), 48000 (r = 18) */
    ja->rates = (1u << 0) | (1u << 1) | (1u << 15) | (1u << 17) | (1u << 18);
}

static void test_ja(void)
{
    static v92_ja_t ja, out;
    static uint8_t b[V92_JA_MAX_BITS + 16];
    int n, i;

    /* LSP = 3 and LTP = 2 take one word each: alpha = 17, beta = 34 in
     * Table 12/V.90. N = 3 takes ceil(3/2) = 2 words. Table 20/V.92:
     *   0:16 sync, 17:33 N, 34:50 LSP-1 / LTP-1, 51:67 SP, 68:84 TP,
     *   85:152 H1..H8 (H1 at 52 + beta = 86), 153:220 REF1..REF8,
     *   221:254 training Ucodes (first at 188 + beta = 222),
     *   187 + beta + 2 * 17 = 255: start bit,
     *   256:271 rates 24000 .. 44000, 272 start bit, 273:288 rates 45333 ..
     *   and reserved, 289 start bit, 290:305 CRC, 306 fill bit;
     *   307 bits, filled with zeros to 312 = 26 * 12. */
    ja_example(&ja);
    memset(b, 9, sizeof b);
    n = v92_ja_build(&ja, b);
    CHECK_EQ_I(n, 312);
    CHECK_EQ_I(frame_ok(b, 289), 0);
    CHECK_EQ_I(fld(b, 18, 25), 3);
    CHECK_EQ_I(fld(b, 26, 33), 0);
    CHECK_EQ_I(fld(b, 35, 41), 2);               /* LSP - 1 */
    CHECK_EQ_I(b[42], 0);
    CHECK_EQ_I(fld(b, 43, 49), 1);               /* LTP - 1 */
    CHECK_EQ_I(b[50], 0);
    CHECK_EQ_I(fld(b, 52, 67), 0x0005);
    CHECK_EQ_I(fld(b, 69, 84), 0x0001);
    CHECK_EQ_I(fld(b, 86, 92), 1);   CHECK_EQ_I(fld(b, 94, 100), 2);
    CHECK_EQ_I(fld(b, 103, 109), 3); CHECK_EQ_I(fld(b, 111, 117), 4);
    CHECK_EQ_I(fld(b, 120, 126), 5); CHECK_EQ_I(fld(b, 128, 134), 6);
    CHECK_EQ_I(fld(b, 137, 143), 7); CHECK_EQ_I(fld(b, 145, 151), 8);
    CHECK_EQ_I(fld(b, 154, 160), 10); CHECK_EQ_I(fld(b, 162, 168), 20);
    CHECK_EQ_I(fld(b, 171, 177), 30); CHECK_EQ_I(fld(b, 179, 185), 40);
    CHECK_EQ_I(fld(b, 188, 194), 50); CHECK_EQ_I(fld(b, 196, 202), 60);
    CHECK_EQ_I(fld(b, 205, 211), 70); CHECK_EQ_I(fld(b, 213, 219), 80);
    CHECK_EQ_I(fld(b, 222, 228), 100); CHECK_EQ_I(fld(b, 230, 236), 101);
    CHECK_EQ_I(fld(b, 239, 245), 102); CHECK_EQ_I(fld(b, 246, 254), 0);
    CHECK_EQ_I(match(b + 256,
        "1100000000000001"       /* 256:271  24000, 25333, 44000 */
        "0"                      /* 272      start bit */
        "011"                    /* 273:275  46666, 48000 */
        "0000000000000"          /* 276:288  reserved */
        "0"                      /* 289      start bit */
        "cccccccccccccccc"       /* 290:305  CRC */
        "0"                      /* 306      fill bit */
        "00000"), 0);            /* 307:311  fill to a multiple of 12 */
    CHECK_EQ_I(b[312], 9);                       /* nothing written beyond */

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v92_ja_parse(b, 312, &out), 312);
    CHECK_EQ_I(memcmp(&ja, &out, sizeof ja), 0);
    CHECK_EQ_I(v92_ja_parse(b, 305, &out), V92_ERR_SHORT);
    /* terminated early at the CRC's end: the fill is not required */
    CHECK_EQ_I(v92_ja_parse(b, 306, &out), 306);
    CHECK_FLIPS(v92_ja_parse(b, 312, &out), b, 0, 305);

    /* 8.5.4: "When N = 0, the DIL descriptor is 276 bits long" (with one SP
     * and one TP word: 239 + 34 = 273, filled to 276) */
    ja.dil.n = 0;
    n = v92_ja_build(&ja, b);
    CHECK_EQ_I(n, 276);
    CHECK_EQ_I(frame_ok(b, 255), 0);
    CHECK_EQ_I(fld(b, 222, 237), 0x8003);
    CHECK_EQ_I(fld(b, 239, 254), 0x0006);
    CHECK_EQ_I(v92_ja_parse(b, 276, &out), 276);
    CHECK_EQ_I(out.dil.n, 0);
    CHECK_EQ_I((int)out.rates, (int)ja.rates);

    /* the largest: N = 255, LSP = LTP = 128: beta = 272,
     * 239 + 272 + 128 * 17 = 2687, filled to 2688 */
    memset(&ja, 0, sizeof ja);
    ja.dil.n = 255; ja.dil.lsp = 128; ja.dil.ltp = 128;
    for (i = 0; i < 16; i++) { ja.dil.sp[i] = (uint8_t)(0x35 + i); ja.dil.tp[i] = (uint8_t)(0xC1 - i); }
    for (i = 0; i < 8; i++) { ja.dil.h[i] = (uint8_t)(127 - i); ja.dil.ref[i] = (uint8_t)(i * 17); }
    for (i = 0; i < 255; i++) ja.dil.train[i] = (uint8_t)(i & 127);
    ja.rates = 0x7FFFF;
    n = v92_ja_build(&ja, b);
    CHECK_EQ_I(n, 2688);
    CHECK_EQ_I(frame_ok(b, 221 + 272 + 128 * 17), 0);
    CHECK_EQ_I(v92_ja_parse(b, n, &out), 2688);
    CHECK_EQ_I(memcmp(&ja, &out, sizeof ja), 0);

    /* not encodable */
    ja.rates = 1u << 19;
    CHECK_EQ_I(v92_ja_build(&ja, b), V92_ERR_RANGE);
    ja.rates = 1; ja.dil.lsp = 0;
    CHECK_EQ_I(v92_ja_build(&ja, b), V92_ERR_RANGE);
    ja.dil.lsp = 1; ja.dil.h[2] = 128;
    CHECK_EQ_I(v92_ja_build(&ja, b), V92_ERR_RANGE);
}

/* ---- Jd, Jp (Tables 21, 22) --------------------------------------------- */

static void test_jd_jp(void)
{
    v92_jd_t jd, jdo;
    v92_jp_t jp, jpo;
    uint8_t b[V92_J_BITS + 8], b2[V92_J_BITS + 8];
    int n;

    /* Jd: 28000 (r = 0), 48000 (r = 15), 56000 (r = 21), look-ahead 3 */
    memset(&jd, 0, sizeof jd);
    jd.rates = (1u << 0) | (1u << 15) | (1u << 21);
    jd.max_ld = 3;
    memset(b, 9, sizeof b);
    n = v92_jd_build(&jd, b);
    CHECK_EQ_I(n, 72);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "1000000000000001"       /* 18:33 28000 .. 48000 */
        "0"                      /* 34    start bit */
        "000001"                 /* 35:40 49333 .. 56000 */
        "000000"                 /* 41:46 reserved */
        "0"                      /* 47    Jd */
        "0"                      /* 48    reserved */
        "11"                     /* 49:50 look-ahead 3 */
        "0"                      /* 51    start bit */
        "cccccccccccccccc"       /* 52:67 CRC */
        "0000"), 0);             /* 68:71 fill */
    CHECK_EQ_I(frame_ok(b, 51), 0);
    CHECK_EQ_I(b[72], 9);
    CHECK_EQ_I(v92_j_kind(b, 72), V92_J_JD);
    CHECK_EQ_I(v92_j_kind(b, 47), V92_ERR_SHORT);
    memset(&jdo, 0xEE, sizeof jdo);
    CHECK_EQ_I(v92_jd_parse(b, 72, &jdo), 72);
    CHECK_EQ_I((int)jdo.rates, (int)jd.rates);
    CHECK_EQ_I(jdo.max_ld, 3);
    CHECK_EQ_I(v92_jp_parse(b, 72, &jpo), V92_ERR_KIND);
    CHECK_EQ_I(v92_jd_parse(b, 67, &jdo), V92_ERR_SHORT);
    CHECK_FLIPS(v92_jd_parse(b, 72, &jdo), b, 0, 67);
    jd.rates = 1u << 22;
    CHECK_EQ_I(v92_jd_build(&jd, b2), V92_ERR_RANGE);
    jd.rates = 1; jd.max_ld = 4;
    CHECK_EQ_I(v92_jd_build(&jd, b2), V92_ERR_RANGE);

    /* Jp: extend by 0xC001 / 65536 of a symbol, 8-point TRN2u in training,
     * 4-point in rate renegotiation */
    memset(&jp, 0, sizeof jp);
    jp.frac = 0xC001; jp.pt8_train = 1; jp.pt8_reneg = 0;
    memset(b, 9, sizeof b);
    n = v92_jp_build(&jp, b);
    CHECK_EQ_I(n, 72);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "1000000000000011"       /* 18:33 0xC001, LSB first */
        "0"                      /* 34    start bit */
        "000000000000"           /* 35:46 reserved */
        "1"                      /* 47    Jp */
        "1"                      /* 48    8-point in training */
        "0"                      /* 49    4-point in rate renegotiation */
        "0"                      /* 50    reserved */
        "0"                      /* 51    start bit */
        "cccccccccccccccc"       /* 52:67 CRC */
        "0000"), 0);             /* 68:71 fill */
    CHECK_EQ_I(frame_ok(b, 51), 0);
    CHECK_EQ_I(b[72], 9);
    CHECK_EQ_I(v92_j_kind(b, 72), V92_J_JP);
    memset(&jpo, 0xEE, sizeof jpo);
    CHECK_EQ_I(v92_jp_parse(b, 72, &jpo), 72);
    CHECK_EQ_I(jpo.frac, 0xC001);
    CHECK_EQ_I(jpo.pt8_train, 1);
    CHECK_EQ_I(jpo.pt8_reneg, 0);
    CHECK_EQ_I(v92_jd_parse(b, 72, &jdo), V92_ERR_KIND);
    CHECK_FLIPS(v92_jp_parse(b, 72, &jpo), b, 0, 67);
    jp.pt8_train = 0; jp.pt8_reneg = 1; jp.frac = 0;
    CHECK_EQ_I(v92_jp_build(&jp, b), 72);
    CHECK_EQ_I(b[48], 0);
    CHECK_EQ_I(b[49], 1);
    CHECK_EQ_I(count_ones(b, 18, 50), 2);        /* bits 47 and 49 only */
}

/* ---- CPt, CPu (Table 23) ------------------------------------------------- */

static void cpu_example(v92_cpu_t *cp)
{
    memset(cp, 0, sizeof *cp);
    cp->type = V92_UP_CPU;
    cp->drn = 22;                /* (22 + 20) * 8000 / 6 = 56000 */
    cp->sr = 2; cp->ack = 1; cp->alaw = 1; cp->ld = 3;
    cp->rms_ratio = 0x2000;      /* 1.0 in Q3.13 */
    cp->a1 = -32;                /* -0.5 in Q1.6: 0xE0 */
    cp->a2 = 5; cp->b1 = 100; cp->b2 = -1;
    cp->idx[1] = 1; cp->idx[3] = 1; cp->idx[5] = 1;
    cp->codec_differs = 1;
    setbit(cp->mask[0], 0); setbit(cp->mask[0], 16); setbit(cp->mask[0], 127);
    setbit(cp->mask[1], 5); setbit(cp->mask[1], 112);
    setbit(cp->codec_mask[0], 1);
    setbit(cp->codec_mask[1], 126);
}

static void test_cpu(void)
{
    static v92_cpu_t cp, out;
    static v90_cp_t v90;
    static uint8_t b[V92_CPU_MAX_BITS + 16];
    int n, i;

    /* Two constellations and their codec counterparts: gamma = 136,
     * delta = 2 * 136 + 136 = 408. Constellation 0 at 136:271, 1 at 272:407,
     * codec constellation 0 at 408:543, 1 at 544:679; start bit 272 + delta
     * = 680, CRC 681:696, fill bit 697: 698 bits. Within a constellation
     * that starts at s, Ucode u is bit s + 1 + 17 * (u / 16) + u % 16. */
    cpu_example(&cp);
    memset(b, 9, sizeof b);
    n = v92_cpu_build(&cp, 1, b);
    CHECK_EQ_I(n, 698);
    CHECK_EQ_I(frame_ok(b, 680), 0);
    CHECK_EQ_I(match(b + 17,
        "0"                      /* 17    start bit */
        "0"                      /* 18    CP */
        "10"                     /* 19:20 type 1 = CPu */
        "01101"                  /* 21:25 drn = 22 */
        "00000"                  /* 26:30 reserved */
        "01"                     /* 31:32 Sr = 2 */
        "1"                      /* 33    acknowledge */
        "0"                      /* 34    start bit */
        "1"                      /* 35    A-law */
        "0000000000000"          /* 36:48 reserved */
        "11"                     /* 49:50 ld = 3 */
        "0"                      /* 51    start bit */
        "0000000000000100"       /* 52:67 0x2000 */
        "0"                      /* 68    start bit */
        "00000111"               /* 69:76 a1 = 0xE0 */
        "10100000"               /* 77:84 a2 = 5 */
        "0"                      /* 85    start bit */
        "00100110"               /* 86:93 b1 = 100 = 0x64 */
        "11111111"               /* 94:101 b2 = -1 */
        "0"                      /* 102   start bit */
        "0000" "1000" "0000" "1000"   /* 103:118 intervals 0..3 */
        "0"                      /* 119   start bit */
        "0000" "1000"            /* 120:127 intervals 4, 5 */
        "1"                      /* 128   codec constellations follow */
        "0000000"                /* 129:135 reserved */
        "0"), 0);                /* 136   start bit */
    CHECK_EQ_I(b[137], 1);                       /* constellation 0: Ucode 0 */
    CHECK_EQ_I(b[154], 1);                       /* Ucode 16 */
    CHECK_EQ_I(b[271], 1);                       /* Ucode 127 */
    CHECK_EQ_I(b[272 + 1 + 5], 1);               /* constellation 1: Ucode 5 */
    CHECK_EQ_I(b[272 + 1 + 17 * 7], 1);          /* Ucode 112 */
    CHECK_EQ_I(b[408 + 1 + 1], 1);               /* codec constellation 0: Ucode 1 */
    CHECK_EQ_I(b[544 + 1 + 17 * 7 + 14], 1);     /* codec constellation 1: Ucode 126 */
    CHECK_EQ_I(count_ones(b, 136, 680), 7);
    CHECK_EQ_I(b[697], 0);
    CHECK_EQ_I(b[698], 9);
    CHECK_EQ_I(v92_cpu_nconst(&cp), 2);
    CHECK_EQ_I(v92_up_kind(b, n), V92_UP_CPU);

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v92_cpu_parse(b, 698, 1, &out), 698);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);
    CHECK_EQ_I(v92_cpu_parse(b, 696, 1, &out), V92_ERR_SHORT);
    CHECK_EQ_I(out.drn, 0);                      /* cleared on error */
    CHECK_FLIPS(v92_cpu_parse(b, 698, 1, &out), b, 0, 696);

    /* fill to 12 symbols: 2 bits per symbol with the 4-point TRN2u (24 bits:
     * 720), 3 with the 8-point one (36 bits: 720 as well) */
    memset(b, 9, sizeof b);
    CHECK_EQ_I(v92_cpu_build(&cp, 24, b), 720);
    CHECK_EQ_I(count_ones(b, 697, 719), 0);
    CHECK_EQ_I(b[720], 9);
    CHECK_EQ_I(v92_cpu_parse(b, 720, 24, &out), 720);
    CHECK_EQ_I(v92_cpu_parse(b, 720, 36, &out), 720);
    /* a short fill is tolerated: the next frame sync starts at 705 */
    for (i = 705; i < 720; i++) b[i] = 1;
    CHECK_EQ_I(v92_cpu_parse(b, 720, 24, &out), 705);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);

    /* the same parameters in V.90's struct */
    v92_cpu_to_v90(&cp, &v90);
    CHECK_EQ_I(v90.is_cp, 1);
    CHECK_EQ_I(v90.drn, 22);
    CHECK_EQ_I(v90.sr, 2);
    CHECK_EQ_I(v90.ack, 1);
    CHECK_EQ_I(v90.alaw, 1);
    CHECK_EQ_I(v90.ld, 3);
    CHECK_EQ_I(v90.rms_ratio, 0x2000);
    CHECK_EQ_I(v90.a1, -32); CHECK_EQ_I(v90.a2, 5);
    CHECK_EQ_I(v90.b1, 100); CHECK_EQ_I(v90.b2, -1);
    CHECK_EQ_I(memcmp(v90.idx, cp.idx, 6), 0);
    CHECK_EQ_I(v90.codec_differs, 1);
    CHECK_EQ_I(memcmp(v90.mask, cp.mask, sizeof cp.mask), 0);
    CHECK_EQ_I(memcmp(v90.codec_mask, cp.codec_mask, sizeof cp.codec_mask), 0);
    CHECK_EQ_I(v90.silence, 0);
    CHECK_EQ_I(v90.rates, 0);

    /* CPt with one constellation: gamma = delta = 0, start bit 272, CRC
     * 273:288, fill bit 289: 290 bits; one bit per symbol, 12 symbols: 300 */
    memset(&cp, 0, sizeof cp);
    cp.type = V92_UP_CPT;
    cp.drn = 13;                 /* (13 + 8) * 8000 / 6 = 28000 */
    setbit(cp.mask[0], 40); setbit(cp.mask[0], 80);
    memset(b, 9, sizeof b);
    n = v92_cpu_build(&cp, 12, b);
    CHECK_EQ_I(n, 300);
    CHECK_EQ_I(frame_ok(b, 272), 0);
    CHECK_EQ_I(fld(b, 18, 20), 0);
    CHECK_EQ_I(fld(b, 21, 25), 13);
    CHECK_EQ_I(b[128], 0);
    CHECK_EQ_I(b[170 + 1 + 8], 1);               /* Uchord3 word at 170: Ucode 40 */
    CHECK_EQ_I(b[221 + 1 + 0], 1);               /* Uchord6 word at 221: Ucode 80 */
    CHECK_EQ_I(count_ones(b, 18, 271), 2 + 3);   /* drn = 13 has three ones */
    CHECK_EQ_I(count_ones(b, 289, 299), 0);
    CHECK_EQ_I(b[300], 9);
    CHECK_EQ_I(v92_up_kind(b, n), V92_UP_CPT);
    CHECK_EQ_I(v92_cpu_parse(b, 300, 12, &out), 300);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);
    v92_cpu_to_v90(&cp, &v90);
    CHECK_EQ_I(v90.is_cp, 0);

    /* six constellations with codec constellations: gamma = 680,
     * delta = 1496; 290 + 1496 = 1786 bits */
    for (i = 0; i < 6; i++) { cp.idx[i] = (uint8_t)i; setbit(cp.mask[i], 20 * i); setbit(cp.codec_mask[i], 127 - i); }
    cp.codec_differs = 1;
    CHECK_EQ_I(v92_cpu_build(&cp, 1, b), 1786);
    CHECK_EQ_I(frame_ok(b, 272 + 1496), 0);
    CHECK_EQ_I(v92_cpu_parse(b, 1786, 1, &out), 1786);
    CHECK_EQ_I(memcmp(&cp, &out, sizeof cp), 0);

    /* not encodable, not a CPt / CPu */
    cp.idx[2] = 6;
    CHECK_EQ_I(v92_cpu_build(&cp, 1, b), V92_ERR_RANGE);
    cp.idx[2] = 2; cp.type = 2;
    CHECK_EQ_I(v92_cpu_build(&cp, 1, b), V92_ERR_RANGE);
    cp.type = 1; cp.ld = 4;
    CHECK_EQ_I(v92_cpu_build(&cp, 1, b), V92_ERR_RANGE);
    cp.ld = 0;
    CHECK_EQ_I(v92_cpu_build(&cp, 0, b), V92_ERR_RANGE);
}

/* ---- CPus, SUVu, SUVd (Tables 24, 27, 31) -------------------------------- */

static void test_short(void)
{
    v92_cpus_t cs, cso;
    v92_suvu_t su, suo;
    v92_suvd_t sd, sdo;
    static v92_cpu_t cpo;
    uint8_t b[96];
    int n;

    /* CPus */
    cs.drn = 9; cs.ack = 1;
    memset(b, 9, sizeof b);
    n = v92_cpus_build(&cs, 1, b);
    CHECK_EQ_I(n, 52);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "0"                      /* 18    CP */
        "01"                     /* 19:20 2 = CPus */
        "10010"                  /* 21:25 drn = 9 */
        "0000000"                /* 26:32 reserved */
        "1"                      /* 33    acknowledge */
        "0"                      /* 34    start bit */
        "cccccccccccccccc"       /* 35:50 CRC */
        "0"), 0);                /* 51    fill bit */
    CHECK_EQ_I(frame_ok(b, 34), 0);
    CHECK_EQ_I(b[52], 9);
    CHECK_EQ_I(v92_up_kind(b, n), V92_UP_CPUS);
    memset(&cso, 0xEE, sizeof cso);
    CHECK_EQ_I(v92_cpus_parse(b, 52, 1, &cso), 52);
    CHECK_EQ_I(cso.drn, 9);
    CHECK_EQ_I(cso.ack, 1);
    CHECK_EQ_I(v92_suvu_parse(b, 52, 1, &suo), V92_ERR_KIND);
    CHECK_EQ_I(v92_cpu_parse(b, 52, 1, &cpo), V92_ERR_KIND);
    CHECK_FLIPS(v92_cpus_parse(b, 52, 1, &cso), b, 0, 50);
    CHECK_EQ_I(v92_cpus_build(&cs, 24, b), 72);
    CHECK_EQ_I(count_ones(b, 51, 71), 0);
    CHECK_EQ_I(v92_cpus_parse(b, 72, 24, &cso), 72);
    cs.drn = 32;
    CHECK_EQ_I(v92_cpus_build(&cs, 1, b), V92_ERR_RANGE);

    /* SUVu: wait for CPu, level -1.25 dB = -5 in Q2.2 = 11011 as 5 bits
     * (the same LSB first), acknowledge */
    memset(&su, 0, sizeof su);
    su.wait_cpu = 1; su.level = -5; su.silence = 0; su.ack = 1;
    memset(b, 9, sizeof b);
    n = v92_suvu_build(&su, 1, b);
    CHECK_EQ_I(n, 52);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "1"                      /* 18    SUVu */
        "0000000"                /* 19:25 reserved */
        "1"                      /* 26    wait for CPu */
        "11011"                  /* 27:31 level */
        "0"                      /* 32    no silent period */
        "1"                      /* 33    acknowledge */
        "0"                      /* 34    start bit */
        "cccccccccccccccc"       /* 35:50 CRC */
        "0"), 0);                /* 51    fill bit */
    CHECK_EQ_I(frame_ok(b, 34), 0);
    CHECK_EQ_I(b[52], 9);
    CHECK_EQ_I(v92_up_kind(b, n), V92_UP_SUVU);
    memset(&suo, 0xEE, sizeof suo);
    CHECK_EQ_I(v92_suvu_parse(b, 52, 1, &suo), 52);
    CHECK_EQ_I(suo.wait_cpu, 1);
    CHECK_EQ_I(suo.level, -5);
    CHECK_EQ_I(suo.silence, 0);
    CHECK_EQ_I(suo.ack, 1);
    CHECK_EQ_I(v92_cpus_parse(b, 52, 1, &cso), V92_ERR_KIND);
    CHECK_FLIPS(v92_suvu_parse(b, 52, 1, &suo), b, 0, 50);
    /* 12 symbols of 3 bits: 72 */
    CHECK_EQ_I(v92_suvu_build(&su, 36, b), 72);
    CHECK_EQ_I(v92_suvu_parse(b, 72, 36, &suo), 72);
    /* no measurement: "the value 16 (-4.00)", 00001 LSB first; +3.75 dB = 15 */
    su.wait_cpu = 0; su.level = V92_SUVU_NO_LEVEL; su.silence = 1; su.ack = 0;
    CHECK_EQ_I(v92_suvu_build(&su, 1, b), 52);
    CHECK_EQ_I(match(b + 18, "1" "0000000" "0" "00001" "1" "0"), 0);
    CHECK_EQ_I(v92_suvu_parse(b, 52, 1, &suo), 52);
    CHECK_EQ_I(suo.level, V92_SUVU_NO_LEVEL);
    CHECK_EQ_I(suo.silence, 1);
    su.level = 15;
    CHECK_EQ_I(v92_suvu_build(&su, 1, b), 52);
    CHECK_EQ_I(fld(b, 27, 31), 15);
    CHECK_EQ_I(v92_suvu_parse(b, 52, 1, &suo), 52);
    CHECK_EQ_I(suo.level, 15);
    su.level = 16;
    CHECK_EQ_I(v92_suvu_build(&su, 1, b), V92_ERR_RANGE);
    su.level = -17;
    CHECK_EQ_I(v92_suvu_build(&su, 1, b), V92_ERR_RANGE);

    /* SUVd */
    sd.silence = 1; sd.ack = 0;
    memset(b, 9, sizeof b);
    n = v92_suvd_build(&sd, 1, b);
    CHECK_EQ_I(n, 52);
    CHECK_EQ_I(match(b,
        "11111111111111111"      /*  0:16 frame sync */
        "0"                      /* 17    start bit */
        "1"                      /* 18    SUVd */
        "0000000000000"          /* 19:31 reserved */
        "1"                      /* 32    silent period */
        "0"                      /* 33    acknowledge */
        "0"                      /* 34    start bit */
        "cccccccccccccccc"       /* 35:50 CRC */
        "0"), 0);                /* 51    fill bit */
    CHECK_EQ_I(frame_ok(b, 34), 0);
    CHECK_EQ_I(b[52], 9);
    CHECK_EQ_I(v92_dn_kind(b, n), V92_DN_SUVD);
    CHECK_EQ_I(v92_dn_kind(b, 18), V92_ERR_SHORT);
    memset(&sdo, 0xEE, sizeof sdo);
    CHECK_EQ_I(v92_suvd_parse(b, 52, 1, &sdo), 52);
    CHECK_EQ_I(sdo.silence, 1);
    CHECK_EQ_I(sdo.ack, 0);
    CHECK_FLIPS(v92_suvd_parse(b, 52, 1, &sdo), b, 0, 50);
    /* 6 symbols of 7 bits: 84 */
    sd.silence = 0; sd.ack = 1;
    CHECK_EQ_I(v92_suvd_build(&sd, 42, b), 84);
    CHECK_EQ_I(b[32], 0);
    CHECK_EQ_I(b[33], 1);
    CHECK_EQ_I(count_ones(b, 51, 83), 0);
    CHECK_EQ_I(v92_suvd_parse(b, 84, 42, &sdo), 84);
    CHECK_EQ_I(sdo.ack, 1);
}

/* ---- CPd (Table 30) ------------------------------------------------------ */

static void cpd_example(v92_cpd_t *c)
{
    static const uint8_t M[12] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 255 };
    memset(c, 0, sizeof *c);
    c->has_mod = 1; c->has_filt = 1; c->has_const = 1;
    c->drn = 19;                 /* 48000 */
    c->e2u_extend = 1;
    c->map.K = 72;
    c->map.trellis = V92_TRELLIS_64;
    memcpy(c->map.M, M, 12);
    c->filt.gain4 = 0x8001;
    c->filt.lz1 = 1; c->filt.lp1 = 2; c->filt.lz2 = 2; c->filt.lp2 = 0;
    c->filt.coef[0] = -8192;     /* z1(1) = -0.25 (Q0.15): 0xE000 */
    c->filt.coef[1] = 16384;     /* p1(1) = 1.0 (Q1.14): 0x4000 */
    c->filt.coef[2] = -1;        /* p1(2): 0xFFFF */
    c->filt.coef[3] = 32767;     /* z2(0): 0x7FFF */
    c->filt.coef[4] = -32768;    /* z2(1): 0x8000 */
    c->map.cidx[1] = 1; c->map.cidx[3] = 1; c->map.cidx[4] = 1;
    c->map.lc[0] = 3; c->map.lc[1] = 2;
    c->map.pt[0][0] = 100; c->map.pt[0][1] = 300; c->map.pt[0][2] = 32767;
    c->map.pt[1][0] = 1; c->map.pt[1][1] = 65;
}

static void test_cpd(void)
{
    static v92_cpd_t c, out;
    static uint8_t b[V92_CPD_MAX_BITS + 16];
    uint32_t seed = 7;
    int n, i, k, len;

    /* All parts present. LZ1 + LP1 + LZ2 + LP2 = 5: alpha = 85.
     * LC1 + LC2 = 5: beta = 85. Table 30:
     *   0:50 always; 51:152 moduli; 153:220 the four lengths;
     *   221:237 z1(1); 238:254 p1(1); 255:271 p1(2); 272:288 z2(0);
     *   289:305 z2(1); 221 + alpha = 306: start bit of the constellation
     *   part; 307:322 and 324:331 the indices; 341:356 LC1, LC2; 358:373
     *   LC3, LC4; 375:390 LC5, LC6; 306 + alpha = 391: start bit, 392:407
     *   first point; ...; 306 + alpha + 17 * LC1 = 442: start bit of the
     *   second set; 306 + alpha + beta = 476: start bit, 477:492 CRC,
     *   493 fill bit: 494 bits. */
    cpd_example(&c);
    memset(b, 9, sizeof b);
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 494);
    CHECK_EQ_I(frame_ok(b, 476), 0);
    CHECK_EQ_I(match(b + 17,
        "0"                      /* 17    start bit */
        "0"                      /* 18    CPd */
        "111"                    /* 19:21 all three parts present */
        "11001"                  /* 22:26 drn = 19 */
        "01"                     /* 27:28 2 = 64 states */
        "1"                      /* 29    extend E2u */
        "000"                    /* 30:32 reserved */
        "0"                      /* 33    acknowledge */
        "0"                      /* 34    start bit */
        "1000000000000001"       /* 35:50 4G = 0x8001 */
        "0"), 0);                /* 51    start bit */
    CHECK_EQ_I(fld(b, 52, 59), 10);    CHECK_EQ_I(fld(b, 60, 67), 20);
    CHECK_EQ_I(fld(b, 69, 76), 30);    CHECK_EQ_I(fld(b, 77, 84), 40);
    CHECK_EQ_I(fld(b, 86, 93), 50);    CHECK_EQ_I(fld(b, 94, 101), 60);
    CHECK_EQ_I(fld(b, 103, 110), 70);  CHECK_EQ_I(fld(b, 111, 118), 80);
    CHECK_EQ_I(fld(b, 120, 127), 90);  CHECK_EQ_I(fld(b, 128, 135), 100);
    CHECK_EQ_I(fld(b, 137, 144), 110); CHECK_EQ_I(fld(b, 145, 152), 255);
    CHECK_EQ_I(fld(b, 154, 162), 1);   CHECK_EQ_I(fld(b, 163, 169), 0);   /* LZ1 */
    CHECK_EQ_I(fld(b, 171, 179), 2);   CHECK_EQ_I(fld(b, 180, 186), 0);   /* LP1 */
    CHECK_EQ_I(fld(b, 188, 196), 2);   CHECK_EQ_I(fld(b, 197, 203), 0);   /* LZ2 */
    CHECK_EQ_I(fld(b, 205, 213), 0);   CHECK_EQ_I(fld(b, 214, 220), 0);   /* LP2 */
    CHECK_EQ_I(fld(b, 222, 237), 0xE000);
    CHECK_EQ_I(fld(b, 239, 254), 0x4000);
    CHECK_EQ_I(fld(b, 256, 271), 0xFFFF);
    CHECK_EQ_I(fld(b, 273, 288), 0x7FFF);
    CHECK_EQ_I(fld(b, 290, 305), 0x8000);
    CHECK_EQ_I(match(b + 306,
        "0"                      /* 306      start bit */
        "0000" "1000" "0000" "1000"   /* 307:322  intervals 0&6 .. 3&9 */
        "0"                      /* 323      start bit */
        "1000" "0000"            /* 324:331  intervals 4&10, 5&11 */
        "00000000"               /* 332:339  reserved */
        "0"                      /* 340      start bit */
        "11000000" "01000000"    /* 341:356  LC1 = 3, LC2 = 2 */
        "0"                      /* 357      start bit */
        "00000000" "00000000"    /* 358:373  LC3, LC4 */
        "0"                      /* 374      start bit */
        "00000000" "00000000"    /* 375:390  LC5, LC6 */
        "0"), 0);                /* 391      start bit */
    CHECK_EQ_I(fld(b, 392, 407), 100);
    CHECK_EQ_I(fld(b, 409, 424), 300);
    CHECK_EQ_I(fld(b, 426, 441), 32767);
    CHECK_EQ_I(fld(b, 443, 458), 1);
    CHECK_EQ_I(fld(b, 460, 475), 65);
    CHECK_EQ_I(b[493], 0);
    CHECK_EQ_I(b[494], 9);
    CHECK_EQ_I(v92_dn_kind(b, n), V92_DN_CPD);

    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v92_cpd_parse(b, 494, 1, &out), 494);
    CHECK_EQ_I(memcmp(&c, &out, sizeof c), 0);   /* K = 2 * (19 + 17) = 72 */
    CHECK_EQ_I(v92_cpd_parse(b, 492, 1, &out), V92_ERR_SHORT);
    CHECK_EQ_I(out.drn, 0);                      /* cleared on error */
    CHECK_FLIPS(v92_cpd_parse(b, 494, 1, &out), b, 0, 492);

    /* fill to 6 symbols of, say, 5 bits: 510 */
    memset(b, 9, sizeof b);
    CHECK_EQ_I(v92_cpd_build(&c, 30, b), 510);
    CHECK_EQ_I(count_ones(b, 493, 509), 0);
    CHECK_EQ_I(b[510], 9);
    CHECK_EQ_I(v92_cpd_parse(b, 510, 30, &out), 510);
    CHECK_EQ_I(memcmp(&c, &out, sizeof c), 0);
    for (i = 500; i < 510; i++) b[i] = 1;        /* short fill */
    CHECK_EQ_I(v92_cpd_parse(b, 510, 30, &out), 500);

    /* "All the bits contained in a part are removed from the CPd sequence
     * when it is indicated that the part is not present" (8.8.3).
     * Nothing optional: 0:50, start bit 51, CRC 52:67, fill bit 68. */
    c.has_mod = c.has_filt = c.has_const = 0;
    c.ack = 1; c.e2u_extend = 0; c.drn = 1; c.map.trellis = V92_TRELLIS_16;
    memset(b, 9, sizeof b);
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 69);
    CHECK_EQ_I(frame_ok(b, 51), 0);
    CHECK_EQ_I(match(b + 17, "0" "0" "000" "10000" "00" "0" "000" "1" "0"), 0);
    CHECK_EQ_I(fld(b, 35, 50), 0x8001);
    CHECK_EQ_I(b[69], 9);
    CHECK_EQ_I(v92_cpd_parse(b, 69, 1, &out), 69);
    CHECK_EQ_I(out.has_mod | out.has_filt | out.has_const, 0);
    CHECK_EQ_I(out.drn, 1);
    CHECK_EQ_I(out.ack, 1);
    CHECK_EQ_I(out.map.K, 36);
    CHECK_EQ_I(out.filt.gain4, 0x8001);
    CHECK_EQ_I(out.filt.lz2, 0);
    CHECK_EQ_I(out.map.M[0], 0);
    CHECK_EQ_I(out.map.lc[0], 0);
    /* moduli only: 51:152, start bit 153, CRC 154:169, fill bit 170 */
    c.has_mod = 1;
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 171);
    CHECK_EQ_I(frame_ok(b, 153), 0);
    CHECK_EQ_I(match(b + 19, "100"), 0);
    CHECK_EQ_I(fld(b, 145, 152), 255);
    CHECK_EQ_I(v92_cpd_parse(b, 171, 1, &out), 171);
    CHECK_EQ_I(memcmp(out.map.M, c.map.M, 12), 0);
    CHECK_EQ_I(out.map.lc[0], 0);
    /* constellation sets only: the part starts at 51 instead of 306:
     * indices 52:67 and 69:76, LC 86:101 .., points from 137; 5 points:
     * start bit 51 + 5 * 17 + 5 * 17 = 221, CRC 222:237, fill bit 238 */
    c.has_mod = 0; c.has_const = 1;
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 239);
    CHECK_EQ_I(frame_ok(b, 221), 0);
    CHECK_EQ_I(match(b + 19, "001"), 0);
    CHECK_EQ_I(fld(b, 56, 59), 1);               /* intervals 1 and 7 */
    CHECK_EQ_I(fld(b, 86, 93), 3);               /* LC1 */
    CHECK_EQ_I(fld(b, 94, 101), 2);              /* LC2 */
    CHECK_EQ_I(fld(b, 137, 152), 100);
    CHECK_EQ_I(fld(b, 205, 220), 65);
    CHECK_EQ_I(v92_cpd_parse(b, 239, 1, &out), 239);
    CHECK_EQ_I(memcmp(&out.map.pt, &c.map.pt, sizeof c.map.pt), 0);
    CHECK_EQ_I(memcmp(out.map.cidx, c.map.cidx, 6), 0);
    CHECK_EQ_I(out.map.M[0], 0);
    /* filter only: lengths 51:118, 5 coefficients 119:203, start bit 204 */
    c.has_const = 0; c.has_filt = 1;
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 222);
    CHECK_EQ_I(frame_ok(b, 204), 0);
    CHECK_EQ_I(match(b + 19, "010"), 0);
    CHECK_EQ_I(fld(b, 52, 60), 1);
    CHECK_EQ_I(fld(b, 120, 135), 0xE000);
    CHECK_EQ_I(fld(b, 188, 203), 0x8000);
    CHECK_EQ_I(v92_cpd_parse(b, 222, 1, &out), 222);
    CHECK_EQ_I(memcmp(&out.filt, &c.filt, sizeof c.filt), 0);

    /* cleardown: drn = 0 */
    cpd_example(&c);
    c.has_mod = c.has_filt = c.has_const = 0; c.drn = 0;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), 69);
    CHECK_EQ_I(v92_cpd_parse(b, 69, 1, &out), 69);
    CHECK_EQ_I(out.drn, 0);
    CHECK_EQ_I(out.map.K, 0);

    /* the largest: 384 coefficients, six sets of 128 points:
     * 324 + 17 * (384 + 768) = 19908 bits */
    cpd_example(&c);
    c.filt.lz1 = 32; c.filt.lp1 = 320; c.filt.lz2 = 16; c.filt.lp2 = 16;
    for (i = 0; i < 384; i++) c.filt.coef[i] = (int16_t)(tu_lcg(&seed) & 0xFFFF);
    for (k = 0; k < 6; k++) {
        c.map.cidx[k] = (uint8_t)(5 - k);
        c.map.lc[k] = 128;
        for (i = 0; i < 128; i++) c.map.pt[k][i] = (uint16_t)(1 + (tu_lcg(&seed) & 0x7FFF));
    }
    n = v92_cpd_build(&c, 1, b);
    CHECK_EQ_I(n, 19908);
    CHECK_EQ_I(frame_ok(b, 19908 - 18), 0);
    memset(&out, 0xEE, sizeof out);
    CHECK_EQ_I(v92_cpd_parse(b, n, 1, &out), 19908);
    CHECK_EQ_I(memcmp(&c, &out, sizeof c), 0);

    /* random contents, every combination of parts */
    for (k = 0; k < 64; k++) {
        int s;
        cpd_example(&c);
        c.has_mod = (uint8_t)(k & 1); c.has_filt = (uint8_t)((k >> 1) & 1);
        c.has_const = (uint8_t)((k >> 2) & 1);
        c.drn = (uint8_t)(1 + tu_lcg(&seed) % 19);
        c.map.K = (uint8_t)V92_UP_K(c.drn);
        c.map.trellis = (uint8_t)(tu_lcg(&seed) % 3);
        c.ack = (uint8_t)(tu_lcg(&seed) & 1);
        c.filt.gain4 = (uint16_t)(1 + tu_lcg(&seed) % 65535);
        memset(c.map.M, 0, 12);
        memset(c.map.cidx, 0, 6); memset(c.map.lc, 0, 6); memset(c.map.pt, 0, sizeof c.map.pt);
        c.filt.lz1 = c.filt.lp1 = c.filt.lz2 = c.filt.lp2 = 0;
        memset(c.filt.coef, 0, sizeof c.filt.coef);
        if (c.has_mod) for (i = 0; i < 12; i++) c.map.M[i] = (uint8_t)(tu_lcg(&seed) & 0xFF);
        if (c.has_filt) {
            c.filt.lz1 = (uint16_t)(tu_lcg(&seed) % 40); c.filt.lp1 = (uint16_t)(tu_lcg(&seed) % 40);
            c.filt.lz2 = (uint16_t)(1 + tu_lcg(&seed) % 40); c.filt.lp2 = (uint16_t)(tu_lcg(&seed) % 40);
            len = c.filt.lz1 + c.filt.lp1 + c.filt.lz2 + c.filt.lp2;
            for (i = 0; i < len; i++) c.filt.coef[i] = (int16_t)(tu_lcg(&seed) & 0xFFFF);
        }
        if (c.has_const) {
            int nset = 1 + (int)(tu_lcg(&seed) % 6);
            for (s = 0; s < nset; s++) {
                c.map.lc[s] = (uint8_t)(1 + tu_lcg(&seed) % 128);
                for (i = 0; i < c.map.lc[s]; i++) c.map.pt[s][i] = (uint16_t)(1 + (tu_lcg(&seed) & 0x7FFF));
            }
            for (i = 0; i < 6; i++) c.map.cidx[i] = (uint8_t)(tu_lcg(&seed) % (unsigned)nset);
        }
        n = v92_cpd_build(&c, 6, b);
        CHECK(n > 0 && n % 6 == 0);
        memset(&out, 0xEE, sizeof out);
        CHECK_EQ_I(v92_cpd_parse(b, n, 6, &out), n);
        CHECK_EQ_I(memcmp(&c, &out, sizeof c), 0);
    }

    /* not encodable */
    cpd_example(&c); c.drn = 20;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.trellis = 3;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.filt.gain4 = 0;                   /* 4G > 0 */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.filt.lp1 = 321;                   /* above any Lmax */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.filt.lp1 = 200; c.filt.lz2 = 185; /* 386 > any Ltot */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.lc[0] = 129;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.lc[0] = 0;                    /* LC1 has no "possibly zero" */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.lc[3] = 1; c.map.pt[3][0] = 5;   /* non-empty after empty */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.pt[1][0] = 0;                 /* no zero point */
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c); c.map.cidx[2] = 6;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), V92_ERR_RANGE);
    cpd_example(&c);
    CHECK_EQ_I(v92_cpd_build(&c, 0, b), V92_ERR_RANGE);
    /* the same faults in parts that are not sent do not matter */
    cpd_example(&c); c.map.pt[1][0] = 0; c.filt.lp1 = 400; c.has_const = 0; c.has_filt = 0;
    CHECK_EQ_I(v92_cpd_build(&c, 1, b), 171);

    /* a received LP1 of 400 (bit 171 + 4 and + 7 and + 8 = 16 + 128 + 256)
     * cannot be laid out */
    cpd_example(&c);
    n = v92_cpd_build(&c, 1, b);
    b[171 + 1] ^= 1; b[171 + 4] = 1; b[171 + 7] = 1; b[171 + 8] = 1;
    CHECK_EQ_I(fld(b, 171, 179), 400);
    CHECK_EQ_I(v92_cpd_parse(b, V92_CPD_MAX_BITS, 1, &out), V92_ERR_RANGE);
    /* an SUVd is not a CPd */
    {
        v92_suvd_t sd = { 0, 0 };
        CHECK_EQ_I(v92_suvd_build(&sd, 1, b), 52);
        CHECK_EQ_I(v92_cpd_parse(b, 52, 1, &out), V92_ERR_KIND);
        cpd_example(&c);
        c.has_mod = c.has_filt = c.has_const = 0;
        CHECK_EQ_I(v92_cpd_build(&c, 1, b), 69);
        CHECK_EQ_I(v92_suvd_parse(b, 69, 1, &sd), V92_ERR_KIND);
    }
}

int main(void)
{
    test_sync();
    test_ja();
    test_jd_jp();
    test_cpu();
    test_short();
    test_cpd();
    T_REPORT("test_seq");
}
