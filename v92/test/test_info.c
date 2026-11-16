/* V.92 INFO sequences, MH sequences and the short Phase 1 QC signals.
 *
 * Expected bit positions are taken by hand from Tables 2, 4, 11, 13, 15 to
 * 19, 32 and 33 of V.92 and Tables 10 and 11 of V.90; the expected CRC comes
 * from a cell-by-cell model of Figure 14/V.34 kept in this file. */
#include <string.h>
#include "test.h"
#include "v92_info.h"

/* ---- independent helpers ------------------------------------------------ */

/* Figure 14/V.34 as drawn (see test_seq.c) */
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

/* the CRC at bits crc..crc+15 covers bits 12..crc-1 */
static int crc_ok(const uint8_t *b, int crc)
{
    fig14_t f;
    int i;
    fig14_init(&f);
    for (i = 12; i < crc; i++) fig14_bit(&f, b[i]);
    return fld(b, crc, crc + 15) == fig14_val(&f);
}

#define CHECK_FLIPS(parse_call, b, from, to) do {                       \
        int fl_, bad_ = 0;                                              \
        for (fl_ = (from); fl_ <= (to); fl_++) {                        \
            (b)[fl_] ^= 1;                                              \
            if ((parse_call) > 0) bad_++;                               \
            (b)[fl_] ^= 1;                                              \
        }                                                               \
        CHECK_EQ_I(bad_, 0);                                            \
    } while (0)

/* ---- INFO0 ---------------------------------------------------------------- */

static void info0_example(v92_info0_t *i)
{
    memset(i, 0, sizeof *i);
    i->sr2743 = 1; i->sr2800 = 0; i->sr3429 = 1;
    i->lo3000 = 1; i->hi3000 = 0; i->lo3200 = 0; i->hi3200 = 1;
    i->allow3429 = 1; i->pwr_reduce = 0;
    i->max_sr_diff = 5;
    i->cme = 0; i->c1664 = 1;
    i->short_p2 = 0; i->v92 = 1; i->ack = 0;
    i->tx_power = 9; i->max_tx_power = 20;
    i->pwr_at_codec = 1; i->alaw = 0; i->up3429 = 1;
}

