/* The transmitter's V.92 signals (V.92 8.6, 8.8): Jp and Jp' after Jd, SCR,
 * the Phase 4 sequences SUVd and CPd that travel like MP, and Rf. The
 * sequences are handed over as packed bits (v90tx_set_xseq), so this
 * library does not need to know their contents. */
#include <string.h>
#include "test.h"
#include "v90_tx.h"
#include "v90_pcm.h"
#include "v90_dec.h"

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }

static void map_k(v90_map_t *m, int K, int npts, int top)
{
    uint8_t mask[16];
    int i, u;
    memset(m, 0, sizeof *m);
    m->K = (uint8_t)K;
    for (i = 0; i < 6; i++) {
        memset(mask, 0, sizeof mask);
        for (u = 0; u < npts; u++) setbit(mask, top - 3 * u - (i & 1));
        v90_map_set_points(m, i, mask);
    }
}

static int run_to(v90tx_t *t, v90tx_sig_t sig, uint32_t nsym, int limit)
{
    int n = 0;
    while (!(v90tx_signal(t) == sig && v90tx_symbols(t) == nsym)) {
        if (n++ >= limit) return -1;
        (void)v90tx_tick(t);
    }
    return n;
}

/* a 72-bit sequence with the frame sync of Table 22: 17 ones, a zero, then a pattern */
static void jp_bits(uint8_t *b)
{
    int i;
    for (i = 0; i < 72; i++) b[i] = (uint8_t)(i < 17 ? 1 : i == 17 ? 0 : ((i * 7 + 3) % 5 < 2));
    b[47] = 1;
}

