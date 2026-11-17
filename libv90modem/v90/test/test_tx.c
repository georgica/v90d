/* The sample-driven transmitter: hand-computed octet vectors for every signal
 * (worked in the comments from V.90 8.4, 8.6 and Table 1), and the rules of
 * the API: when a selection takes effect, default successors, counters,
 * frame alignment, independence of instances. The long round-trip run through
 * the reference decoder is in test_e2e.c. */
#include <string.h>
#include "test.h"
#include "v90_tx.h"
#include "v90_pcm.h"
#include "v90_dec.h"

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }

static void points(v90_map_t *m, int i, const int *uc, int n)
{
    uint8_t mask[16];
    int k;
    memset(mask, 0, sizeof mask);
    for (k = 0; k < n; k++) setbit(mask, uc[k]);
    v90_map_set_points(m, i, mask);
}

/* the small training map used in the hand vectors: K = 6,
 * C0..C3 = {80,50,20}, C4..C5 = {66,33} */
static void small_map(v90_map_t *m, int Sr, int ld)
{
    static const int big[] = { 20, 50, 80 }, small[] = { 33, 66 };
    int i;
    memset(m, 0, sizeof *m);
    m->K = 6; m->Sr = (uint8_t)Sr; m->ld = (uint8_t)ld; m->a1 = 64;
    for (i = 0; i < 4; i++) points(m, i, big, 3);
    for (i = 4; i < 6; i++) points(m, i, small, 2);
}

/* a data map: K = 30 (six 32-point constellations), Sr and ld as given */
static void data_map(v90_map_t *m, int Sr, int ld)
{
    uint8_t mask[16];
    int i, u;
    memset(m, 0, sizeof *m);
    m->K = 30; m->Sr = (uint8_t)Sr; m->ld = (uint8_t)ld; m->a1 = 64; m->b1 = -32;
    for (i = 0; i < 6; i++) {
        memset(mask, 0, sizeof mask);
        for (u = 0; u < 32; u++) setbit(mask, 30 + 3 * u + (i & 1));
        v90_map_set_points(m, i, mask);
    }
}

/* tick until the signal being sent is `sig` and it has sent `nsym` symbols;
 * returns the number of ticks, or -1 after `limit` */
static int run_to(v90tx_t *t, v90tx_sig_t sig, uint32_t nsym, int limit)
{
    int n = 0;
    while (!(v90tx_signal(t) == sig && v90tx_symbols(t) == nsym)) {
        if (n++ >= limit) return -1;
        (void)v90tx_tick(t);
    }
    return n;
}

static int prbs_state_bit(void *ctx)
{
    uint32_t *s = (uint32_t *)ctx;
    *s = *s * 1664525u + 1013904223u;
    return (int)((*s >> 20) & 1);
}

/* ---- idle, Sd, Sd-bar, TRN1d ---------------------------------------------- */

static void test_phase3_start(int alaw)
{
    static v90tx_t t;
    /* Table 1, UINFO = 80, W = Ucode 96:
     *   mu-law: +W 9F, -W 1F, +0 FF, -0 7F, +UINFO AF, -UINFO 2F
     *   A-law:  +W B5, -W 35, +0 D5, -0 55, +UINFO 85, -UINFO 05 */
    const uint8_t Wp = alaw ? 0xB5 : 0x9F, Wn = alaw ? 0x35 : 0x1F;
    const uint8_t Zp = alaw ? 0xD5 : 0xFF, Zn = alaw ? 0x55 : 0x7F;
    const uint8_t Up = alaw ? 0x85 : 0xAF, Un = alaw ? 0x05 : 0x2F;
    /* TRN1d signs = scrambled ones from the zero state (see test_scr.c) */
    static const int runs[] = { 18, 5, 13, 10, 8, 5, 5, 5, 3, 10 };
    int i, r, k, bad;

    CHECK_EQ_I(v90tx_init(&t, alaw, 80), 0);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE);
    CHECK_EQ_I(v90tx_symbols(&t), 0);
    CHECK_EQ_I(v90tx_pending(&t), V90TX_NONE);
    for (i = 0; i < 7; i++) CHECK_EQ_I(v90tx_tick(&t), Zp);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE);
    CHECK_EQ_I(v90tx_symbols(&t), 7);

    /* the first selection starts on the next tick and defines interval 0 */
    CHECK_EQ_I(v90tx_select(&t, V90TX_SD), 0);
    CHECK_EQ_I(v90tx_pending(&t), V90TX_SD);
    CHECK_EQ_I(v90tx_tick(&t), Wp);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SD);
    CHECK_EQ_I(v90tx_symbols(&t), 1);
    CHECK_EQ_I(v90tx_units(&t), 0);
    CHECK_EQ_I(v90tx_phase(&t), 1);
    CHECK_EQ_I(v90tx_pending(&t), V90TX_NONE);
    /* Sd: 64 x {+W,+0,+W,-W,-0,-W} */
    {
        const uint8_t sd[6] = { Wp, Zp, Wp, Wn, Zn, Wn };
        bad = 0;
        for (i = 1; i < 384; i++) if (v90tx_tick(&t) != sd[i % 6]) bad++;
        CHECK_EQ_I(bad, 0);
    }
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SD);
    CHECK_EQ_I(v90tx_symbols(&t), 384);
    CHECK_EQ_I(v90tx_units(&t), 64);
    /* Sd-bar: 8 x {-W,-0,-W,+W,+0,+W}, with no selection needed */
    {
        const uint8_t sdb[6] = { Wn, Zn, Wn, Wp, Zp, Wp };
        bad = 0;
        for (i = 0; i < 48; i++) {
            if (v90tx_tick(&t) != sdb[i % 6]) bad++;
            if (v90tx_signal(&t) != V90TX_SD_BAR) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }
    CHECK_EQ_I(v90tx_symbols(&t), 48);
    CHECK_EQ_I(v90tx_units(&t), 8);
    /* TRN1d follows: UINFO with the sign of the scrambled ones */
    bad = 0; i = 0;
    for (r = 0; r < 10; r++)
        for (k = 0; k < runs[r]; k++, i++) {
            uint8_t want = (r & 1) ? Un : Up;
            if (v90tx_tick(&t) != want) bad++;
            if (v90tx_signal(&t) != V90TX_TRN1D) bad++;
        }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(i, 82);
    CHECK_EQ_I(v90tx_symbols(&t), 82);
    CHECK_EQ_I(v90tx_units(&t), 13);                     /* 82 / 6 */
    CHECK_EQ_I(v90tx_phase(&t), (384 + 48 + 82) % 6);

    CHECK_EQ_I(v90tx_init(&t, alaw, 112), -1);           /* Ucode 16 + 112 does not exist */
    CHECK_EQ_I(v90tx_init(&t, alaw, 111), 0);
}