static void test_info0(void)
{
    v92_info0_t i, o;
    uint8_t b[80];
    int n;

    info0_example(&i);
    memset(b, 9, sizeof b);
    n = v92_info0d_build(&i, b);
    CHECK_EQ_I(n, 62);
    CHECK_EQ_I(match(b,
        "1111"                   /*  0:3  fill */
        "01110010"               /*  4:11 frame sync */
        "101"                    /* 12:14 2743, 2800, 3429 */
        "1001"                   /* 15:18 low/high carrier at 3000 and 3200 */
        "1"                      /* 19    3429 allowed */
        "0"                      /* 20    power reduction */
        "101"                    /* 21:23 5 */
        "0"                      /* 24    CME */
        "1"                      /* 25    1664 points */
        "0"                      /* 26    short Phase 2 not requested */
        "1"                      /* 27    V.92 capability */
        "0"                      /* 28    acknowledge */
        "1001"                   /* 29:32 9 */
        "00101"                  /* 33:37 20 */
        "1"                      /* 38    power at the codec */
        "0"                      /* 39    mu-law */
        "1"                      /* 40    3429 upstream in V.90 */
        "0"                      /* 41    reserved */
        "cccccccccccccccc"       /* 42:57 CRC */
        "1111"), 0);             /* 58:61 fill */
    CHECK(crc_ok(b, 42));
    CHECK_EQ_I(b[62], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info0d_parse(b, 62, &o), 62);
    CHECK_EQ_I(memcmp(&i, &o, sizeof i), 0);
    CHECK_EQ_I(v92_info0d_parse(b, 58, &o), 58);         /* trailing fill not needed */
    CHECK_EQ_I(v92_info0d_parse(b, 57, &o), V92_ERR_SHORT);
    CHECK_FLIPS(v92_info0d_parse(b, 62, &o), b, 4, 57);
    CHECK_EQ_I(v92_info_find(b, 62, 0), 0);
    CHECK_EQ_I(v92_info_find(b, 11, 0), -1);
    CHECK_EQ_I(v92_info_find(b, 12, 0), 0);
    CHECK_EQ_I(v92_info_find(b, 62, 1), -1);

    /* INFO0a: the same first 14 bits; V.92 capability and the short Phase 2
     * request swap places (Table 16: 26 = V.92, 27 = short Phase 2) */
    memset(b, 9, sizeof b);
    n = v92_info0a_build(&i, b);
    CHECK_EQ_I(n, 49);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "101" "1001" "1" "0" "101" "0" "1"
        "1"                      /* 26    V.92 capability */
        "0"                      /* 27    short Phase 2 not requested */
        "0"                      /* 28    acknowledge */
        "cccccccccccccccc"       /* 29:44 CRC */
        "1111"), 0);             /* 45:48 fill */
    CHECK(crc_ok(b, 29));
    CHECK_EQ_I(b[49], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info0a_parse(b, 49, &o), 49);
    CHECK_EQ_I(o.v92, 1);
    CHECK_EQ_I(o.short_p2, 0);
    CHECK_EQ_I(o.sr3429, 1);
    CHECK_EQ_I(o.max_sr_diff, 5);
    CHECK_EQ_I(o.c1664, 1);
    CHECK_EQ_I(o.tx_power, 0);                   /* INFO0d only: zero */
    CHECK_EQ_I(o.up3429, 0);
    CHECK_FLIPS(v92_info0a_parse(b, 49, &o), b, 4, 44);
    i.v92 = 0; i.short_p2 = 1; i.ack = 1;
    CHECK_EQ_I(v92_info0a_build(&i, b), 49);
    CHECK_EQ_I(match(b + 26, "011"), 0);
    CHECK_EQ_I(v92_info0d_build(&i, b), 62);
    CHECK_EQ_I(match(b + 26, "101"), 0);
    CHECK_EQ_I(v92_info0d_parse(b, 62, &o), 62);
    CHECK_EQ_I(o.v92, 0);
    CHECK_EQ_I(o.short_p2, 1);
    CHECK_EQ_I(o.ack, 1);

    i.max_sr_diff = 8;
    CHECK_EQ_I(v92_info0d_build(&i, b), V92_ERR_RANGE);
    CHECK_EQ_I(v92_info0a_build(&i, b), V92_ERR_RANGE);
    i.max_sr_diff = 0; i.max_tx_power = 32;
    CHECK_EQ_I(v92_info0d_build(&i, b), V92_ERR_RANGE);

    /* the frame is found after garbage and a longer fill */
    info0_example(&i);
    memset(b, 1, sizeof b);
    b[0] = 0; b[1] = 0;
    CHECK_EQ_I(v92_info0d_build(&i, b + 10), 62);
    CHECK_EQ_I(v92_info_find(b, 80, 0), 10);
    CHECK_EQ_I(v92_info0d_parse(b + 10, 70, &o), 62);
}

/* ---- INFO1d --------------------------------------------------------------- */

static void test_info1d(void)
{
    v92_info1d_t i, o;
    uint8_t b[120];
    int n, k;

    memset(&i, 0, sizeof i);
    i.min_pwr_red = 3; i.add_pwr_red = 6; i.md_len = 65;
    for (k = 0; k < 6; k++) {
        i.sr[k].hi_carrier = (uint8_t)(k < 5 ? !(k & 1) : 0);
        i.sr[k].preemph = (uint8_t)(k ? k : 10);
        i.sr[k].max_rate = (uint8_t)(k ? k + 1 : 14);
    }
    i.pcm_up = 1;
    i.freq_off = -512;
    memset(b, 9, sizeof b);
    n = v92_info1d_build(&i, b);
    CHECK_EQ_I(n, 109);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "110"                    /* 12:14 3 */
        "011"                    /* 15:17 6 */
        "1000001"                /* 18:24 65 */
        "1" "0101" "0111"        /* 25:33 2400: high carrier, filter 10, 14 x 2400 */
        "0" "1000" "0100"        /* 34:42 2743: low, 1, 2 */
        "1" "0100" "1100"        /* 43:51 2800: high, 2, 3 */
        "0" "1100" "0010"        /* 52:60 3000: low, 3, 4 */
        "1" "0010" "1010"        /* 61:69 3200: high, 4, 5 */
        "1"                      /* 70    the channel supports PCM upstream */
        "1010" "0110"            /* 71:78 3429: filter 5, 6 */
        "0000000001"             /* 79:88 -512 */
        "cccccccccccccccc"       /* 89:104 CRC */
        "1111"), 0);             /* 105:108 fill */
    CHECK(crc_ok(b, 89));
    CHECK_EQ_I(b[109], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info1d_parse(b, 109, &o), 109);
    CHECK_EQ_I(memcmp(&i, &o, sizeof i), 0);
    CHECK_EQ_I(v92_info1d_parse(b, 104, &o), V92_ERR_SHORT);
    CHECK_FLIPS(v92_info1d_parse(b, 109, &o), b, 4, 104);

    /* bit 70 clear: no PCM upstream; +511 and -1 as frequency offsets */
    i.pcm_up = 0; i.freq_off = 511;
    CHECK_EQ_I(v92_info1d_build(&i, b), 109);
    CHECK_EQ_I(b[70], 0);
    CHECK_EQ_I(match(b + 79, "1111111110"), 0);
    CHECK_EQ_I(v92_info1d_parse(b, 109, &o), 109);
    CHECK_EQ_I(o.pcm_up, 0);
    CHECK_EQ_I(o.freq_off, 511);
    i.freq_off = -1;
    CHECK_EQ_I(v92_info1d_build(&i, b), 109);
    CHECK_EQ_I(match(b + 79, "1111111111"), 0);
    CHECK_EQ_I(v92_info1d_parse(b, 109, &o), 109);
    CHECK_EQ_I(o.freq_off, -1);

    i.freq_off = 512;
    CHECK_EQ_I(v92_info1d_build(&i, b), V92_ERR_RANGE);
    i.freq_off = 0; i.sr[2].preemph = 16;
    CHECK_EQ_I(v92_info1d_build(&i, b), V92_ERR_RANGE);
    i.sr[2].preemph = 0; i.md_len = 128;
    CHECK_EQ_I(v92_info1d_build(&i, b), V92_ERR_RANGE);
}