/* ---- Jd, Jp, Jp', then SCR or the DIL (8.6.2 to 8.6.6) -------------------- */
static void test_jp(int alaw, int dil_n)
{
    static v90tx_t t;
    v90_jd_t jd; v90_dil_t dil;
    v90dec_sign_t dj, dt;
    uint8_t jp[72], pk[9], got[72 * 6];
    int i, n, bad_u = 0, ngot = 0, jp_at = -1;
    const unsigned U = 77;

    CHECK_EQ_I(v90tx_init(&t, alaw, (int)U), 0);
    memset(&jd, 0, sizeof jd); jd.rates = 0x3FFFFF; jd.max_ld = 2;
    CHECK_EQ_I(v90tx_set_jd(&t, &jd), 0);
    memset(&dil, 0, sizeof dil);
    dil.n = (uint8_t)dil_n; dil.lsp = 1; dil.ltp = 1; dil.sp[0] = 1; dil.tp[0] = 1;
    for (i = 0; i < dil_n; i++) dil.train[i] = (uint8_t)(40 + i);
    CHECK_EQ_I(v90tx_load_dil(&t, &dil), 0);

    CHECK_EQ_I(v90tx_select(&t, V90TX_JP), -1);              /* no sequence yet */
    jp_bits(jp);
    v90_pack_bits(jp, 72, pk);
    CHECK_EQ_I(v90tx_set_xseq(&t, V90TX_XSEQ_JP, pk, 72), 0);
    CHECK_EQ_I(v90tx_set_xseq(&t, 99, pk, 72), V90_ERR_RANGE);

    CHECK_EQ_I(v90tx_select(&t, V90TX_SD), 0);
    CHECK(run_to(&t, V90TX_TRN1D, 600, 5000) >= 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_JD), 0);
    CHECK(run_to(&t, V90TX_JD, 80, 5000) >= 0);              /* inside the second Jd */
    CHECK_EQ_I(v90tx_select(&t, V90TX_JP), 0);
    /* 9.5.1.1.8: the current Jd is completed first */
    n = run_to(&t, V90TX_JP, 1, 5000);
    CHECK_EQ_I(n, 144 - 80 + 1);

    /* decode everything from here with the differential decoder that started
     * at Sd (it is self-synchronising): replay from a fresh transmitter */
    CHECK_EQ_I(v90tx_init(&t, alaw, (int)U), 0);
    (void)v90tx_set_jd(&t, &jd); (void)v90tx_load_dil(&t, &dil);
    (void)v90tx_set_xseq(&t, V90TX_XSEQ_JP, pk, 72);
    (void)v90tx_select(&t, V90TX_SD);
    v90dec_sign_init(&dj); v90dec_sign_init(&dt);
    for (i = 0; i < 6000; i++) {
        uint8_t o = v90tx_tick(&t);
        v90tx_sig_t s = v90tx_signal(&t);
        int bj, bt;
        if (s == V90TX_SD || s == V90TX_SD_BAR) continue;
        bj = v90dec_jd(&dj, v90_octet_sign(o)); bt = v90dec_trn1d(&dt, v90_octet_sign(o));
        if (s == V90TX_TRN1D && v90tx_symbols(&t) == 600) (void)v90tx_select(&t, V90TX_JD);
        if (s == V90TX_JD && v90tx_symbols(&t) == 100) (void)v90tx_select(&t, V90TX_JP);
        if (s == V90TX_JP) {
            if (jp_at < 0) jp_at = i;
            if (v90_octet_ucode(alaw, o) != U) bad_u++;
            if (ngot < (int)sizeof got) got[ngot++] = (uint8_t)bj;
            if (v90tx_symbols(&t) == 200) (void)v90tx_select(&t, V90TX_JP_PRIME);   /* in the third Jp */
        } else if (s == V90TX_JP_PRIME) {
            /* 8.6.4: 12 zeros, same modulation */
            CHECK_EQ_I(bj, 0);
            if (v90_octet_ucode(alaw, o) != U) bad_u++;
        } else if (s == V90TX_SCR) {
            /* 8.6.6: UINFO, signs are scrambled ones; the scrambler runs on, so
             * the TRN1d descrambler reads ones once 23 symbols have passed */
            if (v90_octet_ucode(alaw, o) != U) bad_u++;
            if (v90tx_symbols(&t) > 23) CHECK_EQ_I(bt, 1);
            if (v90tx_symbols(&t) == 60) (void)v90tx_select(&t, V90TX_RI);
        } else if (s == V90TX_DIL) {
            if (v90tx_symbols(&t) == 1) CHECK_EQ_I(v90_octet_ucode(alaw, o), 40);
            if (v90tx_symbols(&t) == 30) break;
        } else if (s == V90TX_RI) {
            CHECK_EQ_I(v90tx_symbols(&t) % 6 == 0 ? 6 : (int)(v90tx_symbols(&t) % 6), (i - jp_at - 216 - 12 - 60) % 6 + 1);
            if (v90tx_symbols(&t) == 12) break;
        }
    }
    CHECK(jp_at > 0);
    CHECK_EQ_I(ngot, 216);                                   /* three whole Jp sequences */
    for (i = 0; i < ngot && i < 216; i++) if (got[i] != jp[i % 72]) break;
    CHECK_EQ_I(i, 216);
    CHECK_EQ_I(bad_u, 0);
    CHECK_EQ_I(v90tx_signal(&t), dil_n ? V90TX_DIL : V90TX_RI);
}

