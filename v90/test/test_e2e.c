/* Whole start-up from Sd to data mode, driven the way a Phase 3/4 state
 * machine would drive the transmitter (9.3.1, 9.4.1), over an ideal channel
 * into the reference decoder. Every octet sent is accounted for:
 *
 *   Sd, Sd-bar      pattern and length
 *   TRN1d           descrambles to ones
 *   Jd, Jd'         parsed back, CRC good, equal to what was set
 *   DIL             equal to a model written here from 8.4.1
 *   Ri, Ri-bar      pattern and length
 *   TRN2d           5.4 decoder + descrambler give ones
 *   MP, MP'         parsed back from each sequence, zero fill, frame aligned
 *   Ed              two data frames of zeros
 *   B1d             48 data frames of ones with the data mode parameters
 *   data            the bits handed to the transmitter
 *
 * The DIL descriptor, CPt and CP reach the transmitter as bit arrays through
 * the parsers, as they would from the analogue modem. Run for both laws and
 * every Sr / ld combination. */
#include <string.h>
#include "test.h"
#include "v90_tx.h"
#include "v90_pcm.h"
#include "v90_dec.h"

#define MAXT 12000

static uint8_t  oct[MAXT];
static int8_t   sig[MAXT];
static int      ntick;

static void setbit(uint8_t *m, int u) { m[u >> 3] |= (uint8_t)(1u << (u & 7)); }
static int  getbitm(const uint8_t *m, int u) { return (m[u >> 3] >> (u & 7)) & 1; }

/* user data: a PRBS the test can regenerate */
typedef struct { uint32_t s; long count; } src_t;
static int src_bit(void *ctx)
{
    src_t *p = (src_t *)ctx;
    p->s = p->s * 1103515245u + 12345u;
    p->count++;
    return (int)((p->s >> 16) & 1);
}

static void tick(v90tx_t *t)
{
    if (ntick < MAXT) {
        oct[ntick] = v90tx_tick(t);
        sig[ntick] = (int8_t)v90tx_signal(t);
        ntick++;
    }
}

/* a constellation of M points ending at Ucode `top`, spacing `step` */
static void cons(uint8_t *mask, int M, int top, int step)
{
    int k;
    memset(mask, 0, 16);
    for (k = 0; k < M; k++) setbit(mask, top - k * step);
}

/* length of the run of signal s starting at tick i */
static int runlen(int i, int s)
{
    int n = 0;
    while (i + n < ntick && sig[i + n] == s) n++;
    return n;
}

typedef struct {
    int alaw, uinfo;
    int Sr_t, ld_t, Sr_d, ld_d;
    int Mt, Md, Kt, Kd;       /* constellation sizes and K for CPt / CP */
    int mp_type;
    int dil_n;
    int idle;                 /* silence ticks before Sd */
} scen_t;