/* ---- TRN1d -> Jd -> Jd' ---------------------------------------------------- */

static void test_jd(void)
{
    static v90tx_t t;
    v90_jd_t jd, got;
    v90dec_sign_t rx;
    uint8_t bits[72 * 3 + 12];
    int i, n, bad;

    CHECK_EQ_I(v90tx_init(&t, 0, 80), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_JD), -1);          /* no capabilities yet */
    memset(&jd, 0, sizeof jd);
    jd.rates = 0x3FFFFF; jd.cp16_train = 0; jd.cp16_reneg = 0; jd.max_ld = 3;
    CHECK_EQ_I(v90tx_set_jd(&t, &jd), 0);
    jd.max_ld = 7; CHECK_EQ_I(v90tx_set_jd(&t, &jd), V90_ERR_RANGE); jd.max_ld = 3;

    /* 24 symbols of TRN1d (shorter than a real one, to keep the arithmetic
     * in reach): scrambler output n = 0..23 is 18 ones, 5 zeros, 1 one.
     * Jd then starts with 17 ones and the start bit 0:
     *   scrambled n = 24..35: 1 (the all-ones pattern continues), 36..40: 0,
     *   n = 41 (input 0): 0 ^ out[23]=1 ^ out[18]=0 = 1
     * differential encoder seeded with the last TRN1d sign (out[23] = 1):
     *   n = 24..35: 0,1,0,1,0,1,0,1,0,1,0,1   (toggles on every 1)
     *   n = 36..40: 1,1,1,1,1                 (holds on 0)
     *   n = 41:     0 */
    CHECK_EQ_I(v90tx_select(&t, V90TX_TRN1D), 0);
    v90dec_sign_init(&rx);
    bad = 0;
    for (i = 0; i < 24; i++) {
        uint8_t o = v90tx_tick(&t);
        if (v90_octet_ucode(0, o) != 80) bad++;
        if (v90dec_trn1d(&rx, v90_octet_sign(o)) != 1) bad++;    /* descrambles to ones */
        if (i == 20) CHECK_EQ_I(v90tx_select(&t, V90TX_JD), 0);
        if (i == 22) CHECK_EQ_I(v90tx_signal(&t), V90TX_TRN1D);  /* not before the boundary */
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90tx_symbols(&t), 24);
    {
        static const char want[] = "010101010101" "11111" "0";
        uint8_t o;
        for (i = 0; i < 18; i++) {
            o = v90tx_tick(&t);
            CHECK_EQ_I(o, want[i] == '1' ? 0xAF : 0x2F);
            bits[i] = (uint8_t)v90dec_jd(&rx, v90_octet_sign(o));
        }
        CHECK_EQ_I(v90tx_signal(&t), V90TX_JD);
        CHECK_EQ_I(v90tx_symbols(&t), 18);
    }
    /* the rest of three Jd, through the receive path: differential decode,
     * GPC descrambler that has been running since TRN1d */
    for (i = 18; i < 72 * 3; i++) {
        uint8_t o = v90tx_tick(&t);
        bits[i] = (uint8_t)v90dec_jd(&rx, v90_octet_sign(o));
        if (i == 72 * 2 + 30) {
            CHECK_EQ_I(v90tx_units(&t), 2);
            CHECK_EQ_I(v90tx_select(&t, V90TX_JD_PRIME), 0);     /* mid-Jd: finish it first */
        }
    }
    CHECK_EQ_I(v90tx_signal(&t), V90TX_JD);
    CHECK_EQ_I(v90tx_symbols(&t), 216);
    CHECK_EQ_I(v90tx_units(&t), 3);
    for (n = 0; n < 3; n++) {
        memset(&got, 0xEE, sizeof got);
        CHECK_EQ_I(v90_jd_parse(bits + 72 * n, 72, &got), 72);
        CHECK_EQ_I(got.rates, 0x3FFFFF); CHECK_EQ_I(got.max_ld, 3);
        CHECK_EQ_I(got.cp16_train, 0); CHECK_EQ_I(got.cp16_reneg, 0);
    }
    /* Jd': 12 zeros through the same scrambler and differential encoder */
    bad = 0;
    for (i = 0; i < 12; i++) {
        uint8_t o = v90tx_tick(&t);
        if (v90tx_signal(&t) != V90TX_JD_PRIME) bad++;
        if (v90_octet_ucode(0, o) != 80) bad++;
        if (v90dec_jd(&rx, v90_octet_sign(o)) != 0) bad++;
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90tx_units(&t), 1);
    /* no DIL loaded (N = 0): Phase 4 starts, Ri = UINFO with +++--- */
    {
        static const uint8_t ri[6] = { 0xAF, 0xAF, 0xAF, 0x2F, 0x2F, 0x2F };
        bad = 0;
        for (i = 0; i < 30; i++) {
            if (v90tx_tick(&t) != ri[i % 6]) bad++;
            if (v90tx_signal(&t) != V90TX_RI) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }
}

