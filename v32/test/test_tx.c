/* V.32bis transmitter: line level, spectrum (2.1, 2.2), the signals of 5.2
 * recovered by an ideal demodulator written here in double. */
#include <string.h>
#include "test.h"
#include "v32_priv.h"
#include "v32spec.h"

#define N 40000
static double sig[N];
#define PI 3.14159265358979323846

static int ones(void *u) { (void)u; return 1; }

static double rrc(double t, double a)
{
    if (fabs(t) < 1e-9) return 1 - a + 4 * a / PI;
    if (fabs(fabs(t) - 1 / (4 * a)) < 1e-9)
        return a / sqrt(2) * ((1 + 2 / PI) * sin(PI / (4 * a)) + (1 - 2 / PI) * cos(PI / (4 * a)));
    return (sin(PI * t * (1 - a)) + 4 * a * t * cos(PI * t * (1 + a))) / (PI * t * (1 - 16 * a * a * t * t));
}

/* symbol k as an ideal receiver sees it: matched filter at the pulse centre,
 * carrier cos / -sin from the transmitter's time origin (the line signal is
 * the whole thing V32_TXD symbols late, carrier included). Scaled to signal
 * space units for `level`. */
static void demod(int k, double level, double *x, double *y)
{
    double c = k * 10.0 / 3 + (V32_TXD + V32_TXL / 2) * 10.0 / 3, ar = 0, ai = 0, e = 0;
    int n, n0 = (int)c - 40, d0 = V32_TXD * 10 / 3;
    for (n = n0; n <= n0 + 80; n++) {
        double g = rrc((n - c) * 0.3, 0.25);
        if (n < 0 || n >= N) continue;
        ar += sig[n] * g * cos(2 * PI * 1800 / 8000 * (n - d0));
        ai -= sig[n] * g * sin(2 * PI * 1800 / 8000 * (n - d0));
        e += g * g;
    }
    /* line RMS `level` <-> mean |a|^2 = 40: a unit of signal space is level / sqrt(20) per rail */
    *x = 2 * ar / e / (level / sqrt(20.0));
    *y = 2 * ai / e / (level / sqrt(20.0));
}

static double power_at(double f, int n0, int n)
{
    double re = 0, im = 0; int i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * PI * i / n);
        re += sig[n0 + i] * w * cos(2 * PI * f * i / 8000);
        im += sig[n0 + i] * w * sin(2 * PI * f * i / 8000);
    }
    return re * re + im * im;
}

static void gen(v32tx_t *t, v32_txmode m, int rate, int level)
{
    int i;
    v32tx_init(t, 1, level, ones, 0);
    t->rate = rate;
    v32tx_set_mode(t, m);
    for (i = 0; i < N; i++) sig[i] = v32tx_tick(t);
}

