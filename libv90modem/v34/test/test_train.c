/* Training signals of V.34 10.1.3: S, S-bar, PP, TRN and the J-type
 * (scrambled, differentially encoded) modulation used for Ja, CP, E, SCR. */
#include "test.h"
#include "testutil.h"

#define PT(p, x, y) do { v34_pt_t _p = (p); CHECK_EQ_I(_p.re, (x) * V34_ONE); CHECK_EQ_I(_p.im, (y) * V34_ONE); } while (0)

int main(void)
{
    unsigned i;

    /* S: alternates point 0 = (1,1) and point 0 turned 90 degrees
     * counterclockwise = (-1,1); ends on the turned point */
    for (i = 0; i < 128; i++) {
        if (i & 1) PT(v34_s_point(i, 128), -1, 1);
        else       PT(v34_s_point(i, 128), 1, 1);
    }
    PT(v34_s_point(126, 127), -1, 1);          /* odd length still ends turned */
    PT(v34_s_point(0, 127), -1, 1);
    /* S-bar: point 0 turned 180 = (-1,-1), then turned 270 ccw = (1,-1);
     * begins on the 180 degree point. S-bar is S negated. */
    for (i = 0; i < 16; i++) {
        if (i & 1) PT(v34_sbar_point(i), 1, -1);
        else       PT(v34_sbar_point(i), -1, -1);
    }

    /* PP (10-1): hand values for k = 0, 1, 2, then every value against the
     * formula, the 48-symbol period, and the property an equaliser training
     * sequence is chosen for: zero periodic autocorrelation */
    {
        static double pr[V34_PP_LEN], pi[V34_PP_LEN];
        static const double want[12][2] = {
            { 1, 0 }, { 1, 0 }, { 1, 0 }, { 1, 0 },                     /* k = 0 */
            { -0.5, 0.8660254 }, { -0.8660254, 0.5 }, { -1, 0 }, { -0.8660254, -0.5 },
                                              /* k = 1: exp(j pi (I + 4) / 6) */
            { 1, 0 }, { 0.5, 0.8660254 }, { -0.5, 0.8660254 }, { -1, 0 } };
                                              /* k = 2: exp(j pi 2I / 6) */
        for (i = 0; i < V34_PP_LEN; i++) {
            int16_t re, im;
            unsigned k = i / 4, I = i % 4;
            double ph = 3.14159265358979323846 * (k * I + (k % 3 == 1 ? 4 : 0)) / 6;
            v34_pp_point(i, &re, &im);
            pr[i] = re / 16384.0; pi[i] = im / 16384.0;
            CHECK_NEAR(pr[i], cos(ph), 1e-4);
            CHECK_NEAR(pi[i], sin(ph), 1e-4);
            if (i < 12) { CHECK_NEAR(pr[i], want[i][0], 1e-4); CHECK_NEAR(pi[i], want[i][1], 1e-4); }
            if (i >= 48) { CHECK_NEAR(pr[i], pr[i - 48], 1e-9); CHECK_NEAR(pi[i], pi[i - 48], 1e-9); }
        }
        for (unsigned lag = 1; lag < 48; lag++) {
            double ar = 0, ai = 0;
            for (i = 0; i < 48; i++) {
                unsigned j = (i + lag) % 48;
                ar += pr[i] * pr[j] + pi[i] * pi[j];
                ai += pi[i] * pr[j] - pr[i] * pi[j];
            }
            CHECK_NEAR(ar, 0, 1e-2);
            CHECK_NEAR(ai, 0, 1e-2);
        }
    }

    /* 4-point TRN, GPA. Scrambled ones from the zero state are
     * 11111 00000 11111 0... (see test_scr); pairs (I1, I2), I = 2*I2 + I1,
     * point 0 turned I * 90 degrees clockwise:
     * 11 -> 3 -> (-1,1); 11 -> (-1,1); 10 -> 1 -> (1,-1); 00 -> (1,1);
     * 00 -> (1,1); 11 -> (-1,1); 11 -> (-1,1); 10 -> (1,-1) */
    {
        v34_tmod_t t;
        static const signed char want[8][2] = {
            { -1, 1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }, { 1, 1 }, { -1, 1 }, { -1, 1 }, { 1, -1 } };
        v34_tmod_init(&t, V34_GPA, 4);
        for (i = 0; i < 8; i++) PT(v34_tmod_trn(&t), want[i][0], want[i][1]);
    }
    /* 16-point TRN, GPA: bits I1 I2 Q1 Q2.
     * 1111: I = 3, Q = 3: point 3 = (-3,-3) turned 270 cw = (3,-3)
     * 1000: I = 1, Q = 0: (1,1) turned 90 cw = (1,-1)
     * 0011: I = 0, Q = 3: (-3,-3)
     * 1110: I = 3, Q = 1: point 1 = (-3,1) turned 270 cw = (-1,-3) */
    {
        v34_tmod_t t;
        v34_tmod_init(&t, V34_GPA, 16);
        PT(v34_tmod_trn(&t), 3, -3);
        PT(v34_tmod_trn(&t), 1, -1);
        PT(v34_tmod_trn(&t), -3, -3);
        PT(v34_tmod_trn(&t), -1, -3);
    }
    /* J-type, 4 points: the 16-bit J pattern of Table 18 through GPA from
     * the zero state: in 0000 1001 1001 0001 -> scrambled 0000 1001 1101 1111
     * (out(n) = in(n) ^ out(n-5); n-23 not reached). Pairs give
     * I = 0 0 1 2 3 2 3 3, running sum mod 4 from Z = 0: Z = 0 0 1 3 2 0 3 2,
     * point 0 turned Z * 90 clockwise. */
    {
        v34_tmod_t t;
        static const char jbits[] = "0000100110010001";
        static const signed char want[8][2] = {
            { 1, 1 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 } };
        v34_tmod_init(&t, V34_GPA, 4);
        for (i = 0; i < 8; i++) {
            unsigned b = (unsigned)(jbits[2 * i] - '0') | (unsigned)(jbits[2 * i + 1] - '0') << 1;
            PT(v34_tmod_put(&t, b), want[i][0], want[i][1]);
        }
    }
    /* The differential encoder of a J-type sequence starts from the last
     * TRN symbol: an all-zero scrambler output would repeat that symbol.
     * Check through the demodulator instead: TRN then data, both sizes, with
     * a little noise; every bit must come back, including the first J
     * symbol. */
    for (int points = 4; points <= 16; points += 12)
        for (int poly = 0; poly < 2; poly++) {
            v34_tmod_t t;
            v34_tdem_t d;
            unsigned nb = points == 4 ? 2 : 4, errs = 0, trn_bad = 0, mask = (1u << nb) - 1;
            double e = 0;
            v34_tmod_init(&t, poly, points);
            v34_tdem_init(&d, poly, points);
            for (i = 0; i < 600; i++) {
                v34_pt_t s = v34_tmod_trn(&t);
                e += (double)s.re * s.re + (double)s.im * s.im;
                if (v34_tdem_trn(&d, tu_add_noise(s, 0.15)) != mask) trn_bad++;
            }
            CHECK_EQ_I(trn_bad, 0);
            CHECK_NEAR(e / 600 / (V34_ONE * V34_ONE), points == 4 ? 2.0 : 10.0, points == 4 ? 1e-9 : 0.8);
            for (i = 0; i < 2000; i++) {
                unsigned b = tu_rand() & mask;
                if (v34_tdem_put(&d, tu_add_noise(v34_tmod_put(&t, b), 0.15)) != b) errs++;
            }
            CHECK_EQ_I(errs, 0);
            /* E: 20 ones = 10 or 5 symbols */
            for (i = 0; i < 20 / nb; i++)
                CHECK_EQ_I(v34_tdem_put(&d, v34_tmod_put(&t, mask)), mask);
            /* a demodulator started in mid-stream is right after one symbol
             * (differential) plus 23 bits (descrambler) */
            {
                v34_tdem_t late;
                unsigned bad = 0;
                v34_tdem_init(&late, poly, points);
                for (i = 0; i < 500; i++) {
                    unsigned b = tu_rand() & mask;
                    v34_pt_t s = v34_tmod_put(&t, b);
                    unsigned g = v34_tdem_put(&late, s);
                    if (i > 24 / nb && g != b) bad++;
                    if (v34_tdem_put(&d, s) != b) bad++;
                }
                CHECK_EQ_I(bad, 0);
            }
            /* switching constellation size keeps scrambler and phase */
            {
                unsigned bad = 0;
                v34_tmod_set_points(&t, 20 - points);
                v34_tdem_set_points(&d, 20 - points);
                for (i = 0; i < 200; i++) {
                    unsigned m2 = (1u << (6 - nb)) - 1, b = tu_rand() & m2;
                    if (v34_tdem_put(&d, v34_tmod_put(&t, b)) != b) bad++;
                }
                CHECK_EQ_I(bad, 0);
            }
        }
    T_REPORT("test_train");
}