/* ---- DIL ------------------------------------------------------------------- */

static void test_dil(void)
{
    static v90tx_t t;
    static v90_dil_t d;
    int i, bad;
    /* N = 2: training Ucodes 5 (Uchord1) and 100 (Uchord7)
     *   H1 = 0 -> 6 symbols, H7 = 1 -> 12 symbols; REF1 = 10, REF7 = 110
     *   SP = 1,0,1 (LSP 3), 1 = positive;  TP = 0,1 (LTP 2), 1 = training symbol
     * segment 1, m = 0..5:   SP 1,0,1,1,0,1   TP 0,1,0,1,0,1
     *   +10, -5, +10, +5, -10, +5          = F5 7A F5 FA 75 FA
     * segment 2, m = 0..11:  SP 1,0,1,1,0,1,1,0,1,1,0,1   TP 0,1,0,1,...
     *   +110,-100,+110,+100,-110,+100, +110,-100,+110,+100,-110,+100
     *                                       = 91 1B 91 9B 11 9B 91 1B 91 9B 11 9B
     * then the whole DIL repeats. (mu-law octet = FF - Ucode, minus 80 if negative) */
    static const uint8_t seg1[6] = { 0xF5, 0x7A, 0xF5, 0xFA, 0x75, 0xFA };
    static const uint8_t seg2[12] = { 0x91, 0x1B, 0x91, 0x9B, 0x11, 0x9B, 0x91, 0x1B, 0x91, 0x9B, 0x11, 0x9B };

    CHECK_EQ_I(v90tx_init(&t, 0, 80), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_DIL), -1);         /* nothing loaded */
    memset(&d, 0, sizeof d);
    d.n = 0; d.lsp = 1; d.ltp = 1;
    CHECK_EQ_I(v90tx_load_dil(&t, &d), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_DIL), -1);         /* N = 0: there is no DIL */
    d.n = 2; d.lsp = 3; d.ltp = 2;
    d.sp[0] = 0x05; d.tp[0] = 0x02;
    d.h[0] = 0; d.h[6] = 1; d.ref[0] = 10; d.ref[6] = 110;
    d.train[0] = 5; d.train[1] = 100;
    d.ltp = 0; CHECK_EQ_I(v90tx_load_dil(&t, &d), V90_ERR_RANGE); d.ltp = 2;
    CHECK_EQ_I(v90tx_load_dil(&t, &d), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_DIL), 0);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), seg1[i]);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_DIL);
    CHECK_EQ_I(v90tx_units(&t), 1);
    for (i = 0; i < 12; i++) CHECK_EQ_I(v90tx_tick(&t), seg2[i]);
    CHECK_EQ_I(v90tx_units(&t), 2);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), seg1[i]);
    CHECK_EQ_I(v90tx_units(&t), 3);
    /* asked to stop three symbols into segment 2: the segment is completed */
    for (i = 0; i < 3; i++) CHECK_EQ_I(v90tx_tick(&t), seg2[i]);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RI), 0);
    bad = 0;
    for (i = 3; i < 12; i++) {
        if (v90tx_tick(&t) != seg2[i]) bad++;
        if (v90tx_signal(&t) != V90TX_DIL) bad++;
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90tx_units(&t), 4);
    CHECK_EQ_I(v90tx_symbols(&t), 36);
    CHECK_EQ_I(v90tx_tick(&t), 0xAF);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RI);
}

