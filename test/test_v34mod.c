/* modem/v34mod: transmit power, power reduction, the pre-emphasis templates
 * of V.34 5.4 (Figures 1 and 2, Tables 3 and 4: +-1 dB over the carrier
 * +-0.45 S), symbol timing, and demodulation by the receiver core at 2400,
 * 3000 and 3200 symbols/s. */
#include <string.h>
#include "test.h"
#include "../modem/v34mod.h"
#include "../modem/qamrx.h"
#include "../modem/v34train.h"
#include "../modem/p2ans.h"

#define PI 3.14159265358979323846
static const int RATE[3] = { 2400, 3000, 3200 }, IDX[3] = { 0, 3, 4 };

static double tmpl_db(int idx, double x)
{
    static const double alpha[6] = { 0, 2, 4, 6, 8, 10 };              /* Table 3 */
    static const double beta[5] = { 0.5, 1.0, 1.5, 2.0, 2.5 }, gamma[5] = { 1, 2, 3, 4, 5 };   /* Table 4 */
    if (idx <= 5) return alpha[idx] * x;
    if (x <= 0.4) return 0;
    if (x <= 0.8) return beta[idx - 6] * (x - 0.4) / 0.4;
    return beta[idx - 6] + gamma[idx - 6] * (x - 0.8) / 0.4;
}

static uint32_t lcg = 12345;
static int rnd4(void) { lcg = lcg * 1664525u + 1013904223u; return (int)(lcg >> 30); }

static double run_rms(int S, int num, int den, int pre, int level, int red)
{
    v34mod_t m; double pw = 0; int n;
    CHECK_EQ_I(v34mod_init(&m, S, num, den, pre, level, red), 0);
    for (n = 0; n < 24000; n++) {
        int16_t y;
        if (v34mod_due(&m)) { int32_t re = QRX_REF, im = QRX_REF; v34_rot4(rnd4(), &re, &im); v34mod_put(&m, re, im); }
        y = v34mod_tick(&m);
        if (n >= 4000) pw += (double)y * y;
    }
    return sqrt(pw / 20000);
}