/* ---- TRN2d, SUVd, CPd, SUVd', CPd', Ed, B1d (8.8) -------------------------- */
static void test_phase4(int alaw, int Sr)
{
    static v90tx_t t;
    static v90dec_t dec;
    v90_map_t tm, dm;
    v90_scr_t ds;
    uint8_t suvd[52], suvda[52], cpd[400], pk[4][50], oct[6], fb[V90_MAX_D];
    static uint8_t rx[20000];
    int i, k, D, nrx = 0, fon = 0, at[8], units;
    v90tx_sig_t last = V90TX_NONE;

    map_k(&tm, 10, 4, 100);                                  /* D = 16 - Sr */
    tm.Sr = (uint8_t)Sr; tm.ld = 1; tm.a1 = 60; tm.b1 = 10;
    map_k(&dm, 30, 32, 127);
    D = V90_MAP_D(&tm);
    for (i = 0; i < 52; i++) suvd[i] = (uint8_t)(i < 17 ? 1 : i == 17 ? 0 : (i % 3 == 0));
    memcpy(suvda, suvd, 52); suvda[33] ^= 1;
    for (i = 0; i < 400; i++) cpd[i] = (uint8_t)(i < 17 ? 1 : i == 17 ? 0 : ((i * 5 + 1) % 7 < 3));

    CHECK_EQ_I(v90tx_init(&t, alaw, 80), 0);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_TRAIN, &tm), 0);
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &dm), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_SUVD), -1);
    v90_pack_bits(suvd, 52, pk[0]); v90_pack_bits(suvda, 52, pk[1]);
    v90_pack_bits(cpd, 400, pk[2]);
    CHECK_EQ_I(v90tx_set_xseq(&t, V90TX_XSEQ_SUVD, pk[0], 52), 0);
    CHECK_EQ_I(v90tx_set_xseq(&t, V90TX_XSEQ_SUVD_ACK, pk[1], 52), 0);
    CHECK_EQ_I(v90tx_set_xseq(&t, V90TX_XSEQ_CPD, pk[2], 400), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_CPD_ACK), -1);         /* not loaded */
    cpd[33] ^= 1;
    v90_pack_bits(cpd, 400, pk[3]);
    CHECK_EQ_I(v90tx_set_xseq(&t, V90TX_XSEQ_CPD_ACK, pk[3], 400), 0);
    cpd[33] ^= 1;

    CHECK_EQ_I(v90tx_select(&t, V90TX_RI), 0);
    v90dec_init(&dec, &tm, alaw); v90_scr_init(&ds);
    memset(at, -1, sizeof at);
    units = (52 + D - 1) / D;                                /* data frames of one SUVd */
    for (i = 0; i < 9000 && nrx < (int)sizeof rx - 64; i++) {
        uint8_t o = v90tx_tick(&t);
        v90tx_sig_t s = v90tx_signal(&t);
        if (s == V90TX_RI && v90tx_symbols(&t) == 12) (void)v90tx_select(&t, V90TX_RI_BAR);
        if (s == V90TX_TRN2D && v90tx_symbols(&t) == 30) (void)v90tx_select(&t, V90TX_SUVD);
        /* 9.6.1.1.2: a single CPd after the SUVd in progress, then SUVd again */
        if (s == V90TX_SUVD && last != V90TX_CPD && v90tx_symbols(&t) == (uint32_t)(6 * units + 8)) (void)v90tx_select(&t, V90TX_CPD);
        if (s == V90TX_CPD && v90tx_symbols(&t) == 1) (void)v90tx_select(&t, V90TX_SUVD);
        if (s == V90TX_SUVD && last == V90TX_CPD && v90tx_symbols(&t) == 2) (void)v90tx_select(&t, V90TX_SUVD_ACK);
        if (s == V90TX_SUVD_ACK && v90tx_symbols(&t) == 2) (void)v90tx_select(&t, V90TX_CPD_ACK);
        if (s == V90TX_CPD_ACK && v90tx_symbols(&t) == 2) (void)v90tx_select(&t, V90TX_ED);
        if (s == V90TX_B1D) break;
        if (s != V90TX_TRN2D && s != V90TX_SUVD && s != V90TX_SUVD_ACK && s != V90TX_CPD && s != V90TX_CPD_ACK && s != V90TX_ED) continue;
        if (v90tx_symbols(&t) == 1) {
            if (s == V90TX_SUVD && at[1] < 0) at[1] = nrx;
            else if (s == V90TX_CPD) at[2] = nrx;
            else if (s == V90TX_SUVD && at[2] >= 0 && at[3] < 0) at[3] = nrx;
            else if (s == V90TX_SUVD_ACK) at[4] = nrx;
            else if (s == V90TX_CPD_ACK) at[5] = nrx;
            else if (s == V90TX_ED) at[6] = nrx;
            if (s != V90TX_TRN2D && s != last) CHECK_EQ_I(fon, 0);   /* on a data frame boundary */
            if (s == V90TX_CPD) last = V90TX_CPD;
        }
        oct[fon++] = o;
        if (fon == 6) {
            fon = 0;
            CHECK_EQ_I(v90dec_frame(&dec, oct, fb), D);
            for (k = 0; k < D; k++) rx[nrx++] = (uint8_t)v90_descramble_gpc(&ds, fb[k]);
        }
    }
    CHECK_EQ_I(v90tx_signal(&t), V90TX_B1D);
    /* TRN2d: scrambled ones */
    for (i = 23, k = 0; i < at[1]; i++) k += rx[i] != 1;
    CHECK(at[1] >= 30 / 6 * D); CHECK_EQ_I(k, 0);
    /* two SUVd, each filled with zeros to whole data frames (8.8.5) */
    CHECK_EQ_I(at[2] - at[1], 2 * units * D);
    for (i = 0, k = 0; i < 2 * units * D; i++) k += rx[at[1] + i] != ((i % (units * D)) < 52 ? suvd[i % (units * D)] : 0);
    CHECK_EQ_I(k, 0);
    /* one CPd, zero filled */
    CHECK_EQ_I(at[3] - at[2], (400 + D - 1) / D * D);
    for (i = 0, k = 0; i < at[3] - at[2]; i++) k += rx[at[2] + i] != (i < 400 ? cpd[i] : 0);
    CHECK_EQ_I(k, 0);
    CHECK_EQ_I(at[4] - at[3], units * D);                    /* one more SUVd */
    CHECK_EQ_I(at[5] - at[4], units * D);                    /* SUVd' */
    for (i = 0, k = 0; i < 52; i++) k += rx[at[4] + i] != suvda[i];
    CHECK_EQ_I(k, 0);
    CHECK_EQ_I(rx[at[5] + 33], !cpd[33]);                    /* CPd' */
    CHECK_EQ_I(at[6] - at[5], (400 + D - 1) / D * D);
    /* Ed: two data frames of zeros (8.8.2) */
    CHECK_EQ_I(nrx - at[6], 2 * D);
    for (i = at[6], k = 0; i < nrx; i++) k += rx[i];
    CHECK_EQ_I(k, 0);
}