/* ---- Ri, Ri-bar, TRN2d ------------------------------------------------------ */

static void test_phase4_start(int alaw)
{
    static v90tx_t t;
    static v90_map_t m;
    /* UINFO = 80: + AF / 85, - 2F / 05 (mu / A) */
    const uint8_t Up = alaw ? 0x85 : 0xAF, Un = alaw ? 0x05 : 0x2F;
    /* TRN2d with the small map, Sr = 0, D = 12, scrambled ones from zero:
     * frame 1: bits n = 0..11 are ones (test_enc.c test_frame):
     *   U = 80,80,50,20,66,66   signs + - + - + -
     *   mu-law AF 2F CD 6B BD 3D        A-law 85 05 E7 41 97 17
     * frame 2: bits n = 12..23 = 1,1,1,1,1,1 (s) 0,0,0,0,0,1 (b; scrambler
     *   output is 0 for n = 18..22 and 1 at n = 23): R0 = 32
     *   32 = 3*10 + 2, 10 = 3*3 + 1, 3 = 3*1 + 0, 1 = 3*0 + 1 -> K = 2,1,0,1,0,0
     *   U = 20,50,80,50,66,66   signs continue the differential: + - + - + -
     *   mu-law EB 4D AF 4D BD 3D        A-law C1 67 85 67 97 17 */
    static const uint8_t mu1[6] = { 0xAF, 0x2F, 0xCD, 0x6B, 0xBD, 0x3D };
    static const uint8_t mu2[6] = { 0xEB, 0x4D, 0xAF, 0x4D, 0xBD, 0x3D };
    static const uint8_t al1[6] = { 0x85, 0x05, 0xE7, 0x41, 0x97, 0x17 };
    static const uint8_t al2[6] = { 0xC1, 0x67, 0x85, 0x67, 0x97, 0x17 };
    int i, bad;

    CHECK_EQ_I(v90tx_init(&t, alaw, 80), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RI), 0);
    bad = 0;
    for (i = 0; i < 192; i++) if (v90tx_tick(&t) != ((i % 6) < 3 ? Up : Un)) bad++;
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90tx_units(&t), 32);
    CHECK_EQ_I(v90tx_select(&t, V90TX_TRN2D), -1);       /* CPt not loaded yet */
    CHECK_EQ_I(v90tx_select(&t, V90TX_RT), -1);
    small_map(&m, 0, 0);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_TRAIN, &m), 0);
    m.K = 9; CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &m), V90_ERR_RANGE); m.K = 6;
    CHECK_EQ_I(v90tx_select(&t, V90TX_RI_BAR), 0);
    /* Ri-bar: 4 x ---+++ = 24T, then TRN2d by default */
    bad = 0;
    for (i = 0; i < 24; i++) {
        if (v90tx_tick(&t) != ((i % 6) < 3 ? Un : Up)) bad++;
        if (v90tx_signal(&t) != V90TX_RI_BAR) bad++;
    }
    CHECK_EQ_I(bad, 0);
    CHECK_EQ_I(v90tx_units(&t), 4);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), alaw ? al1[i] : mu1[i]);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_TRN2D);
    CHECK_EQ_I(v90tx_units(&t), 1);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), alaw ? al2[i] : mu2[i]);

    /* Rt: the largest point of each training constellation: 80 x4, 66 x2 */
    CHECK_EQ_I(v90tx_select(&t, V90TX_RT), 0);
    for (i = 0; i < 6; i++) (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RT);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), v90_pcm_octet(alaw, (unsigned)(i < 4 ? 80 : 66), i < 3));
    /* Rt-bar then MP by default; MP has no contents yet -> silence instead */
    CHECK_EQ_I(v90tx_select(&t, V90TX_RT_BAR), 0);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), v90_pcm_octet(alaw, (unsigned)(i < 4 ? 80 : 66), i >= 3));
    for (i = 0; i < 18; i++) (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RT_BAR);
    CHECK_EQ_I(v90tx_units(&t), 4);
    CHECK_EQ_I(v90tx_tick(&t), v90_pcm_octet(alaw, 0, 1));
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE);

    /* Ri-bar without a training map falls to silence, still on the grid */
    CHECK_EQ_I(v90tx_init(&t, alaw, 80), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RI_BAR), 0);
    for (i = 0; i < 24; i++) (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RI_BAR);
    CHECK_EQ_I(v90tx_tick(&t), v90_pcm_octet(alaw, 0, 1));
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RI), 0);           /* aligned now: waits for the boundary */
    for (i = 0; i < 5; i++) { (void)v90tx_tick(&t); CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE); }
    CHECK_EQ_I(v90tx_tick(&t), Up);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RI);
}