/* ---- INFO1a --------------------------------------------------------------- */

static void test_info1a(void)
{
    v92_info1a_t i, o;
    v92_filt_t f;
    uint8_t b[80];
    int n;

    /* Table 18: PCM upstream. All four sections, Ltot = 320, Lmax = 192,
     * MD of 5 x 276 symbols, UINFO = 85 */
    memset(&i, 0, sizeof i);
    i.kind = V92_INFO1A_PCM;
    i.sections = 3; i.ltot_code = 2; i.lmax_code = 1;
    i.md_len = 5; i.uinfo = 85;
    memset(b, 9, sizeof b);
    n = v92_info1a_build(&i, b);
    CHECK_EQ_I(n, 70);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "11"                     /* 12:13 z1, p1, p2 and z2 */
        "01"                     /* 14:15 2: 320 */
        "10"                     /* 16:17 1: 192 */
        "1010000"                /* 18:24 5 */
        "1010101"                /* 25:31 85 */
        "00"                     /* 32:33 reserved */
        "011"                    /* 34:36 the integer 6 */
        "011"                    /* 37:39 the integer 6 */
        "1111111111"             /* 40:49 reserved, set to 1 */
        "cccccccccccccccc"       /* 50:65 CRC */
        "1111"), 0);             /* 66:69 fill */
    CHECK(crc_ok(b, 50));
    CHECK_EQ_I(b[70], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info1a_parse(b, 70, &o), 70);
    CHECK_EQ_I(memcmp(&i, &o, sizeof i), 0);
    CHECK_EQ_I(V92_INFO1A_LTOT(o.ltot_code), 320);
    CHECK_EQ_I(V92_INFO1A_LMAX(o.lmax_code), 192);
    CHECK_EQ_I(V92_INFO1A_LTOT(0), 192);
    CHECK_EQ_I(V92_INFO1A_LTOT(3), 384);
    CHECK_EQ_I(V92_INFO1A_LMAX(0), 128);
    CHECK_EQ_I(V92_INFO1A_LMAX(3), 320);
    CHECK_EQ_I(v92_info1a_parse(b, 65, &o), V92_ERR_SHORT);
    CHECK_FLIPS(v92_info1a_parse(b, 70, &o), b, 4, 65);

    /* what the digital modem may put in CPd against this INFO1a (8.8.3) */
    memset(&f, 0, sizeof f);
    f.gain4 = 1; f.lz1 = 10; f.lp1 = 192; f.lz2 = 100; f.lp2 = 18;   /* 320 */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 1);
    f.lp2 = 19;                                  /* 321 > Ltot */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);
    f.lp2 = 0; f.lp1 = 193;                      /* > Lmax */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);
    f.lp1 = 192; f.lz2 = 0;                      /* no prefilter output */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);
    f.lz2 = 1;
    o.sections = 0;                              /* p1 and z2 only */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);     /* LZ1 = 10 */
    f.lz1 = 0;
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 1);
    f.lp2 = 1;
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);
    o.sections = 2;                              /* p1, p2, z2 */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 1);
    f.lz1 = 1;
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);
    o.sections = 1;                              /* z1, p1, z2 */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);     /* LP2 = 1 */
    f.lp2 = 0;
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 1);
    o.kind = V92_INFO1A_V90;                     /* not a PCM upstream INFO1a */
    CHECK_EQ_I(v92_filt_allowed(&f, &o), 0);

    /* Table 19/V.92 (short Phase 2) and Table 10/V.90: V.34 upstream.
     * MD 2 x 35 ms, UINFO 67, high carrier, 3200 (4), offset -3 */
    memset(&i, 0, sizeof i);
    i.kind = V92_INFO1A_V90;
    i.md_len = 2; i.uinfo = 67; i.hi_carrier = 1; i.up_sr = 4; i.freq_off = -3;
    memset(b, 9, sizeof b);
    n = v92_info1a_build(&i, b);
    CHECK_EQ_I(n, 70);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "000000"                 /* 12:17 reserved */
        "0100000"                /* 18:24 2 */
        "1100001"                /* 25:31 67 */
        "0"                      /* 32    reserved */
        "1"                      /* 33    high carrier */
        "001"                    /* 34:36 4 = 3200 */
        "011"                    /* 37:39 the integer 6 */
        "1011111111"             /* 40:49 -3 */
        "cccccccccccccccc"       /* 50:65 CRC */
        "1111"), 0);             /* 66:69 fill */
    CHECK(crc_ok(b, 50));
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info1a_parse(b, 70, &o), 70);
    CHECK_EQ_I(memcmp(&i, &o, sizeof i), 0);

    /* Table 11/V.90: V.34 both ways */
    memset(&i, 0, sizeof i);
    i.kind = V92_INFO1A_V34;
    i.min_pwr_red = 1; i.add_pwr_red = 2; i.md_len = 0; i.hi_carrier = 1;
    i.preemph = 7; i.max_rate = 12; i.up_sr = 5; i.down_sr = 4; i.freq_off = 100;
    memset(b, 9, sizeof b);
    n = v92_info1a_build(&i, b);
    CHECK_EQ_I(n, 70);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "100"                    /* 12:14 1 */
        "010"                    /* 15:17 2 */
        "0000000"                /* 18:24 0 */
        "1"                      /* 25    high carrier */
        "1110"                   /* 26:29 7 */
        "0011"                   /* 30:33 12 */
        "101"                    /* 34:36 5 = 3429 */
        "001"                    /* 37:39 4 = 3200 */
        "0010011000"             /* 40:49 100 */
        "cccccccccccccccc"       /* 50:65 CRC */
        "1111"), 0);             /* 66:69 fill */
    CHECK(crc_ok(b, 50));
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_info1a_parse(b, 70, &o), 70);
    CHECK_EQ_I(memcmp(&i, &o, sizeof i), 0);

    /* not encodable */
    i.down_sr = 6;
    CHECK_EQ_I(v92_info1a_build(&i, b), V92_ERR_RANGE);
    memset(&i, 0, sizeof i);
    i.kind = V92_INFO1A_PCM; i.uinfo = 128;
    CHECK_EQ_I(v92_info1a_build(&i, b), V92_ERR_RANGE);
    i.uinfo = 70; i.sections = 4;
    CHECK_EQ_I(v92_info1a_build(&i, b), V92_ERR_RANGE);
    i.sections = 0; i.kind = 3;
    CHECK_EQ_I(v92_info1a_build(&i, b), V92_ERR_RANGE);
    i.kind = V92_INFO1A_V90; i.up_sr = 6;
    CHECK_EQ_I(v92_info1a_build(&i, b), V92_ERR_RANGE);
}

