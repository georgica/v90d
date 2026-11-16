/* Tests for the Phase 3 training signal references and the S detector. */
#include <string.h>
#include "test.h"
#include "../modem/v34train.h"
#include "../modem/qamrx.h"
#include "../sim/qammod.h"
#include "../sim/noise.h"

#define PI 3.14159265358979323846

/* ---- a test transmitter: silence, S, S-bar, PP, TRN ---- */
#define SIL 300
static int txn;
static v34trn_t txtrn;
static void train_sym(void *u, double *re, double *im)
{
    int32_t r = 0, i = 0;
    int n = txn++;
    (void)u;
    if (n < SIL) { *re = *im = 0; return; }
    n -= SIL;
    if (n < V34_S_LEN) v34_s(n, V34_S_LEN, &r, &i);
    else if (n < V34_S_LEN + V34_SBAR_LEN) v34_sbar(n - V34_S_LEN, &r, &i);
    else if (n < V34_S_LEN + V34_SBAR_LEN + V34_PP_LEN) v34_pp(n - V34_S_LEN - V34_SBAR_LEN, &r, &i);
    else v34trn_next(&txtrn, &r, &i);
    *re = r / (double)QRX_REF; *im = i / (double)QRX_REF;
}

static void acquire(int rate, int cn, int cd, double gain, double snr_db, double tilt, int chdelay,
                    double *det_err, double *mse_db, int *ntone_events, double *dd_mse_db, int *dd_errors)
{
    static qamrx_t rx; static sdet_t sd; qammod_t mod; rng_t rng; v34trn_t ref;
    static double dl[64];
    double fc = (double)rate * cn / cd, z1 = 0, nrms, bound;
    int i, synced = 0, nev = 0, dpos = 0;

    txn = 0; v34trn_init(&txtrn, V34T_GPA); v34trn_init(&ref, V34T_GPA);
    qammod_init(&mod, rate, 0, fc, gain, train_sym, 0);
    qamrx_init(&rx, rate, cn, cd);
    sdet_init(&sd, rate, cn, cd);
    rng_seed(&rng, 31);
    memset(dl, 0, sizeof dl);
    /* line RMS = gain (unit-power symbols have |a|^2 = 2, pass-band halves it) */
    nrms = snr_db > 0 ? gain / pow(10.0, snr_db / 20.0) : 0;
    /* the S / S-bar boundary at the receiver input, in samples */
    bound = qammod_delay(&mod) + (SIL + V34_S_LEN - 0.5) * mod.sps + chdelay;
    *det_err = 1e9; *mse_db = 0; *dd_mse_db = 0; *dd_errors = 0;
    for (i = 0; i < 12000; i++) {
        double v = qammod_tick(&mod), y;
        int16_t x;
        dl[dpos] = v; y = dl[(dpos - chdelay) & 63]; dpos = (dpos + 1) & 63;
        v = y + tilt * z1; z1 = y;
        x = (int16_t)floor(v + nrms * rng_gauss(&rng) + 0.5);
        if (sdet_tick(&sd, x)) {
            nev++;
            if (!synced) {
                /* the event came SDET_DELAY after the boundary; PP(0) is centred
                 * 16.5 symbols after the boundary */
                double sps = 8000.0 / rate;
                *det_err = i - (bound + SDET_DELAY(&sd));
                qamrx_sync(&rx, (int32_t)((16.5 * sps - SDET_DELAY(&sd) - 1) * 256));
                synced = 1;
                continue;
            }
        }
        if (qamrx_tick(&rx, x)) {
            int32_t dr, di;
            int n = rx.nsym - 1;
            if (n < V34_PP_LEN) v34_pp(n, &dr, &di); else v34trn_next(&ref, &dr, &di);
            if (n >= V34_PP_LEN + 512) {
                /* the known part is over: carry on decision-directed, as on Ja */
                int32_t sr = rx.zr < 0 ? -QRX_REF : QRX_REF, si = rx.zi < 0 ? -QRX_REF : QRX_REF;
                if (sr != dr || si != di) (*dd_errors)++;
                dr = sr; di = si;
            }
            qamrx_update(&rx, dr, di, QRX_EQ | QRX_CARRIER | (n > 300 ? QRX_TIMING : 0), n < 600 ? 10 : 11);
            if (n == V34_PP_LEN + 512 - 1) *mse_db = 10 * log10(rx.mse / 65536.0);
            if (n == V34_PP_LEN + 512 + 2000 - 1) *dd_mse_db = 10 * log10(rx.mse / 65536.0);
        }
    }
    *ntone_events = nev;
}