/* ---- MP, Ed, B1d, data: lengths and boundaries ------------------------------ */

static void test_phase4_seq(int Sr, int ld)
{
    static v90tx_t t;
    static v90_map_t tm, dm;
    v90_mp_t mp;
    uint32_t prbs = 1;
    int i, D, mpfr, n;

    small_map(&tm, Sr, ld);
    data_map(&dm, Sr, ld);
    D = V90_MAP_D(&tm);                                  /* 12 - Sr */
    mpfr = (86 + D - 1) / D;                             /* data frames per Type 0 MP */
    CHECK_EQ_I(v90tx_init(&t, 0, 80), 0);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_TRAIN, &tm), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_MP), -1);          /* no MP contents yet */
    memset(&mp, 0, sizeof mp);
    mp.max_rate = 12; mp.rates = 0x07FF;
    CHECK_EQ_I(v90tx_set_mp(&t, &mp), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_B1D), -1);         /* no CP yet */
    CHECK_EQ_I(v90tx_select(&t, V90TX_DATA), -1);

    CHECK_EQ_I(v90tx_select(&t, V90TX_TRN2D), 0);
    CHECK(run_to(&t, V90TX_TRN2D, 61, 1000) >= 0);       /* one symbol into frame 11 */
    CHECK_EQ_I(v90tx_select(&t, V90TX_MP), 0);
    /* takes effect at the next data frame boundary even with lookahead */
    CHECK_EQ_I(run_to(&t, V90TX_MP, 1, 1000), 6);
    /* halfway through the second MP ask for MP': exactly two MP are sent */
    CHECK(run_to(&t, V90TX_MP, (uint32_t)(6 * mpfr + 6 * mpfr / 2), 100000) >= 0);
    CHECK_EQ_I(v90tx_units(&t), 1);
    CHECK_EQ_I(v90tx_select(&t, V90TX_MP_ACK), 0);
    n = run_to(&t, V90TX_MP_ACK, 1, 100000);
    CHECK_EQ_I(n, 6 * mpfr - 6 * mpfr / 2 + 1);
    /* in the last data frame of the first MP' ask for Ed: one MP' is sent */
    CHECK(run_to(&t, V90TX_MP_ACK, (uint32_t)(6 * mpfr - 2), 100000) >= 0);
    CHECK_EQ_I(v90tx_units(&t), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_ED), 0);
    CHECK_EQ_I(run_to(&t, V90TX_ED, 1, 100000), 3);
    /* Ed is 2 data frames; no CP -> silence follows */
    CHECK_EQ_I(run_to(&t, V90TX_SILENCE, 1, 100000), 12);

    /* again with CP loaded while MP' runs: Ed, B1d (48 frames), data */
    CHECK_EQ_I(v90tx_select(&t, V90TX_TRN2D), 0);
    CHECK(run_to(&t, V90TX_TRN2D, 1, 100) >= 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_MP_ACK), 0);
    CHECK(run_to(&t, V90TX_MP_ACK, 2, 100) >= 0);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &dm), 0);
    v90tx_set_getbit(&t, prbs_state_bit, &prbs);
    CHECK_EQ_I(v90tx_select(&t, V90TX_ED), 0);
    CHECK_EQ_I(run_to(&t, V90TX_ED, 1, 100000), 6 * mpfr - 1);
    CHECK_EQ_I(run_to(&t, V90TX_B1D, 1, 100000), 12);
    CHECK_EQ_I(run_to(&t, V90TX_DATA, 1, 100000), 288);
    for (i = 0; i < 6 * 20 - 1; i++) (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_DATA);
    CHECK_EQ_I(v90tx_units(&t), 20);
    /* leaving data: built-ahead frames hold user bits and are sent first */
    (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RD), 0);
    n = run_to(&t, V90TX_RD, 1, 100000);
    CHECK(n >= 6 && n <= 24 && n % 6 == 0);
    if (Sr == 0 || ld == 0) CHECK_EQ_I(n, 6);
    /* Rd: largest point of each data constellation: 30 + 93 + (i & 1) */
    for (i = 1; i < 6; i++) CHECK_EQ_I(v90tx_tick(&t), v90_pcm_octet(0, (unsigned)(123 + (i & 1)), i < 3));
    CHECK_EQ_I(v90tx_select(&t, V90TX_RD_BAR), 0);
    CHECK_EQ_I(run_to(&t, V90TX_TRN2D, 1, 1000), 25);    /* Rd-bar 24T, then TRN2d */
}

/* 8.6.5: the magnitudes do not depend on ld, and nothing is inserted ahead of
 * the first data frame */