/* ---- MH -------------------------------------------------------------------- */

static void test_mh(void)
{
    v92_mh_t m, o;
    uint8_t b[48];
    int n;

    m.sig = V92_MH_REQ; m.info = 0;
    memset(b, 9, sizeof b);
    n = v92_mh_build(&m, b);
    CHECK_EQ_I(n, 40);
    CHECK_EQ_I(match(b,
        "1111" "01110010"
        "0011"                   /* 12:15 MHreq */
        "0011"                   /* 16:19 repeated */
        "cccccccccccccccc"       /* 20:35 CRC */
        "1111"), 0);             /* 36:39 fill */
    CHECK(crc_ok(b, 20));
    CHECK_EQ_I(b[40], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_mh_parse(b, 40, &o), 40);
    CHECK_EQ_I(o.sig, V92_MH_REQ);
    CHECK_EQ_I(o.info, V92_MH_REQ);
    CHECK_EQ_I(v92_mh_parse(b, 35, &o), V92_ERR_SHORT);
    CHECK_FLIPS(v92_mh_parse(b, 40, &o), b, 4, 35);

    /* MHack, 4 minutes: 1000 */
    m.sig = V92_MH_ACK; m.info = 0x8;
    CHECK_EQ_I(v92_mh_build(&m, b), 40);
    CHECK_EQ_I(match(b + 12, "0101" "1000"), 0);
    CHECK_EQ_I(v92_mh_parse(b, 40, &o), 40);
    CHECK_EQ_I(o.sig, V92_MH_ACK);
    CHECK_EQ_I(o.info, 0x8);
    CHECK_EQ_I(v92_mh_t1_seconds(o.info), 240);
    /* MHclrd, outgoing call: 0110 */
    m.sig = V92_MH_CLRD; m.info = V92_MH_CLRD_OUTGOING;
    CHECK_EQ_I(v92_mh_build(&m, b), 40);
    CHECK_EQ_I(match(b + 12, "1001" "0110"), 0);
    CHECK_EQ_I(v92_mh_parse(b, 40, &o), 40);
    CHECK_EQ_I(o.sig, V92_MH_CLRD);
    CHECK_EQ_I(o.info, V92_MH_CLRD_OUTGOING);
    /* the other three */
    m.sig = V92_MH_NACK; m.info = 0;
    CHECK_EQ_I(v92_mh_build(&m, b), 40);
    CHECK_EQ_I(match(b + 12, "0111" "0111"), 0);
    m.sig = V92_MH_CDA;
    CHECK_EQ_I(v92_mh_build(&m, b), 40);
    CHECK_EQ_I(match(b + 12, "1011" "1011"), 0);
    m.sig = V92_MH_FRR;
    CHECK_EQ_I(v92_mh_build(&m, b), 40);
    CHECK_EQ_I(match(b + 12, "1101" "1101"), 0);
    CHECK_EQ_I(v92_mh_parse(b, 40, &o), 40);
    CHECK_EQ_I(o.sig, V92_MH_FRR);
    /* undefined signal */
    m.sig = 0x4;
    CHECK_EQ_I(v92_mh_build(&m, b), V92_ERR_RANGE);
    m.sig = V92_MH_ACK; m.info = 16;
    CHECK_EQ_I(v92_mh_build(&m, b), V92_ERR_RANGE);

    /* Table 33 */
    CHECK_EQ_I(v92_mh_t1_seconds(0x0), 0);
    CHECK_EQ_I(v92_mh_t1_seconds(0x1), 10);
    CHECK_EQ_I(v92_mh_t1_seconds(0x2), 20);
    CHECK_EQ_I(v92_mh_t1_seconds(0x3), 30);
    CHECK_EQ_I(v92_mh_t1_seconds(0x4), 40);
    CHECK_EQ_I(v92_mh_t1_seconds(0x5), 60);
    CHECK_EQ_I(v92_mh_t1_seconds(0x6), 120);
    CHECK_EQ_I(v92_mh_t1_seconds(0x7), 180);
    CHECK_EQ_I(v92_mh_t1_seconds(0x9), 360);
    CHECK_EQ_I(v92_mh_t1_seconds(0xA), 480);
    CHECK_EQ_I(v92_mh_t1_seconds(0xB), 720);
    CHECK_EQ_I(v92_mh_t1_seconds(0xC), 960);
    CHECK_EQ_I(v92_mh_t1_seconds(0xD), -1);
    CHECK_EQ_I(v92_mh_t1_seconds(0xE), 0);
    CHECK_EQ_I(v92_mh_t1_seconds(0xF), 0);
}