int main(void)
{
    int r, c, pre, n;

    /* ---- power, power reduction ---- */
    for (r = 0; r < 3; r++) for (c = 0; c < 2; c++) {
        int num, den; double worst = 0;
        v34_carrier(IDX[r], c, &num, &den);
        for (pre = 0; pre <= 10; pre++) {
            double rms = run_rms(RATE[r], num, den, pre, 4000, 0), db = 20 * log10(rms / 4000);
            if (fabs(db) > fabs(worst)) worst = db;
            CHECK_NEAR(db, 0.0, 0.25);
        }
        printf("  %d symbols/s, carrier %d/%d: power within %+.2f dB of nominal over the 11 filters\n", RATE[r], num, den, worst);
    }
    for (n = 1; n <= 7; n++) {
        double a = run_rms(3200, 3, 5, 0, 4000, 0), b = run_rms(3200, 3, 5, 3, 4000, n);
        CHECK_NEAR(20 * log10(b / a), -n, 0.2);
    }
    CHECK_NEAR(20 * log10(run_rms(3000, 3, 5, 0, 1000, 0) / 1000), 0.0, 0.25);

    /* ---- spectrum: one symbol, the transform of what comes out ---- */
    {
        double worst_all = 0;
        for (r = 0; r < 3; r++) for (c = 0; c < 2; c++) for (pre = 0; pre <= 10; pre++) {
            static double y[1024];
            v34mod_t m; int num, den, k; double dev[19], mean = 0, worst = 0;
            v34_carrier(IDX[r], c, &num, &den);
            (void)v34mod_init(&m, RATE[r], num, den, pre, 8000, 0);
            for (n = 0; n < 1024; n++) {
                if (v34mod_due(&m)) v34mod_put(&m, m.nsym == 20 ? 8000 : 0, 0);
                y[n] = v34mod_tick(&m);
            }
            for (k = 0; k < 19; k++) {
                double x = (double)num / den - 0.45 + 0.05 * k, f = x * RATE[r], re = 0, im = 0;
                for (n = 0; n < 1024; n++) { re += y[n] * cos(2 * PI * f * n / 8000); im += y[n] * sin(2 * PI * f * n / 8000); }
                dev[k] = 10 * log10(re * re + im * im) - tmpl_db(pre, x);
                mean += dev[k] / 19;
            }
            for (k = 0; k < 19; k++) if (fabs(dev[k] - mean) > worst) worst = fabs(dev[k] - mean);
            CHECK(worst < 0.5);         /* the Recommendation allows 1 dB */
            if (worst > worst_all) worst_all = worst;
        }
        printf("  pre-emphasis templates: worst deviation %.2f dB over 3 symbol rates x 2 carriers x 11 filters\n", worst_all);
    }
    {   /* the templates themselves at their defining points (Tables 3 and 4) */
        CHECK_NEAR(tmpl_db(5, 1.0), 10.0, 1e-9); CHECK_NEAR(tmpl_db(1, 1.0), 2.0, 1e-9);
        CHECK_NEAR(tmpl_db(6, 0.8), 0.5, 1e-9); CHECK_NEAR(tmpl_db(10, 1.2), 7.5, 1e-9);
    }

    /* ---- symbol clock: 128 symbols take 128 * 8000 / S samples, exactly ---- */
    for (r = 0; r < 3; r++) {
        v34mod_t m; int num, den, cnt = 0;
        v34_carrier(IDX[r], 0, &num, &den);
        (void)v34mod_init(&m, RATE[r], num, den, 0, 4000, 0);
        for (n = 0; n < 128 * 8000 / RATE[r] * 3; n++) { if (v34mod_due(&m)) { v34mod_put(&m, 0, 0); cnt++; } (void)v34mod_tick(&m); }
        CHECK_EQ_I(cnt, 128 * 3);
    }
    CHECK_EQ_I(v34mod_init(&(v34mod_t){0}, 2743, 3, 5, 0, 4000, 0), -1);
    CHECK_EQ_I(v34mod_init(&(v34mod_t){0}, 3200, 3, 5, 11, 4000, 0), -1);

    /* ---- through the receiver core: PP, then the 4-point TRN ---- */
    for (r = 0; r < 3; r++) for (c = 0; c < 2; c++) for (pre = 0; pre <= 10; pre += 5) {
        v34mod_t m; qamrx_t rx; v34trn_t tt, tr;
        int num, den, started = 0; int32_t first = -1;
        v34_carrier(IDX[r], c, &num, &den);
        (void)v34mod_init(&m, RATE[r], num, den, pre, 4000, 0);
        qamrx_init(&rx, RATE[r], num, den);
        v34trn_init(&tt, V34T_GPC); v34trn_init(&tr, V34T_GPC);
        for (n = 0; n < 16000; n++) {
            int16_t y;
            if (v34mod_due(&m)) {
                int32_t re, im; int i = m.nsym - 256;
                if (i < 0) v34_s(m.nsym, 256, &re, &im);            /* something for the AGC */
                else if (i < V34_PP_LEN) v34_pp(i, &re, &im);
                else v34trn_next(&tt, &re, &im);
                if (i == 0) first = n;
                v34mod_put(&m, re, im);
            }
            y = v34mod_tick(&m);
            if (first >= 0 && !started && n == first) {
                /* PP(0) was taken by this tick: its centre is delay samples after it */
                (void)qamrx_tick(&rx, y);
                qamrx_sync(&rx, v34mod_delay_q8(&m) - 256);
                started = 1;
                continue;
            }
            if (qamrx_tick(&rx, y)) {
                int32_t dr, di; int i = rx.nsym - 1;
                if (!started) { qamrx_update(&rx, rx.zr, rx.zi, 0, 12); continue; }
                if (i < V34_PP_LEN) v34_pp(i, &dr, &di); else v34trn_next(&tr, &dr, &di);
                qamrx_update(&rx, dr, di, QRX_EQ | QRX_CARRIER | (i > 300 ? QRX_TIMING : 0), i < 600 ? 10 : 11);
            }
        }
        {
            double db = 10 * log10((rx.mse + 1) / 65536.0);
            if (pre == 0) printf("  %d symbols/s, carrier %d/%d: receiver error %.1f dB after training\n", RATE[r], num, den, db);
            CHECK(db < -35.0);
        }
    }
    T_REPORT("v34mod");
}