static void test_ld_magnitudes(void)
{
    static v90tx_t t[4];
    static v90_map_t m;
    v90_mp_t mp;
    int ld, i, Sr, bad;
    memset(&mp, 0, sizeof mp);
    mp.max_rate = 10; mp.rates = 0x1FF;
    for (Sr = 1; Sr <= 3; Sr++) {
        int signdiff = 0;
        for (ld = 0; ld < 4; ld++) {
            small_map(&m, Sr, ld);
            CHECK_EQ_I(v90tx_init(&t[ld], 0, 80), 0);
            CHECK_EQ_I(v90tx_set_map(&t[ld], V90TX_MAP_TRAIN, &m), 0);
            CHECK_EQ_I(v90tx_set_mp(&t[ld], &mp), 0);
            CHECK_EQ_I(v90tx_select(&t[ld], V90TX_TRN2D), 0);
        }
        bad = 0;
        for (i = 0; i < 3000; i++) {
            uint8_t o[4];
            for (ld = 0; ld < 4; ld++) {
                o[ld] = v90tx_tick(&t[ld]);
                if (i == 1200) (void)v90tx_select(&t[ld], V90TX_MP);
                if (i == 2400) (void)v90tx_select(&t[ld], V90TX_ED);
                if (v90_octet_ucode(0, o[ld]) != v90_octet_ucode(0, o[0])) bad++;
                if (v90tx_signal(&t[ld]) != v90tx_signal(&t[0])) bad++;
                if (o[ld] != o[0]) signdiff++;
            }
        }
        CHECK_EQ_I(bad, 0);
        CHECK(signdiff > 0);                             /* lookahead does change signs */
    }
}

/* The transmitter's frame queue against a flat model: encode the whole bit
 * stream into frames first, then shape each shaping frame with every later
 * frame in view. Checks that the queue hands the shaper the right symbols and
 * the right number of lookahead frames, across data frame boundaries and from
 * B1d into data. */
typedef struct { int ones; uint32_t prbs; } msrc_t;
static int msrc_bit(msrc_t *s)
{
    if (s->ones > 0) { s->ones--; return 1; }
    return prbs_state_bit(&s->prbs);
}

static void flat_model(const v90_map_t *m, int alaw, msrc_t *src, int nframes, uint8_t *out)
{
    enum { MAXF = 400 };
    static uint8_t u[MAXF][6], tt[MAXF][6];
    v90_scr_t scr;
    v90_diff_t df;
    v90_shaper_t sh;
    uint8_t bits[V90_MAX_D];
    int D = V90_MAP_D(m), j, k, f;
    v90_scr_init(&scr);
    v90_diff_init(&df);
    v90_shaper_init(&sh);
    for (j = 0; j < nframes; j++) {
        for (k = 0; k < D; k++) bits[k] = (uint8_t)v90_scramble_gpc(&scr, msrc_bit(src));
        v90_frame_encode(m, &df, bits, u[j], tt[j]);
    }
    for (j = 0; j < nframes; j++) {
        uint8_t sign[6];
        if (m->Sr == 0) memcpy(sign, tt[j], 6);
        else {
            int F = 6 / m->Sr;
            for (f = 0; f < m->Sr; f++) {
                int32_t lin[24];
                uint8_t t2[24];
                int nf = 0, pos = j * 6 + f * F;
                while (nf <= m->ld && pos + (nf + 1) * F <= nframes * 6) {
                    for (k = 0; k < F; k++) {
                        int p = pos + nf * F + k;
                        lin[nf * F + k] = v90_ucode_linear(alaw, u[p / 6][p % 6]);
                        t2[nf * F + k] = tt[p / 6][p % 6];
                    }
                    nf++;
                }
                (void)v90_shape(&sh, m, lin, t2, nf, sign + f * F);
            }
        }
        for (k = 0; k < 6; k++) out[j * 6 + k] = v90_pcm_octet(alaw, u[j][k], sign[k]);
    }
}

static void test_lookahead_model(void)
{
    static v90tx_t t;
    static v90_map_t m;
    static uint8_t ref[400 * 6];
    int Sr, ld, alaw, i, bad;
    for (alaw = 0; alaw < 2; alaw++)
    for (Sr = 0; Sr <= 3; Sr++)
    for (ld = 0; ld <= 3; ld++) {
        msrc_t src;
        uint32_t prbs = 77;
        /* TRN2d: scrambled ones for ever */
        small_map(&m, Sr, ld);
        m.b1 = 20; m.a2 = -30;
        src.ones = 1 << 30; src.prbs = 0;
        flat_model(&m, alaw, &src, 400, ref);
        (void)v90tx_init(&t, alaw, 80);
        CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_TRAIN, &m), 0);
        CHECK_EQ_I(v90tx_select(&t, V90TX_TRN2D), 0);
        bad = 0;
        for (i = 0; i < 390 * 6; i++) if (v90tx_tick(&t) != ref[i]) bad++;
        CHECK_EQ_I(bad, 0);
        /* B1d (48 data frames of ones) running into data, one stream */
        data_map(&m, Sr, ld);
        src.ones = 48 * V90_MAP_D(&m); src.prbs = 77;
        flat_model(&m, alaw, &src, 400, ref);
        (void)v90tx_init(&t, alaw, 80);
        CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &m), 0);
        v90tx_set_getbit(&t, prbs_state_bit, &prbs);
        CHECK_EQ_I(v90tx_select(&t, V90TX_B1D), 0);
        bad = 0;
        for (i = 0; i < 390 * 6; i++) {
            if (v90tx_tick(&t) != ref[i]) bad++;
            if (v90tx_signal(&t) != (i < 288 ? V90TX_B1D : V90TX_DATA)) bad++;
        }
        CHECK_EQ_I(bad, 0);
    }
}