int main(void)
{
    v32tx_t t;
    int i, k, rate;
    double x, y;
    int sx, sy;

    /* ---- level ---- */
    for (rate = 0; rate < V32_NRATES; rate++) {
        double pw = 0;
        gen(&t, V32_TX_ONES, rate, 3000);
        for (i = 1000; i < N; i++) pw += sig[i] * sig[i];
        printf("  %5d bit/s scrambled ones: line RMS %.0f for level 3000\n", V32_BITRATE(rate), sqrt(pw / (N - 1000)));
        CHECK_NEAR(20 * log10(sqrt(pw / (N - 1000)) / 3000), 0.0, 0.35);
    }

    /* ---- 2.2: with scrambled ones, the density at 600 and 3000 Hz is
     * 4.5 +/- 2.5 dB below the maximum between them ---- */
    for (rate = 0; rate < V32_NRATES; rate++) {
        double pmax = 0, p600 = 0, p3000 = 0, p200 = 0, p3500 = 0, f;
        gen(&t, V32_TX_ONES, rate, 3000);
        for (f = 200; f <= 3600; f += 50) {
            double p = 0; int b;
            for (b = 0; b < 150; b++) p += power_at(f, 1000 + b * 256, 512);
            if (f >= 600 && f <= 3000 && p > pmax) pmax = p;
            if (f == 600) p600 = p;
            if (f == 3000) p3000 = p;
            if (f == 200) p200 = p;
            if (f == 3500) p3500 = p;
        }
        printf("  %5d bit/s: 600 Hz %.1f dB, 3000 Hz %.1f dB, 200 Hz %.1f dB, 3500 Hz %.1f dB re maximum\n", V32_BITRATE(rate),
               10 * log10(p600 / pmax), 10 * log10(p3000 / pmax), 10 * log10(p200 / pmax), 10 * log10(p3500 / pmax));
        CHECK_NEAR(10 * log10(p600 / pmax), -4.5, 2.5);
        CHECK_NEAR(10 * log10(p3000 / pmax), -4.5, 2.5);
        CHECK(10 * log10(p200 / pmax) < -35);
        CHECK(10 * log10(p3500 / pmax) < -35);
    }

    /* ---- 2.1: carrier 1800 Hz (AA is a pure tone there), AC is 600 + 3000 Hz ---- */
    gen(&t, V32_TX_AA, 0, 3000);
    { double p = power_at(1800, 4000, 32000), q = power_at(1801, 4000, 32000), q2 = power_at(1799, 4000, 32000), tot = 0;
      for (i = 4000; i < 36000; i++) tot += sig[i] * sig[i];
      CHECK(p > q * 1.5 && p > q2 * 1.5);                 /* within a fraction of a hertz */
      CHECK(power_at(600, 4000, 32000) < p * 1e-5);
      CHECK(power_at(3000, 4000, 32000) < p * 1e-5);
      (void)tot; }
    gen(&t, V32_TX_AC, 0, 3000);
    { double p6 = power_at(600, 4000, 32000), p30 = power_at(3000, 4000, 32000), p18 = power_at(1800, 4000, 32000);
      CHECK(p18 < p6 * 1e-5); CHECK(p18 < p30 * 1e-5);
      CHECK_NEAR(10 * log10(p6 / p30), 0.0, 0.5); }

    /* ---- the signal states at the line ---- */
    gen(&t, V32_TX_S, 0, 3000);
    for (k = 20; k < 60; k++) {
        demod(k, 3000, &x, &y); v32_state_point(k & 1, &sx, &sy);
        CHECK_NEAR(x, sx, 0.25); CHECK_NEAR(y, sy, 0.25);
    }
    gen(&t, V32_TX_SBAR, 0, 3000);
    for (k = 20; k < 60; k++) {
        demod(k, 3000, &x, &y); v32_state_point(2 + (k & 1), &sx, &sy);
        CHECK_NEAR(x, sx, 0.25); CHECK_NEAR(y, sy, 0.25);
    }
    /* TRN, answer mode: 5.2.3's states, then Table 4 on the scrambler's dibits */
    gen(&t, V32_TX_TRN, 0, 3000);
    { v32_scr_t s = { 0 };
      for (k = 0; k < 600; k++) {
          int d1 = v32_scramble(&s, 1, 1), d2 = v32_scramble(&s, 1, 1), st;
          if (k < 256) st = d1 ? 2 : 0;
          else st = (d1 << 1 | d2) == 0 ? 0 : (d1 << 1 | d2) == 1 ? 1 : (d1 << 1 | d2) == 3 ? 2 : 3;
          if (k < (int)strlen(V32SPEC_GPA_TRN)) CHECK_EQ_I("ABCD"[st], V32SPEC_GPA_TRN[k]);
          demod(k, 3000, &x, &y); v32_state_point(st, &sx, &sy);
          CHECK_NEAR(x, sx, 0.3); CHECK_NEAR(y, sy, 0.3);
      } }
    /* call mode scrambler gives the other sequence */
    v32tx_init(&t, 0, 3000, ones, 0); v32tx_set_mode(&t, V32_TX_TRN);
    for (i = 0; i < N; i++) sig[i] = v32tx_tick(&t);
    for (k = 0; k < (int)strlen(V32SPEC_GPC_TRN); k++) {
        demod(k, 3000, &x, &y); v32_state_point(V32SPEC_GPC_TRN[k] - 'A', &sx, &sy);
        CHECK_NEAR(x, sx, 0.3); CHECK_NEAR(y, sy, 0.3);
    }
    /* a rate sequence: scrambled, Table 2, continuing from the last TRN state */
    v32tx_init(&t, 1, 3000, ones, 0); v32tx_set_mode(&t, V32_TX_TRN);
    t.rword = v32_rate_word(V32_RALL);
    for (i = 0; i < N; i++) {
        if (v32tx_symbol_due(&t) && t.mode == V32_TX_TRN && t.nsym == 300) v32tx_set_mode(&t, V32_TX_RATE);
        sig[i] = v32tx_tick(&t);
    }
    { v32_scr_t s = { 0 }; int st = 0, q;
      static const char *t2[4] = { "+90", "0", "+180", "+270" };
      (void)t2;
      for (k = 0; k < 400; k++) {
          int b1, b2;
          if (k < 300) { b1 = v32_scramble(&s, 1, 1); b2 = v32_scramble(&s, 1, 1);
              q = b1 << 1 | b2; st = k < 256 ? (b1 ? 2 : 0) : (q == 0 ? 0 : q == 1 ? 1 : q == 3 ? 2 : 3); }
          else {
              int bit = ((k - 300) * 2) & 15; uint16_t w = v32_rate_word(V32_RALL);
              b1 = v32_scramble(&s, 1, (w >> (15 - bit)) & 1); b2 = v32_scramble(&s, 1, (w >> (14 - bit)) & 1);
              q = b1 << 1 | b2;
              st = (st + (q == 0 ? 1 : q == 1 ? 0 : q == 2 ? 2 : 3)) & 3;      /* Table 2 quadrant change */
          }
          demod(k, 3000, &x, &y); v32_state_point(st, &sx, &sy);
          CHECK_NEAR(x, sx, 0.3); CHECK_NEAR(y, sy, 0.3);
      } }
    /* data at 14400: the encoder's points */
    gen(&t, V32_TX_ONES, 4, 3000);
    { v32_scr_t s = { 0 }; v32_enc_t e = { 0, 0 }; int px, py, j, q; double worst = 0;
      for (k = 0; k < 2000; k++) {
          for (q = 0, j = 0; j < 6; j++) q = q << 1 | v32_scramble(&s, 1, 1);
          v32_encode(&e, 4, q, &px, &py);
          demod(k, 3000, &x, &y);
          if (fabs(x - px) > worst) worst = fabs(x - px);
          if (fabs(y - py) > worst) worst = fabs(y - py);
      }
      printf("  14400 bit/s: worst deviation of a demodulated point %.3f units (points are 1.41 apart)\n", worst);
      CHECK(worst < 0.3); }
    T_REPORT("test_tx");
}
