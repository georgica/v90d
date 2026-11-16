/* Re-acquisition on the second S of Phase 3 (V.90 9.3.2.10): the caller's
 * modem was silent for seconds (while we sent TRN1d, Jd, the DIL), its clock
 * drifted and its carrier phase is arbitrary. The equaliser is kept; carrier
 * phase and timing phase are recovered from S, the symbol count from the
 * S / S-bar boundary. What follows S-bar (CPt in the real procedure) must be
 * received from its first symbol. */
#include <string.h>
#include "test.h"
#include "../modem/v34train.h"
#include "../modem/qamrx.h"
#include "../sim/qammod.h"
#include "../sim/noise.h"

#define SIL 300
#define NTRN 1500
#define NDATA 800

static int txn, gap, fill;
static double rot;
static v34trn_t txtrn, txdat, txscr;

static void sym(void *u, double *re, double *im)
{
    int32_t r = 0, i = 0;
    int n = txn++, first = SIL + V34_S_LEN + V34_SBAR_LEN + V34_PP_LEN + NTRN;
    (void)u;
    *re = *im = 0;
    if (n < SIL) return;
    if (n < first) {
        n -= SIL;
        if (n < V34_S_LEN) v34_s(n, V34_S_LEN, &r, &i);
        else if (n < V34_S_LEN + V34_SBAR_LEN) v34_sbar(n - V34_S_LEN, &r, &i);
        else if (n < V34_S_LEN + V34_SBAR_LEN + V34_PP_LEN) v34_pp(n - V34_S_LEN - V34_SBAR_LEN, &r, &i);
        else v34trn_next(&txtrn, &r, &i);
        *re = r / (double)QRX_REF; *im = i / (double)QRX_REF;
        return;
    }
    n -= first + gap;
    if (n < 0 && !(fill && n >= -gap + 400)) return;
    if (n < 0) v34trn_next(&txscr, &r, &i);        /* SCR while the DIL runs */
    else if (n < V34_S_LEN) v34_s(n, V34_S_LEN, &r, &i);
    else if (n < V34_S_LEN + V34_SBAR_LEN) v34_sbar(n - V34_S_LEN, &r, &i);
    else v34trn_next(&txdat, &r, &i);
    /* a carrier that did not keep its phase through the silence */
    *re = (r * cos(rot) - i * sin(rot)) / QRX_REF;
    *im = (i * cos(rot) + r * sin(rot)) / QRX_REF;
}

typedef struct { int found, errors, n; double mse_db; } res_t;