/* 8.6.1, 8.6.5: scrambler, differential encoder and shape filter memory are
 * zeroed before TRN2d and before B1d, so what follows cannot depend on what
 * was sent before. */
static void test_reset_history(void)
{
    static v90tx_t a, b;
    static v90_map_t tm, dm;
    v90_mp_t mp;
    uint32_t pa = 5, pb = 5;
    int Sr, ld, i, bad;
    memset(&mp, 0, sizeof mp);
    mp.max_rate = 12; mp.rates = 0x7FF;
    for (Sr = 0; Sr <= 3; Sr++)
    for (ld = 0; ld <= 3; ld += 3) {
        small_map(&tm, (Sr + 1) % 4, ld);
        data_map(&dm, Sr, ld);
        pa = pb = 5;
        /* a: a long history; b: straight in */
        (void)v90tx_init(&a, 0, 80);
        (void)v90tx_set_map(&a, V90TX_MAP_TRAIN, &tm);
        (void)v90tx_set_map(&a, V90TX_MAP_DATA, &dm);
        (void)v90tx_set_mp(&a, &mp);
        v90tx_set_getbit(&a, prbs_state_bit, &pa);
        CHECK_EQ_I(v90tx_select(&a, V90TX_TRN2D), 0);
        CHECK(run_to(&a, V90TX_TRN2D, 601, 5000) >= 0);
        CHECK_EQ_I(v90tx_select(&a, V90TX_MP_ACK), 0);
        CHECK(run_to(&a, V90TX_MP_ACK, 100, 5000) >= 0);
        CHECK_EQ_I(v90tx_select(&a, V90TX_ED), 0);
        CHECK(run_to(&a, V90TX_B1D, 1, 5000) >= 0);
        (void)v90tx_init(&b, 0, 80);
        (void)v90tx_set_map(&b, V90TX_MAP_DATA, &dm);
        v90tx_set_getbit(&b, prbs_state_bit, &pb);
        CHECK_EQ_I(v90tx_select(&b, V90TX_B1D), 0);
        CHECK(run_to(&b, V90TX_B1D, 1, 10) >= 0);
        CHECK_EQ_I(a.out[0], b.out[0]);                  /* the first symbol, already sent */
        bad = 0;
        for (i = 0; i < 288 + 600; i++) if (v90tx_tick(&a) != v90tx_tick(&b)) bad++;
        CHECK_EQ_I(bad, 0);
        /* and TRN2d after data equals TRN2d from cold */
        CHECK_EQ_I(v90tx_select(&a, V90TX_RD_BAR), 0);
        CHECK(run_to(&a, V90TX_TRN2D, 1, 5000) >= 0);
        (void)v90tx_init(&b, 0, 80);
        (void)v90tx_set_map(&b, V90TX_MAP_TRAIN, &tm);
        CHECK_EQ_I(v90tx_select(&b, V90TX_TRN2D), 0);
        CHECK(run_to(&b, V90TX_TRN2D, 1, 10) >= 0);
        CHECK_EQ_I(a.out[0], b.out[0]);
        bad = 0;
        for (i = 0; i < 600; i++) if (v90tx_tick(&a) != v90tx_tick(&b)) bad++;
        CHECK_EQ_I(bad, 0);
        /* 8.4.5: likewise the scrambler is zeroed before TRN1d (a retrain) */
        CHECK_EQ_I(v90tx_select(&a, V90TX_TRN1D), 0);
        CHECK(run_to(&a, V90TX_TRN1D, 1, 5000) >= 0);
        (void)v90tx_init(&b, 0, 80);
        CHECK_EQ_I(v90tx_select(&b, V90TX_TRN1D), 0);
        CHECK(run_to(&b, V90TX_TRN1D, 1, 10) >= 0);
        CHECK_EQ_I(a.out[0], b.out[0]);
        bad = 0;
        for (i = 0; i < 300; i++) if (v90tx_tick(&a) != v90tx_tick(&b)) bad++;
        CHECK_EQ_I(bad, 0);
    }
}

/* Leaving data mode: every bit taken from the caller is sent, also the ones
 * in frames built ahead for the lookahead, and they decode. */