/* ---- QC -------------------------------------------------------------------- */

static void test_qc(void)
{
    static const int uqts[15] = { 61, 62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 86, 87 };
    v92_qc_t q, o;
    uint8_t b[80];
    int n, k;

    /* QC1a, LAPM, WXYZ = 1011 (UQTS 82): bits 24:29 = W 0 X Y Z 1 = 100111 */
    q.digital = 0; q.qca = 0; q.lapm = 1; q.code = 0xB;
    memset(b, 9, sizeof b);
    n = v92_qc_build(&q, b);
    CHECK_EQ_I(n, 60);
    CHECK_EQ_I(match(b,
        "1111111111"             /*  0:9  ten ones */
        "0101010101"             /* 10:19 synchronization sequence */
        "0"                      /* 20    start bit */
        "0"                      /* 21    analogue modem */
        "0"                      /* 22    QC */
        "1"                      /* 23    P */
        "100111"                 /* 24:29 W0XYZ1 */
        "1111111111"             /* 30:39 ten ones */
        "0101010101"             /* 40:49 */
        "0001100111"), 0);       /* 50:59 000PW0XYZ1 */
    CHECK_EQ_I(b[60], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_qc_parse(b, 60, &o), 60);
    CHECK_EQ_I(o.digital, 0); CHECK_EQ_I(o.qca, 0);
    CHECK_EQ_I(o.lapm, 1);    CHECK_EQ_I(o.code, 0xB);
    CHECK_EQ_I(v92_qc_uqts(o.code), 82);
    CHECK_EQ_I(v92_qc_parse(b, 59, &o), V92_ERR_SHORT);
    CHECK_FLIPS(v92_qc_parse(b, 60, &o), b, 0, 59);

    /* QCA1d, no LAPM, LM = 10 (-15 dBm0): 011P000LM1 = 0110000101, and ten
     * more ones */
    q.digital = 1; q.qca = 1; q.lapm = 0; q.code = 2;
    memset(b, 9, sizeof b);
    n = v92_qc_build(&q, b);
    CHECK_EQ_I(n, 70);
    CHECK_EQ_I(match(b,
        "1111111111" "0101010101" "0110000101"
        "1111111111" "0101010101" "0110000101"
        "1111111111"), 0);       /* 60:69 */
    CHECK_EQ_I(b[70], 9);
    memset(&o, 0xEE, sizeof o);
    CHECK_EQ_I(v92_qc_parse(b, 70, &o), 70);
    CHECK_EQ_I(o.digital, 1); CHECK_EQ_I(o.qca, 1);
    CHECK_EQ_I(o.lapm, 0);    CHECK_EQ_I(o.code, 2);
    CHECK_EQ_I(v92_qc_anspcm_level(o.code), -30);
    CHECK_EQ_I(v92_qc_parse(b, 69, &o), V92_ERR_SHORT);

    /* QCA1a (Table 4): 001PW0XYZ1 with WXYZ = 0100 (UQTS 67), 70 bits */
    q.digital = 0; q.qca = 1; q.lapm = 0; q.code = 0x4;
    CHECK_EQ_I(v92_qc_build(&q, b), 70);
    CHECK_EQ_I(match(b + 20, "0010001001"), 0);
    CHECK_EQ_I(match(b + 50, "0010001001" "1111111111"), 0);
    /* QC1d (Table 11): 010P000LM1 with P = 1, LM = 01 (-12 dBm0), 60 bits */
    q.digital = 1; q.qca = 0; q.lapm = 1; q.code = 1;
    CHECK_EQ_I(v92_qc_build(&q, b), 60);
    CHECK_EQ_I(match(b + 20, "0101000011"), 0);
    CHECK_EQ_I(v92_qc_parse(b, 60, &o), 60);
    CHECK_EQ_I(o.digital, 1); CHECK_EQ_I(o.qca, 0);
    CHECK_EQ_I(o.lapm, 1);    CHECK_EQ_I(o.code, 1);
    /* a digital QC with a set bit where 000 belongs is rejected */
    b[24] = 1; b[54] = 1;
    CHECK_EQ_I(v92_qc_parse(b, 60, &o), V92_ERR_SYNC);

    q.code = 4;
    CHECK_EQ_I(v92_qc_build(&q, b), V92_ERR_RANGE);
    q.digital = 0; q.code = 16;
    CHECK_EQ_I(v92_qc_build(&q, b), V92_ERR_RANGE);

    for (k = 0; k < 15; k++) CHECK_EQ_I(v92_qc_uqts(k), uqts[k]);
    CHECK_EQ_I(v92_qc_uqts(15), 0);
    CHECK_EQ_I(v92_qc_anspcm_level(0), -19);
    CHECK_EQ_I(v92_qc_anspcm_level(1), -24);
    CHECK_EQ_I(v92_qc_anspcm_level(2), -30);
    CHECK_EQ_I(v92_qc_anspcm_level(3), -36);
}

int main(void)
{
    test_info0();
    test_info1d();
    test_info1a();
    test_mh();
    test_qc();
    T_REPORT("test_info");
}