static void run_scenario(const scen_t *sc)
{
    static v90tx_t tx;
    static v90_dil_t dil, dil_rx;
    static v90_cp_t cpt, cp, cp_rx;
    static v90_map_t tmap, dmap;
    static uint8_t bits[V90_DIL_MAX_BITS];
    v90_jd_t jd;
    v90_mp_t mp;
    src_t src;
    int alaw = sc->alaw, i, k, n, pos, step = 0, guard = 0;
    int bad, mpfr_drv = 0;

    ntick = 0;
    memset(&jd, 0, sizeof jd);
    jd.rates = 0x2AAAAB; jd.cp16_train = 0; jd.cp16_reneg = 1; jd.max_ld = 3;
    memset(&mp, 0, sizeof mp);
    mp.type = (uint8_t)sc->mp_type; mp.max_rate = 11; mp.trellis = 0; mp.rates = 0x03FF;
    if (mp.type) { mp.h[0][0] = 1000; mp.h[0][1] = -2000; mp.h[2][1] = 77; }

    /* DIL descriptor as the analogue modem would send it in Ja */
    memset(&dil, 0, sizeof dil);
    dil.n = (uint8_t)sc->dil_n; dil.lsp = 5; dil.ltp = 12;
    dil.sp[0] = 0x16;                                    /* 0,1,1,0,1 */
    dil.tp[0] = 0xDB; dil.tp[1] = 0x06;                  /* 1,1,0,1,1,0,1,1,0,1,1,0 */
    for (i = 0; i < 8; i++) { dil.h[i] = (uint8_t)(i % 3); dil.ref[i] = (uint8_t)(16 * i + 3); }
    for (i = 0; i < dil.n; i++) dil.train[i] = (uint8_t)((i * 29 + 7) & 127);
    if (dil.n == 0) { dil.lsp = 1; dil.ltp = 1; }
    n = v90_dil_build(&dil, bits);
    CHECK(n > 0);
    CHECK_EQ_I(v90_dil_parse(bits, n, &dil_rx), n);

    /* CPt: D = drn + 8 = Kt + 6 - Sr_t; two constellations used alternately */
    memset(&cpt, 0, sizeof cpt);
    cpt.is_cp = 0; cpt.drn = (uint8_t)(sc->Kt + 6 - sc->Sr_t - 8);
    cpt.sr = (uint8_t)sc->Sr_t; cpt.ld = (uint8_t)sc->ld_t; cpt.alaw = (uint8_t)alaw;
    cpt.rates = 0x1FFF; cpt.rms_ratio = 0x2000;
    cpt.a1 = 64; cpt.a2 = 0; cpt.b1 = -24; cpt.b2 = 0;
    for (i = 0; i < 6; i++) cpt.idx[i] = (uint8_t)(i & 1);
    cons(cpt.mask[0], sc->Mt, 90, 9);
    cons(cpt.mask[1], sc->Mt, 85, 8);
    /* CP: D = drn + 20 = Kd + 6 - Sr_d; three constellations */
    memset(&cp, 0, sizeof cp);
    cp.is_cp = 1; cp.drn = (uint8_t)(sc->Kd + 6 - sc->Sr_d - 20);
    cp.sr = (uint8_t)sc->Sr_d; cp.ld = (uint8_t)sc->ld_d; cp.alaw = (uint8_t)alaw;
    cp.rates = 0x1FFF; cp.rms_ratio = 0x2000; cp.ack = 1;
    cp.a1 = 60; cp.a2 = -58; cp.b1 = 10; cp.b2 = -10;
    for (i = 0; i < 6; i++) cp.idx[i] = (uint8_t)(i % 3);
    cons(cp.mask[0], sc->Md, 126, 1);
    cons(cp.mask[1], sc->Md, 125, 1);
    cons(cp.mask[2], sc->Md, 127, 1);

    CHECK_EQ_I(v90tx_init(&tx, alaw, sc->uinfo), 0);
    CHECK_EQ_I(v90tx_set_jd(&tx, &jd), 0);
    CHECK_EQ_I(v90tx_load_dil(&tx, &dil_rx), 0);
    src.s = 2024; src.count = 0;
    v90tx_set_getbit(&tx, src_bit, &src);

    for (i = 0; i < sc->idle; i++) (void)v90tx_tick(&tx);
    CHECK_EQ_I(v90tx_select(&tx, V90TX_SD), 0);

    /* ---- the state machine's side ---- */
    while (ntick < MAXT && guard++ < 10 * MAXT) {
        v90tx_sig_t s;
        uint32_t ns, nu;
        tick(&tx);
        s = v90tx_signal(&tx); ns = v90tx_symbols(&tx); nu = v90tx_units(&tx);
        switch (step) {
        case 0:   /* 9.3.1.4: TRN1d for at least 2040T, then Jd */
            if (s == V90TX_TRN1D && ns >= 2040) { CHECK_EQ_I(v90tx_select(&tx, V90TX_JD), 0); step++; }
            break;
        case 1:   /* 9.3.1.5: S detected during the third Jd: finish it, send Jd' */
            if (s == V90TX_JD && ns == 72 * 2 + 17) { CHECK_EQ_I(v90tx_select(&tx, V90TX_JD_PRIME), 0); step++; }
            break;
        case 2:   /* 9.3.1.6: DIL until the S-to-S-bar transition, here in the middle
                     of a segment of the third pass; no DIL when N = 0 */
            if (sc->dil_n == 0) { if (s == V90TX_RI) step++; }
            else if (s == V90TX_DIL && nu == (uint32_t)(2 * sc->dil_n + 1) && v90tx_phase(&tx) == 2) {
                CHECK_EQ_I(v90tx_select(&tx, V90TX_RI), 0); step++;
            }
            break;
        case 3:   /* 9.4.1.1, 9.4.1.2: Ri for at least 192T; CPt arrives; Ri-bar */
            if (s == V90TX_RI && ns >= 195) {
                n = v90_cp_build(&cpt, bits);
                CHECK(n > 0);
                CHECK_EQ_I(v90_cp_parse(bits, n, &cp_rx), n);
                CHECK_EQ_I(v90tx_load_cp(&tx, &cp_rx), 0);
                CHECK_EQ_I(v90_map_from_cp(&tmap, &cp_rx), 0);
                /* data frames per MP: its bits, zero filled to whole frames */
                mpfr_drv = ((sc->mp_type ? V90_MP1_BITS : V90_MP0_BITS) + V90_MAP_D(&tmap) - 1) / V90_MAP_D(&tmap);
                CHECK_EQ_I(v90tx_select(&tx, V90TX_RI_BAR), 0);
                step++;
            }
            break;
        case 4:   /* 9.4.1.3: TRN2d for at least 2040T, then MP */
            if (s == V90TX_TRN2D && ns >= 2040) {
                CHECK_EQ_I(v90tx_set_mp(&tx, &mp), 0);
                CHECK_EQ_I(v90tx_select(&tx, V90TX_MP), 0);
                step++;
            }
            break;
        case 5:   /* CP received during the third MP: finish it, then MP' */
            if (s == V90TX_MP && nu == 2 && v90tx_phase(&tx) == 4) {
                n = v90_cp_build(&cp, bits);
                CHECK(n > 0);
                CHECK_EQ_I(v90_cp_parse(bits, n, &cp_rx), n);
                CHECK_EQ_I(v90tx_load_cp(&tx, &cp_rx), 0);
                CHECK_EQ_I(v90_map_from_cp(&dmap, &cp_rx), 0);
                CHECK_EQ_I(v90tx_select(&tx, V90TX_MP_ACK), 0);
                step++;
            }
            break;
        case 6:   /* 9.4.1.4: CP' received near the end of the second MP': one Ed.
                     B1d and data follow by themselves (9.4.1.5). */
            if (s == V90TX_MP_ACK && nu == 1 && ns >= 2 * 6 * (uint32_t)mpfr_drv - 3) {
                CHECK_EQ_I(v90tx_select(&tx, V90TX_ED), 0);
                step++;
            }
            break;
        default:
            break;
        }
    }
    CHECK_EQ_I(step, 7);
    CHECK_EQ_I(ntick, MAXT);

    /* ---- the analogue modem's side ---- */
    pos = 0;

    /* Sd: 64 x {+W,+0,+W,-W,-0,-W}; Sd-bar: 8 x the inverse */
    {
        unsigned W = (unsigned)sc->uinfo + 16;
        bad = 0;
        CHECK_EQ_I(runlen(pos, V90TX_SD), 384);
        for (i = 0; i < 384; i++)
            if (oct[pos + i] != v90_pcm_octet(alaw, (i % 3 == 1) ? 0 : W, (i % 6) < 3)) bad++;
        pos += 384;
        CHECK_EQ_I(runlen(pos, V90TX_SD_BAR), 48);
        for (i = 0; i < 48; i++)
            if (oct[pos + i] != v90_pcm_octet(alaw, (i % 3 == 1) ? 0 : W, (i % 6) >= 3)) bad++;
        pos += 48;
        CHECK_EQ_I(bad, 0);
    }

    /* TRN1d, Jd, Jd' */
    {
        v90dec_sign_t rx;
        v90_jd_t got;
        uint8_t jb[72];
        int j;
        v90dec_sign_init(&rx);
        n = runlen(pos, V90TX_TRN1D);
        CHECK(n >= 2040 && n < 2040 + 12);
        CHECK_EQ_I(n % 6, 0);
        bad = 0;
        for (i = 0; i < n; i++) {
            if (v90_octet_ucode(alaw, oct[pos + i]) != (unsigned)sc->uinfo) bad++;
            if (v90dec_trn1d(&rx, v90_octet_sign(oct[pos + i])) != 1) bad++;
        }
        CHECK_EQ_I(bad, 0);
        pos += n;
        CHECK_EQ_I(runlen(pos, V90TX_JD), 72 * 3);
        for (j = 0; j < 3; j++) {
            bad = 0;
            for (i = 0; i < 72; i++) {
                if (v90_octet_ucode(alaw, oct[pos + i]) != (unsigned)sc->uinfo) bad++;
                jb[i] = (uint8_t)v90dec_jd(&rx, v90_octet_sign(oct[pos + i]));
            }
            CHECK_EQ_I(bad, 0);
            memset(&got, 0, sizeof got);
            CHECK_EQ_I(v90_jd_parse(jb, 72, &got), 72);
            CHECK_EQ_I(got.rates, jd.rates);
            CHECK_EQ_I(got.cp16_train, 0); CHECK_EQ_I(got.cp16_reneg, 1); CHECK_EQ_I(got.max_ld, 3);
            pos += 72;
        }
        CHECK_EQ_I(runlen(pos, V90TX_JD_PRIME), 12);
        bad = 0;
        for (i = 0; i < 12; i++) {
            if (v90_octet_ucode(alaw, oct[pos + i]) != (unsigned)sc->uinfo) bad++;
            if (v90dec_jd(&rx, v90_octet_sign(oct[pos + i])) != 0) bad++;
        }
        CHECK_EQ_I(bad, 0);
        pos += 12;
    }

    /* DIL, modelled from 8.4.1: segment s uses training Ucode train[s]; with c
     * its Uchord, the segment is (Hc + 1) * 6 symbols; symbol m has the sign
     * SP[m mod LSP] and is the training Ucode where TP[m mod LTP] = 1, REFc
     * otherwise. Ends on a segment boundary. */
    if (sc->dil_n > 0) {
        int seg = 0, nseg = 0;
        n = runlen(pos, V90TX_DIL);
        bad = 0; i = 0;
        while (i < n) {
            int u = dil.train[seg], c = u / 16, L = (dil.h[c] + 1) * 6, m;
            if (i + L > n) { bad++; break; }             /* cut inside a segment */
            for (m = 0; m < L; m++) {
                int sgn = getbitm(dil.sp, m % dil.lsp);
                int mag = getbitm(dil.tp, m % dil.ltp) ? u : dil.ref[c];
                if (oct[pos + i + m] != v90_pcm_octet(alaw, (unsigned)mag, sgn)) bad++;
            }
            i += L; nseg++;
            seg = (seg + 1) % dil.n;
        }
        CHECK_EQ_I(bad, 0);
        CHECK_EQ_I(nseg, 2 * sc->dil_n + 2);             /* the segment in progress was completed */
        pos += n;
    }

    /* Ri, Ri-bar */
    {
        n = runlen(pos, V90TX_RI);
        CHECK(n >= 192 && n <= 192 + 12);
        CHECK_EQ_I(n % 6, 0);
        bad = 0;
        for (i = 0; i < n; i++)
            if (oct[pos + i] != v90_pcm_octet(alaw, (unsigned)sc->uinfo, (i % 6) < 3)) bad++;
        pos += n;
        CHECK_EQ_I(runlen(pos, V90TX_RI_BAR), 24);
        for (i = 0; i < 24; i++)
            if (oct[pos + i] != v90_pcm_octet(alaw, (unsigned)sc->uinfo, (i % 6) >= 3)) bad++;
        pos += 24;
        CHECK_EQ_I(bad, 0);
    }

    /* TRN2d, MP, MP', Ed with the CPt parameters */
    {
        v90dec_t dec;
        v90_scr_t ds;
        uint8_t fb[V90_MAX_D];
        static uint8_t mb[V90_MP_MAX_BITS + V90_MAX_D];
        int D = V90_MAP_D(&tmap), mplen = sc->mp_type ? V90_MP1_BITS : V90_MP0_BITS;
        int mpfr = (mplen + D - 1) / D, pass;
        CHECK_EQ_I(tmap.K, sc->Kt);
        CHECK_EQ_I(tmap.Sr, sc->Sr_t);
        v90dec_init(&dec, &tmap, alaw);
        v90_scr_init(&ds);
        n = runlen(pos, V90TX_TRN2D);
        CHECK(n >= 2040 && n < 2040 + 12);
        CHECK_EQ_I(n % 6, 0);
        bad = 0;
        for (i = 0; i < n; i += 6) {
            if (v90dec_frame(&dec, oct + pos + i, fb) != D) { bad++; continue; }
            for (k = 0; k < D; k++) if (v90_descramble_gpc(&ds, fb[k]) != 1) bad++;
        }
        CHECK_EQ_I(bad, 0);
        pos += n;
        for (pass = 0; pass < 2; pass++) {
            int s = pass ? V90TX_MP_ACK : V90TX_MP, want_units = pass ? 2 : 3, j, f;
            v90_mp_t got;
            n = runlen(pos, s);
            CHECK_EQ_I(n, want_units * mpfr * 6);
            for (j = 0; j + mpfr * 6 <= n; j += mpfr * 6) {
                bad = 0;
                for (f = 0; f < mpfr; f++) {
                    if (v90dec_frame(&dec, oct + pos + j + f * 6, fb) != D) { bad++; continue; }
                    for (k = 0; k < D; k++) mb[f * D + k] = (uint8_t)v90_descramble_gpc(&ds, fb[k]);
                }
                CHECK_EQ_I(bad, 0);
                memset(&got, 0xEE, sizeof got);
                CHECK_EQ_I(v90_mp_parse(mb, mpfr * D, &got), mplen);
                CHECK_EQ_I(got.type, mp.type); CHECK_EQ_I(got.ack, pass);
                CHECK_EQ_I(got.max_rate, 11); CHECK_EQ_I(got.rates, 0x03FF);
                CHECK_EQ_I(got.trellis, 0);
                CHECK_EQ_I(got.h[0][0], mp.h[0][0]); CHECK_EQ_I(got.h[0][1], mp.h[0][1]);
                CHECK_EQ_I(got.h[2][1], mp.h[2][1]);
                bad = 0;
                for (k = mplen; k < mpfr * D; k++) if (mb[k]) bad++;    /* fill */
                CHECK_EQ_I(bad, 0);
            }
            pos += n;
        }
        CHECK_EQ_I(runlen(pos, V90TX_ED), 12);
        bad = 0;
        for (i = 0; i < 12; i += 6) {
            if (v90dec_frame(&dec, oct + pos + i, fb) != D) { bad++; continue; }
            for (k = 0; k < D; k++) if (v90_descramble_gpc(&ds, fb[k]) != 0) bad++;
        }
        CHECK_EQ_I(bad, 0);
        pos += 12;
    }

    /* B1d and data with the CP parameters */
    {
        v90dec_t dec;
        v90_scr_t ds;
        src_t ref;
        uint8_t fb[V90_MAX_D];
        int D = V90_MAP_D(&dmap), nd = 0;
        CHECK_EQ_I(dmap.K, sc->Kd);
        CHECK_EQ_I(dmap.Sr, sc->Sr_d);
        CHECK_EQ_I(D * 4000 / 3, V90_RATE_BPS(cp.drn - 1));
        v90dec_init(&dec, &dmap, alaw);
        v90_scr_init(&ds);
        CHECK_EQ_I(runlen(pos, V90TX_B1D), 288);
        bad = 0;
        for (i = 0; i < 288; i += 6) {
            if (v90dec_frame(&dec, oct + pos + i, fb) != D) { bad++; continue; }
            for (k = 0; k < D; k++) if (v90_descramble_gpc(&ds, fb[k]) != 1) bad++;
        }
        CHECK_EQ_I(bad, 0);
        pos += 288;
        n = runlen(pos, V90TX_DATA);
        CHECK_EQ_I(pos + n, ntick);                      /* data to the end of the log */
        CHECK(n >= 6 * 100);
        ref.s = 2024; ref.count = 0;
        bad = 0;
        for (i = 0; i + 6 <= n; i += 6) {
            if (v90dec_frame(&dec, oct + pos + i, fb) != D) { bad++; continue; }
            for (k = 0; k < D; k++, nd++) if (v90_descramble_gpc(&ds, fb[k]) != src_bit(&ref)) bad++;
        }
        CHECK_EQ_I(bad, 0);
        CHECK(nd >= 100 * D);
        /* the transmitter took at most the lookahead's worth of bits early */
        CHECK(src.count >= nd && src.count <= nd + 4 * D);
    }
    /* 8.4.4: everything stayed on the 6-symbol grid that began with Sd */
    bad = 0;
    for (i = 1; i < ntick; i++) if (sig[i] != sig[i - 1] && (i % 6) != 0) bad++;
    CHECK_EQ_I(bad, 0);
}