typedef struct { uint32_t s; long n; } csrc_t;
static int csrc_bit(void *ctx)
{
    csrc_t *c = (csrc_t *)ctx;
    c->n++;
    return prbs_state_bit(&c->s);
}

static void test_data_exit(int Sr, int ld)
{
    static v90tx_t t;
    static v90_map_t dm;
    static uint8_t log[6 * 80];
    csrc_t src, ref;
    v90dec_t dec;
    v90_scr_t ds;
    uint8_t fb[V90_MAX_D];
    int D, i, k, n = 0, nb1 = 0, bad = 0, asked = 0;
    data_map(&dm, Sr, ld);
    D = V90_MAP_D(&dm);
    src.s = 9; src.n = 0;
    (void)v90tx_init(&t, 1, 70);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &dm), 0);
    v90tx_set_getbit(&t, csrc_bit, &src);
    CHECK_EQ_I(v90tx_select(&t, V90TX_B1D), 0);
    v90dec_init(&dec, &dm, 1);
    v90_scr_init(&ds);
    for (i = 0; i < 6 * 200 && n < (int)sizeof log; i++) {
        uint8_t o = v90tx_tick(&t);
        if (v90tx_signal(&t) == V90TX_B1D) {
            log[nb1 % 6] = o;
            if (++nb1 % 6 == 0) {                        /* keep the decoder in step through B1d */
                if (v90dec_frame(&dec, log, fb) != D) bad++;
                for (k = 0; k < D; k++) if (v90_descramble_gpc(&ds, fb[k]) != 1) bad++;
            }
        } else if (v90tx_signal(&t) == V90TX_DATA) {
            log[n++] = o;
            if (!asked && v90tx_symbols(&t) == 6 * 30 + 4) { CHECK_EQ_I(v90tx_select(&t, V90TX_RD), 0); asked = 1; }
        } else break;
    }
    CHECK_EQ_I(v90tx_signal(&t), V90TX_RD);
    CHECK_EQ_I(nb1, 288);
    CHECK_EQ_I(n % 6, 0);
    CHECK(n >= 6 * 31 && n <= 6 * 34);
    CHECK_EQ_I(src.n, (long)(n / 6) * D);                /* nothing taken and dropped */
    ref.s = 9; ref.n = 0;
    for (i = 0; i < n; i += 6) {
        if (v90dec_frame(&dec, log + i, fb) != D) { bad++; continue; }
        for (k = 0; k < D; k++) if (v90_descramble_gpc(&ds, fb[k]) != csrc_bit(&ref)) bad++;
    }
    CHECK_EQ_I(bad, 0);
}

/* instances do not share state */
static void test_instances(void)
{
    static v90tx_t a, b, solo;
    static v90_map_t m1, m2;
    static uint8_t ref[4000];
    int i, bad = 0;
    small_map(&m1, 2, 2);
    small_map(&m2, 1, 3);
    m2.a1 = -64;
    (void)v90tx_init(&solo, 0, 80);
    (void)v90tx_set_map(&solo, V90TX_MAP_TRAIN, &m1);
    (void)v90tx_select(&solo, V90TX_TRN2D);
    for (i = 0; i < 4000; i++) ref[i] = v90tx_tick(&solo);
    (void)v90tx_init(&a, 0, 80);
    (void)v90tx_set_map(&a, V90TX_MAP_TRAIN, &m1);
    (void)v90tx_select(&a, V90TX_TRN2D);
    (void)v90tx_init(&b, 1, 100);
    (void)v90tx_set_map(&b, V90TX_MAP_TRAIN, &m2);
    (void)v90tx_select(&b, V90TX_TRN2D);
    for (i = 0; i < 4000; i++) {
        (void)v90tx_tick(&b);
        if (v90tx_tick(&a) != ref[i]) bad++;
        if (i % 7 == 0) (void)v90tx_tick(&b);
    }
    CHECK_EQ_I(bad, 0);
}

int main(void)
{
    static v90tx_t t;
    int Sr, ld;
    test_phase3_start(0);
    test_phase3_start(1);
    test_jd();
    test_dil();
    test_phase4_start(0);
    test_phase4_start(1);
    for (Sr = 0; Sr <= 3; Sr++)
        for (ld = 0; ld <= 3; ld++) test_phase4_seq(Sr, ld);
    for (Sr = 0; Sr <= 3; Sr++)
        for (ld = 0; ld <= 3; ld++) test_data_exit(Sr, ld);
    test_ld_magnitudes();
    test_lookahead_model();
    test_reset_history();
    test_instances();
    (void)v90tx_init(&t, 0, 80);
    CHECK_EQ_I(v90tx_select(&t, V90TX_NSIG), -1);
    CHECK_EQ_I(v90tx_select(&t, V90TX_NONE), -1);
    printf("  sizeof(v90tx_t) = %u bytes\n", (unsigned)sizeof(v90tx_t));
    T_REPORT("test_tx");
}