int main(void)
{
    int32_t re, im;
    int i, k;

    /* ---- PP: spot values from equation 10-1 and the property it is built for ---- */
    v34_pp(0, &re, &im); CHECK_NEAR(re, QRX_REF * sqrt(2.0), 3); CHECK_NEAR(im, 0, 3);
    v34_pp(3, &re, &im); CHECK_NEAR(re, QRX_REF * sqrt(2.0), 3);            /* k = 0: all ones */
    v34_pp(4, &re, &im);                                                   /* k = 1, I = 0: 120 deg */
    CHECK_NEAR(re, QRX_REF * sqrt(2.0) * cos(2 * PI / 3), 3); CHECK_NEAR(im, QRX_REF * sqrt(2.0) * sin(2 * PI / 3), 3);
    v34_pp(7, &re, &im);                                                   /* k = 1, I = 3: 210 deg */
    CHECK_NEAR(re, QRX_REF * sqrt(2.0) * cos(7 * PI / 6), 3); CHECK_NEAR(im, QRX_REF * sqrt(2.0) * sin(7 * PI / 6), 3);
    v34_pp(9, &re, &im);                                                   /* k = 2, I = 1: 60 deg */
    CHECK_NEAR(re, QRX_REF * sqrt(2.0) * cos(PI / 3), 3); CHECK_NEAR(im, QRX_REF * sqrt(2.0) * sin(PI / 3), 3);
    {
        /* period 48, constant amplitude, zero periodic autocorrelation */
        double pr[48], pi[48]; int ok_period = 1; double worst = 0;
        for (i = 0; i < 48; i++) { v34_pp(i, &re, &im); pr[i] = re; pi[i] = im; }
        for (i = 48; i < V34_PP_LEN; i++) {
            v34_pp(i, &re, &im);
            if (fabs(re - pr[i % 48]) > 1 || fabs(im - pi[i % 48]) > 1) ok_period = 0;
        }
        CHECK(ok_period);
        for (k = 1; k < 48; k++) {
            double ar = 0, ai = 0;
            for (i = 0; i < 48; i++) {
                int j = (i + k) % 48;
                ar += pr[j] * pr[i] + pi[j] * pi[i];
                ai += pi[j] * pr[i] - pr[j] * pi[i];
            }
            if (sqrt(ar * ar + ai * ai) > worst) worst = sqrt(ar * ar + ai * ai);
        }
        CHECK(worst < 48.0 * QRX_REF * QRX_REF * 2 * 0.002);
    }

    /* ---- S and S-bar ---- */
    v34_s(127, 128, &re, &im); CHECK_EQ_I(re, -QRX_REF); CHECK_EQ_I(im, QRX_REF);   /* ends on +90 deg */
    v34_s(126, 128, &re, &im); CHECK_EQ_I(re, QRX_REF); CHECK_EQ_I(im, QRX_REF);    /* point 0 */
    v34_s(0, 128, &re, &im); CHECK_EQ_I(re, QRX_REF); CHECK_EQ_I(im, QRX_REF);
    v34_sbar(0, &re, &im); CHECK_EQ_I(re, -QRX_REF); CHECK_EQ_I(im, -QRX_REF);      /* starts on 180 deg */
    v34_sbar(1, &re, &im); CHECK_EQ_I(re, QRX_REF); CHECK_EQ_I(im, -QRX_REF);       /* 270 deg */

    /* ---- scrambler: ones into a cleared GPA register (worked by hand) ---- */
    {
        static const char *want = "11111000001111100000111001";
        v34scr_t s, d; int ok = 1;
        v34scr_init(&s, V34T_GPA);
        for (i = 0; want[i]; i++) if (v34scr_put(&s, 1) != want[i] - '0') ok = 0;
        CHECK(ok);
        /* GPC differs: first change at bit 18 */
        v34scr_init(&s, V34T_GPC);
        for (i = 0, ok = 1; i < 18; i++) if (v34scr_put(&s, 1) != 1) ok = 0;
        CHECK(ok); CHECK_EQ_I(v34scr_put(&s, 1), 0);
        /* descrambler inverts it and self-synchronises */
        v34scr_init(&s, V34T_GPA); v34scr_init(&d, V34T_GPA);
        d.sr = 0x55aa55;                         /* wrong start state */
        for (i = 0, ok = 1; i < 200; i++) {
            int b = (i * 7 + i / 3) & 1, o = v34scr_get(&d, v34scr_put(&s, b));
            if (i >= 23 && o != b) ok = 0;
        }
        CHECK(ok);
    }
    /* ---- TRN: first symbols from the hand-worked bits: (1,1) (1,1) (1,0) (0,0) ... ---- */
    {
        v34trn_t t;
        v34trn_init(&t, V34T_GPA);
        v34trn_next(&t, &re, &im); CHECK_EQ_I(re, -QRX_REF); CHECK_EQ_I(im, QRX_REF);   /* I = 3: 270 deg clockwise */
        v34trn_next(&t, &re, &im); CHECK_EQ_I(re, -QRX_REF); CHECK_EQ_I(im, QRX_REF);
        v34trn_next(&t, &re, &im); CHECK_EQ_I(re, QRX_REF); CHECK_EQ_I(im, -QRX_REF);   /* I1 = 1, I2 = 0: 90 deg clockwise */
        v34trn_next(&t, &re, &im); CHECK_EQ_I(re, QRX_REF); CHECK_EQ_I(im, QRX_REF);    /* I = 0 */
    }

    /* ---- acquisition: S detector fires at the S / S-bar boundary, the equaliser
     *      trains on PP + 512 T of TRN ---- */
    {
        double derr, mse, dd; int nev, errs;
        acquire(3200, 3, 5, 3000.0, 0, 0, 0, &derr, &mse, &nev, &dd, &errs);
        printf("  3200 clean: S detector %.1f samples off, mse %.1f dB after PP + 512 TRN, %.1f dB after 2000 more, %d errors\n", derr, mse, dd, errs);
        CHECK_NEAR(derr, 0, 2.5); CHECK(mse < -18.0); CHECK(dd < -38.0); CHECK_EQ_I(errs, 0); CHECK_EQ_I(nev, 1);
        acquire(3000, 2, 3, 3000.0, 0, 0, 0, &derr, &mse, &nev, &dd, &errs);
        printf("  3000 clean: S detector %.1f samples off, mse %.1f dB after PP + 512 TRN, %.1f dB after 2000 more, %d errors\n", derr, mse, dd, errs);
        CHECK_NEAR(derr, 0, 2.5); CHECK(mse < -18.0); CHECK(dd < -38.0); CHECK_EQ_I(errs, 0); CHECK_EQ_I(nev, 1);
        acquire(3200, 3, 5, 150.0, 28.0, -0.5, 7, &derr, &mse, &nev, &dd, &errs);
        printf("  3200 weak, 28 dB, tilt: S detector %.1f samples off, mse %.1f dB after PP + 512 TRN, %.1f dB after 2000 more, %d errors\n", derr, mse, dd, errs);
        CHECK_NEAR(derr, 0, 4.0); CHECK(mse < -18.0); CHECK(dd < -25.0); CHECK_EQ_I(errs, 0); CHECK_EQ_I(nev, 1);
    }

    T_REPORT("v34train");
}