int main(void)
{
    scen_t sc;
    int alaw, Sr, ld, v, runs = 0;
    for (alaw = 0; alaw < 2; alaw++)
    for (Sr = 0; Sr <= 3; Sr++)
    for (ld = 0; ld <= 3; ld++)
    for (v = 0; v < 2; v++) {
        memset(&sc, 0, sizeof sc);
        sc.alaw = alaw; sc.uinfo = alaw ? 67 : 111;
        sc.Sr_t = Sr; sc.ld_t = ld;
        sc.Sr_d = v ? (Sr + 1) % 4 : Sr;
        sc.ld_d = v ? 3 - ld : ld;
        /* CPt: 5-point constellations, 5^6 = 15625 >= 2^13
         * CP : 33-point, 33^6 >= 2^30; or the top rate, 91-point, 91^6 >= 2^39 */
        sc.Mt = 5; sc.Kt = 13;
        if (v && sc.Sr_d == 3) { sc.Md = 91; sc.Kd = 39; }       /* 56000 bit/s */
        else                   { sc.Md = 33; sc.Kd = 30; }
        sc.mp_type = v;
        sc.dil_n = v ? 0 : 5 + Sr;
        sc.idle = 4 * v + ld;
        run_scenario(&sc);
        runs++;
    }
    printf("  %d start-ups decoded\n", runs);
    T_REPORT("test_e2e");
}