/* ---- Rf (8.8.4) ------------------------------------------------------------ */
static void test_rf(int alaw)
{
    static v90tx_t t;
    v90_map_t dm;
    int i;
    map_k(&dm, 30, 32, 127);
    CHECK_EQ_I(v90tx_init(&t, alaw, 80), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RF), -1);              /* needs the data constellations */
    CHECK_EQ_I(v90tx_set_map(&t, V90TX_MAP_DATA, &dm), 0);
    CHECK_EQ_I(v90tx_select(&t, V90TX_RF), 0);
    for (i = 0; i < 36; i++) {
        uint8_t o = v90tx_tick(&t);
        /* + + - - + + - - + + - -, the largest point of each interval */
        CHECK_EQ_I(o, v90_pcm_octet(alaw, dm.C[i % 6][0], (i % 4) < 2));
        if (i == 25) CHECK_EQ_I(v90tx_select(&t, V90TX_RF_BAR), 0);   /* takes effect after the third Rf */
    }
    CHECK_EQ_I(v90tx_units(&t), 3);
    for (i = 0; i < 24; i++) {
        uint8_t o = v90tx_tick(&t);
        CHECK_EQ_I(v90tx_signal(&t), V90TX_RF_BAR);
        CHECK_EQ_I(o, v90_pcm_octet(alaw, dm.C[i % 6][0], (i % 4) >= 2));
    }
    (void)v90tx_tick(&t);
    CHECK_EQ_I(v90tx_signal(&t), V90TX_SILENCE);             /* two repetitions, then nothing by itself */
}

int main(void)
{
    test_jp(0, 0); test_jp(1, 0); test_jp(0, 3);
    test_phase4(0, 0); test_phase4(1, 0); test_phase4(0, 2);
    test_rf(0); test_rf(1);
    T_REPORT("tx92");
}