static void run(int rate, int cn, int cd, double ppm, int gap_syms, double phase, double snr_db, int scr, res_t *o)
{
    static qamrx_t rx; static sdet_t sd; static sres_t sr; qammod_t mod; rng_t rng; v34trn_t ref, dref;
    double fc = (double)rate * cn / cd, gain = 2000.0, nrms;
    int i, synced = 0, resumed = 0, searching = 0, total;

    txn = 0; gap = gap_syms; rot = phase; fill = scr;
    v34trn_init(&txscr, V34T_GPA); txscr.scr.sr = 0x2b3c5;
    v34trn_init(&txtrn, V34T_GPA); v34trn_init(&txdat, V34T_GPA);
    v34trn_init(&ref, V34T_GPA); v34trn_init(&dref, V34T_GPA);
    qammod_init(&mod, rate, ppm, fc, gain, sym, 0);
    qamrx_init(&rx, rate, cn, cd);
    sdet_init(&sd, rate, cn, cd);
    rng_seed(&rng, 77);
    nrms = snr_db > 0 ? gain / pow(10.0, snr_db / 20.0) : 0;
    memset(o, 0, sizeof *o);
    total = (int)((SIL + 2 * (V34_S_LEN + V34_SBAR_LEN) + V34_PP_LEN + NTRN + gap_syms + NDATA + 100) * 8000.0 / rate);
    for (i = 0; i < total; i++) {
        int16_t x = (int16_t)floor(qammod_tick(&mod) + nrms * rng_gauss(&rng) + 0.5);
        if (!synced && sdet_tick(&sd, x)) {
            qamrx_sync(&rx, (int32_t)((16.5 * 8000.0 / rate - SDET_DELAY(&sd) - 1) * 256));
            synced = 1;
            continue;
        }
        if (!qamrx_tick(&rx, x)) continue;
        if (!searching) {
            int32_t dr, di;
            int n = rx.nsym - 1;
            if (n < V34_PP_LEN) v34_pp(n, &dr, &di); else v34trn_next(&ref, &dr, &di);
            /* stop adapting a little before the signal ends, as on the end of Ja */
            if (n < V34_PP_LEN + NTRN - 40)
                qamrx_update(&rx, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
            else
                qamrx_update(&rx, dr, di, 0, 11);
            if (n == V34_PP_LEN + NTRN + 30) { sres_init(&sr); searching = 1; }
        } else if (!resumed) {
            int ev = sres_symbol(&sr, &rx);
            if (ev) { resumed = 1; o->found++; }
        } else {
            int32_t dr, di, sr2, si2;
            if (o->n >= NDATA) continue;
            v34trn_next(&dref, &dr, &di);
            sr2 = rx.zr < 0 ? -QRX_REF : QRX_REF; si2 = rx.zi < 0 ? -QRX_REF : QRX_REF;
            if (sr2 != dr || si2 != di) o->errors++;
            qamrx_update(&rx, sr2, si2, QRX_EQ | QRX_CARRIER | QRX_TIMING, 12);
            if (++o->n == 300) o->mse_db = 10 * log10(rx.mse / 65536.0 + 1e-12);
        }
    }
}

int main(void)
{
    static const int gaps[] = { 1500, 2777, 4001, 5555, 7003, 9000 };
    static const double phases[] = { 0.0, 1.0, 2.5, -2.0, 3.1, -0.7 };
    res_t o;
    int k, worst_err = 0, nfound = 0; double worst_mse = -99;

    for (k = 0; k < 6; k++) {
        run(3200, 3, 5, (k & 1) ? 90.0 : -90.0, gaps[k], phases[k], 0, 0, &o);
        nfound += o.found; if (o.errors > worst_err) worst_err = o.errors;
        if (o.mse_db > worst_mse) worst_mse = o.mse_db;
        if (o.found != 1 || o.errors || o.n != NDATA)
            printf("  3200 gap %d phase %.1f: found %d, %d symbols, %d errors, mse %.1f dB\n", gaps[k], phases[k], o.found, o.n, o.errors, o.mse_db);
        CHECK_EQ_I(o.found, 1); CHECK_EQ_I(o.n, NDATA); CHECK_EQ_I(o.errors, 0);
    }
    printf("  3200 clean, 6 gaps / phases / +-90 ppm: resumed %d of 6, worst mse after 300 symbols %.1f dB\n", nfound, worst_mse);
    CHECK(worst_mse < -27.0);

    worst_mse = -99;
    for (k = 0; k < 6; k++) {
        run(3000, 2, 3, (k & 1) ? -60.0 : 60.0, gaps[5 - k], phases[k], 32.0, 0, &o);
        if (o.mse_db > worst_mse) worst_mse = o.mse_db;
        if (o.found != 1 || o.errors || o.n != NDATA)
            printf("  3000 gap %d phase %.1f: found %d, %d symbols, %d errors, mse %.1f dB\n", gaps[5 - k], phases[k], o.found, o.n, o.errors, o.mse_db);
        CHECK_EQ_I(o.found, 1); CHECK_EQ_I(o.n, NDATA); CHECK_EQ_I(o.errors, 0);
    }
    printf("  3000, 32 dB, 6 gaps / phases / +-60 ppm: worst mse after 300 symbols %.1f dB\n", worst_mse);
    CHECK(worst_mse < -25.0);

    /* SCR (random 4-point symbols) right up to S must not be taken for S */
    for (k = 0; k < 6; k++) {
        run(3200, 3, 5, (k & 1) ? 50.0 : -50.0, gaps[k], phases[5 - k], 34.0, 1, &o);
        if (o.found != 1 || o.errors || o.n != NDATA)
            printf("  SCR then S, gap %d: found %d, %d symbols, %d errors, mse %.1f dB\n", gaps[k], o.found, o.n, o.errors, o.mse_db);
        CHECK_EQ_I(o.found, 1); CHECK_EQ_I(o.n, NDATA); CHECK_EQ_I(o.errors, 0);
    }

    T_REPORT("sres");
}
