/* V.92 training signal mappings (v92_sig).
 *
 * Expected sequences are worked out by hand from the scrambler equations of
 * clause 7/V.34 and from 8.5 to 8.7 of V.92. Longer runs are compared with
 * a model kept in this file that works on whole arrays straight from the
 * definitions. */
#include <math.h>
#include <string.h>
#include "test.h"
#include "testutil.h"
#include "v92_sig.h"
#include "v92_seq.h"

/* out[n] = in[n] ^ out[n - a] ^ out[n - 23], history zero before n = 0 */
static void model_scramble(const uint8_t *in, uint8_t *out, int n, int a)
{
    int i;
    for (i = 0; i < n; i++)
        out[i] = (uint8_t)(in[i] ^ (i >= a ? out[i - a] : 0) ^ (i >= 23 ? out[i - 23] : 0));
}

static int match(const uint8_t *b, const char *s)
{
    int i, bad = 0;
    for (i = 0; s[i]; i++)
        if (b[i] != (uint8_t)(s[i] - '0')) bad++;
    return bad;
}

/* ---- two-point signals ----------------------------------------------------- */

static void test_trn1u(void)
{
    v92_sgn_t tx, rx;
    uint8_t line[600], ones[600], want[600];
    int i, bad;

    /* GPA, y(n) = 1 ^ y(n-5) ^ y(n-23) from the zero state:
     *   n = 0..4    1 (nothing fed back yet)
     *   n = 5..9    1 ^ 1 = 0
     *   n = 10..14  1 ^ 0 = 1
     *   n = 15..19  0
     *   n = 20..22  1
     *   n = 23      1 ^ y(18) ^ y(0) = 1 ^ 0 ^ 1 = 0
     *   n = 24      1 ^ y(19) ^ y(1) = 0
     *   n = 25..27  1 ^ 1 ^ 1 = 1
     *   n = 28, 29  1 ^ y(23), y(24) ^ y(5), y(6) = 1 ^ 0 ^ 0 = 1
     *   n = 30..32  1 ^ 1 ^ 0 = 0
     *   n = 33, 34  1 ^ 1 ^ 1 = 1
     *   n = 35      1 ^ y(30) ^ y(12) = 1 ^ 0 ^ 1 = 0
     * A scrambler output of 1 is a negative voltage (8.5.7): TRN1u starts
     * with five symbols of -LU. */
    v92_sgn_init(&tx, 0);
    for (i = 0; i < 600; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    CHECK_EQ_I(match(line, "111110000011111000001110011111000110"), 0);
    CHECK_EQ_I(V92_UP_SIGN(line[0]), -1);
    CHECK_EQ_I(V92_UP_SIGN(line[5]), 1);
    memset(ones, 1, sizeof ones);
    model_scramble(ones, want, 600, 5);
    CHECK_EQ_I(memcmp(line, want, 600), 0);

    /* the receiver sees ones from the 24th symbol on; zeros if the line
     * inverts */
    v92_sgn_init(&rx, 0);
    for (i = 0, bad = 0; i < 600; i++) {
        int d = v92_sgn_rx_trn(&rx, line[i]);
        if (i >= 23 && d != 1) bad++;
    }
    CHECK_EQ_I(bad, 0);
    v92_sgn_init(&rx, 0);
    for (i = 0, bad = 0; i < 600; i++) {
        int d = v92_sgn_rx_trn(&rx, !line[i]);
        if (i >= 23 && d != 0) bad++;
    }
    CHECK_EQ_I(bad, 0);

    /* a second TRN1u segment starts from a zeroed scrambler again */
    v92_sgn_scr_reset(&tx);
    for (i = 0; i < 36; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    CHECK_EQ_I(match(line, "111110000011111000001110011111000110"), 0);
}

static void test_ja_on_line(void)
{
    static v92_ja_t ja, out;
    static uint8_t seq[V92_JA_MAX_BITS], src[4000], scr[4000], line[4000], got[4000];
    v92_sgn_t tx, rx;
    int n, i, k, pos, at;

    /* 24 symbols of TRN1u (line bits 11111 00000 11111 00000 1110, the last
     * one 0), then Ja. Ja opens with 24 ones; the scrambler runs on:
     * y(24..35) = 0 1 1 1 1 1 0 0 0 1 1 0 (see test_trn1u). Differential
     * encoding from the last TRN1u symbol (0):
     *   0, 0^1 = 1, 1^1 = 0, 0^1 = 1, 1^1 = 0, 0^1 = 1, 1^0 = 1, 1, 1,
     *   1^1 = 0, 0^1 = 1, 1^0 = 1 */
    v92_sgn_init(&tx, 0);
    for (i = 0; i < 24; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    CHECK_EQ_I(line[23], 0);
    for (i = 24; i < 36; i++) line[i] = (uint8_t)v92_sgn_tx_bit(&tx, 1);
    CHECK_EQ_I(match(line + 24, "010101111011"), 0);

    /* The whole thing against the array model: TRN1u (240 symbols), 24
     * ones, two DIL descriptors, then E1u-like 12 zeros. */
    memset(&ja, 0, sizeof ja);
    ja.dil.n = 2; ja.dil.lsp = 5; ja.dil.ltp = 1; ja.dil.sp[0] = 0x15; ja.dil.tp[0] = 1;
    ja.dil.train[0] = 77; ja.dil.train[1] = 90;
    ja.rates = 0x7FFFF;
    n = v92_ja_build(&ja, seq);
    CHECK(n > 0 && n % 12 == 0);
    pos = 0;
    for (i = 0; i < 240 + 24; i++) src[pos++] = 1;
    for (k = 0; k < 2; k++) { memcpy(src + pos, seq, (size_t)n); pos += n; }
    for (i = 0; i < 12; i++) src[pos++] = 0;
    model_scramble(src, scr, pos, 5);
    v92_sgn_init(&tx, 0);
    for (i = 0; i < 240; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    for (; i < pos; i++) line[i] = (uint8_t)v92_sgn_tx_bit(&tx, src[i]);
    for (i = 0, k = 0; i < pos; i++) {
        /* TRN1u: the scrambled bit itself; afterwards XOR the previous line bit */
        int want = i < 240 ? scr[i] : scr[i] ^ line[i - 1];
        if (line[i] != want) k++;
    }
    CHECK_EQ_I(k, 0);

    /* receiver: descramble TRN1u, then differential decoding as well:
     * every bit of Ja comes out, from the first of the 24 ones */
    v92_sgn_init(&rx, 0);
    for (i = 0; i < 240; i++) got[i] = (uint8_t)v92_sgn_rx_trn(&rx, line[i]);
    for (; i < pos; i++) got[i] = (uint8_t)v92_sgn_rx_bit(&rx, line[i]);
    CHECK_EQ_I(memcmp(got + 240, src + 240, (size_t)(pos - 240)), 0);
    /* The same on an inverted line. The descrambler still holds inverted
     * TRN1u symbols when Ja starts, so up to 23 bits are wrong: the 24 ones
     * that open Ja are there to be lost. The descriptors are intact. */
    v92_sgn_init(&rx, 0);
    for (i = 0; i < 240; i++) got[i] = (uint8_t)v92_sgn_rx_trn(&rx, !line[i]);
    for (; i < pos; i++) got[i] = (uint8_t)v92_sgn_rx_bit(&rx, !line[i]);
    CHECK_EQ_I(memcmp(got + 263, src + 263, (size_t)(pos - 263)), 0);
    at = v92_find_sync(got, pos, 240);
    CHECK_EQ_I(at, 240 + 24);
    CHECK_EQ_I(v92_ja_parse(got + at, pos - at, &out), n);
    CHECK_EQ_I(memcmp(&ja, &out, sizeof ja), 0);
    at = v92_find_sync(got, pos, at + n);
    CHECK_EQ_I(at, 240 + 24 + n);
    CHECK_EQ_I(v92_ja_parse(got + at, pos - at, &out), n);
}

static void test_downstream(void)
{
    v92_sgn_t tx, rx;
    v92_jp_t jp, jpo;
    uint8_t seq[V92_J_BITS], src[400], scr[400], line[400], got[400];
    int i, pos, bad;

    /* GPC, y(n) = 1 ^ y(n-18) ^ y(n-23) from the zero state: 18 ones,
     * 5 zeros (1 ^ 1), 13 ones (n = 23..35: 1 ^ 1 ^ 1), then zeros
     * (n = 36: 1 ^ y(18) ^ y(13) = 1 ^ 0 ^ 1). SCR sends this as the sign,
     * 1 = positive (8.6.6). */
    v92_sgn_init(&tx, 1);
    for (i = 0; i < 40; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    CHECK_EQ_I(match(line, "1111111111111111110000011111111111110000"), 0);
    CHECK_EQ_I(V92_DN_SIGN(line[0]), 1);
    CHECK_EQ_I(V92_DN_SIGN(line[18]), -1);

    /* Jp (twice) and Jp' after 60 symbols of scrambled ones, against the
     * array model */
    jp.frac = 0x1234; jp.pt8_train = 1; jp.pt8_reneg = 1;
    CHECK_EQ_I(v92_jp_build(&jp, seq), 72);
    pos = 0;
    for (i = 0; i < 60; i++) src[pos++] = 1;
    memcpy(src + pos, seq, 72); pos += 72;
    memcpy(src + pos, seq, 72); pos += 72;
    for (i = 0; i < V92_JPP_BITS; i++) src[pos++] = 0;
    model_scramble(src, scr, pos, 18);
    v92_sgn_init(&tx, 1);
    for (i = 0; i < 60; i++) line[i] = (uint8_t)v92_sgn_tx_trn(&tx);
    for (; i < pos; i++) line[i] = (uint8_t)v92_sgn_tx_bit(&tx, src[i]);
    for (i = 0, bad = 0; i < pos; i++)
        if (line[i] != (i < 60 ? scr[i] : scr[i] ^ line[i - 1])) bad++;
    CHECK_EQ_I(bad, 0);
    v92_sgn_init(&rx, 1);
    for (i = 0; i < 60; i++) got[i] = (uint8_t)v92_sgn_rx_trn(&rx, line[i]);
    for (; i < pos; i++) got[i] = (uint8_t)v92_sgn_rx_bit(&rx, line[i]);
    CHECK_EQ_I(memcmp(got + 60, src + 60, (size_t)(pos - 60)), 0);
    CHECK_EQ_I(v92_find_sync(got, pos, 60), 60);
    CHECK_EQ_I(v92_jp_parse(got + 60, pos - 60, &jpo), 72);
    CHECK_EQ_I(jpo.frac, 0x1234);
    CHECK_EQ_I(v92_jp_parse(got + 132, pos - 132, &jpo), 72);
    for (i = 0, bad = 0; i < V92_JPP_BITS; i++) bad += got[204 + i];
    CHECK_EQ_I(bad, 0);                          /* Jp': 12 zeros */
}

/* ---- Ru, Su ---------------------------------------------------------------- */

static void test_ru_su(void)
{
    static const int ru[6] = { 1, 1, 1, -1, -1, -1 };
    static const int su[6] = { 1, 0, 1, -1, 0, -1 };
    int n, lu = 10000;
    double a = sqrt(1.5) * lu, ms = 0;

    for (n = 0; n < 18; n++) {
        CHECK_EQ_I(V92_UP_SIGN(v92_ru_sbit((unsigned)n, 0)), ru[n % 6]);
        CHECK_EQ_I(V92_UP_SIGN(v92_ru_sbit((unsigned)n, 1)), -ru[n % 6]);
        CHECK_NEAR(v92_su_level((unsigned)n, 0, lu), su[n % 6] * a, 0.51);
        CHECK_NEAR(v92_su_level((unsigned)n, 1, lu), -su[n % 6] * a, 0.51);
    }
    /* sqrt(3/2) * 10000 = 12247.4 */
    CHECK_EQ_I(v92_su_level(0, 0, lu), 12247);
    CHECK_EQ_I(v92_su_level(1, 0, lu), 0);
    CHECK_EQ_I(v92_su_level(3, 0, lu), -12247);
    CHECK_EQ_I(v92_su_level(3, 1, lu), 12247);
    /* same power as TRN1u */
    for (n = 0; n < 6; n++) ms += (double)v92_su_level((unsigned)n, 0, lu) * v92_su_level((unsigned)n, 0, lu);
    CHECK_NEAR(ms / 6, (double)lu * lu, 1e-4 * lu * lu);
    CHECK_EQ_I(v92_su_level(0, 0, 32124), 39344);        /* 32124 * 1.2247449 = 39343.7 */
}

/* ---- TRN2u ----------------------------------------------------------------- */

static void test_trn2u(void)
{
    static const int want4[12] = { -3, 3, 3, 1, 1, -3, 3, 3, 1, 1, -3, -3 };
    static const int want4m[6] = { -3, 3, -1, -1, -1, 3 };
    static const int want8[8] = { -7, -7, -1, 5, -7, -1, 1, 7 };
    v92_ml_t tx, rx;
    int i, k, bad, lu = 16384;
    double ms;

    /* 4-point, scrambler output 1 1 1 1 1 0 0 0 0 0 1 1 1 1 1 0 0 0 0 0
     * 1 1 1 0 taken two at a time, the first bit as the LSB (magnitude:
     * 0 = 1, 1 = 3), the second as the MSB (sign, differentially encoded
     * starting from a previous sign bit 0):
     *   (1,1) sign 1^0 = 1: -3     (1,1) sign 1^1 = 0: +3
     *   (1,0) sign 0^0 = 0: +3     (0,0) +1    (0,0) +1
     *   (1,1) sign 1^0 = 1: -3     (1,1) sign 1^1 = 0: +3
     *   (1,0) +3   (0,0) +1   (0,0) +1
     *   (1,1) sign 1^0 = 1: -3     (1,0) sign 0^1 = 1: -3 */
    v92_ml_init(&tx, 0, 0);
    for (i = 0; i < 12; i++) CHECK_EQ_I(v92_ml_tx(&tx, NULL), want4[i]);
    /* the previous sign bit 1 (E1u ended negative) flips every sign */
    v92_ml_init(&tx, 0, 1);
    for (i = 0; i < 12; i++) CHECK_EQ_I(v92_ml_tx(&tx, NULL), -want4[i]);
    /* the other reading: first bit = MSB (sign), second = magnitude:
     *   (1,1) sign 1: -3   (1,1) sign 1^1 = 0: +3   (1,0) sign 1^0 = 1: -1
     *   (0,0) sign 0^1 = 1: -1   (0,0) -1   (1,1) sign 1^1 = 0: +3 */
    v92_ml_init(&tx, 0, 0);
    v92_ml_set_msb_first(&tx, 1);
    for (i = 0; i < 6; i++) CHECK_EQ_I(v92_ml_tx(&tx, NULL), want4m[i]);

    /* 8-point, three at a time, first bit = LSB, third = sign; magnitude
     * index m = b0 + 2 * b1, level 2 * m + 1:
     *   (1,1,1) m = 3, sign 1^0 = 1: -7    (1,1,0) m = 3, sign 0^1 = 1: -7
     *   (0,0,0) m = 0, sign 0^1 = 1: -1    (0,1,1) m = 2, sign 1^1 = 0: +5
     *   (1,1,1) m = 3, sign 1^0 = 1: -7    (0,0,0) sign 0^1 = 1: -1
     *   (0,0,1) m = 0, sign 1^1 = 0: +1    (1,1,0) m = 3, sign 0: +7 */
    v92_ml_init(&tx, 1, 0);
    for (i = 0; i < 8; i++) CHECK_EQ_I(v92_ml_tx(&tx, NULL), want8[i]);

    /* levels: 1/sqrt(5) and 3/sqrt(5) of LU, 1 .. 7 / sqrt(21) of LU */
    CHECK_NEAR(v92_trn2u_level(1, 0, lu), lu / sqrt(5.0), 0.51);
    CHECK_NEAR(v92_trn2u_level(-3, 0, lu), -3 * lu / sqrt(5.0), 0.51);
    for (k = 1; k <= 7; k += 2) {
        CHECK_NEAR(v92_trn2u_level(k, 1, lu), k * lu / sqrt(21.0), 0.51);
        CHECK_NEAR(v92_trn2u_level(-k, 1, lu), -k * lu / sqrt(21.0), 0.51);
    }
    CHECK_EQ_I(v92_trn2u_level(1, 0, 10000), 4472);      /* 4472.1 */
    CHECK_EQ_I(v92_trn2u_level(3, 0, 10000), 13416);     /* 13416.4 */
    CHECK_EQ_I(v92_trn2u_level(7, 1, 10000), 15275);     /* 15275.3 */
    /* equally likely levels have the power of TRN1u */
    ms = 0;
    for (k = 1; k <= 3; k += 2) ms += (double)v92_trn2u_level(k, 0, lu) * v92_trn2u_level(k, 0, lu);
    CHECK_NEAR(ms / 2, (double)lu * lu, 1e-3 * lu * lu);
    ms = 0;
    for (k = 1; k <= 7; k += 2) ms += (double)v92_trn2u_level(k, 1, lu) * v92_trn2u_level(k, 1, lu);
    CHECK_NEAR(ms / 4, (double)lu * lu, 1e-3 * lu * lu);

    /* slicer: thresholds half way between levels, saturating outside */
    for (k = -3; k <= 3; k += 2) {
        int v = v92_trn2u_level(k, 0, lu);
        CHECK_EQ_I(v92_trn2u_slice(v, 0, lu), k);
        CHECK_EQ_I(v92_trn2u_slice(v + lu / 3, 0, lu), k);       /* spacing is 0.894 LU */
        CHECK_EQ_I(v92_trn2u_slice(v - lu / 3, 0, lu), k);
    }
    CHECK_EQ_I(v92_trn2u_slice(5 * lu, 0, lu), 3);
    CHECK_EQ_I(v92_trn2u_slice(-5 * lu, 0, lu), -3);
    CHECK_EQ_I(v92_trn2u_slice(0, 0, lu), 1);
    CHECK_EQ_I(v92_trn2u_slice(-1, 0, lu), -1);
    for (k = -7; k <= 7; k += 2) {
        int v = v92_trn2u_level(k, 1, lu);
        CHECK_EQ_I(v92_trn2u_slice(v, 1, lu), k);
        CHECK_EQ_I(v92_trn2u_slice(v + lu / 6, 1, lu), k);       /* spacing is 0.436 LU */
        CHECK_EQ_I(v92_trn2u_slice(v - lu / 6, 1, lu), k);
    }
    CHECK_EQ_I(v92_trn2u_slice(5 * lu, 1, lu), 7);
    CHECK_EQ_I(v92_trn2u_slice(-5 * lu, 1, lu), -7);

    /* TRN2u, then an SUVu and a CPu' as bits, then E2u (a data frame of
     * zeros), through modulator, an inverted line and demodulator; both
     * constellation sizes and both bit orders */
    for (k = 0; k < 4; k++) {
        static v92_cpu_t cp, cpo;
        static uint8_t src[4000], got[4000];
        v92_suvu_t su = { 1, -3, 0, 0 }, suo;
        int pt8 = k & 1, nb = pt8 ? 3 : 2, unit = 12 * nb, pos = 0, n, at, sym;
        memset(&cp, 0, sizeof cp);
        cp.type = V92_UP_CPU; cp.drn = 15; cp.ack = 1; cp.idx[3] = 1;
        cp.mask[0][3] = 0x81; cp.mask[1][9] = 0x18;
        for (i = 0; i < 10 * unit; i++) src[pos++] = 1;          /* 120 symbols of TRN2u */
        n = v92_suvu_build(&su, unit, src + pos);
        CHECK_EQ_I(n % unit, 0);
        pos += n;
        n = v92_cpu_build(&cp, unit, src + pos);
        CHECK_EQ_I(n % unit, 0);
        pos += n;
        for (i = 0; i < unit; i++) src[pos++] = 0;               /* E2u */
        CHECK_EQ_I(pos % unit, 0);

        v92_ml_init(&tx, pt8, 1);
        v92_ml_init(&rx, pt8, 0);                /* the line is inverted: the symbol
                                                    before TRN2u arrived positive */
        v92_ml_set_msb_first(&tx, k >> 1);
        v92_ml_set_msb_first(&rx, k >> 1);
        for (sym = 0, bad = 0; sym < pos / nb; sym++) {
            int lev = v92_ml_tx(&tx, sym < 120 ? NULL : src + sym * nb);
            int32_t v = -v92_trn2u_level(lev, pt8, lu);          /* inverted line */
            if (lev < -(2 * (1 << (nb - 1)) - 1) || lev > 2 * (1 << (nb - 1)) - 1 || !(lev & 1)) bad++;
            v92_ml_rx(&rx, v92_trn2u_slice(v + (sym % 3 - 1) * lu / 8, pt8, lu), got + sym * nb);
        }
        CHECK_EQ_I(bad, 0);
        CHECK_EQ_I(memcmp(got + 24, src + 24, (size_t)(pos - 24)), 0);
        at = v92_find_sync(got, pos, 24);
        CHECK_EQ_I(at, 10 * unit);
        CHECK_EQ_I(v92_up_kind(got + at, pos - at), V92_UP_SUVU);
        n = v92_suvu_parse(got + at, pos - at, unit, &suo);
        CHECK(n > 0 && n % unit == 0);
        CHECK_EQ_I(suo.level, -3);
        CHECK_EQ_I(suo.wait_cpu, 1);
        at += n;
        CHECK_EQ_I(v92_up_kind(got + at, pos - at), V92_UP_CPU);
        n = v92_cpu_parse(got + at, pos - at, unit, &cpo);
        CHECK(n > 0 && n % unit == 0);
        CHECK_EQ_I(memcmp(&cp, &cpo, sizeof cp), 0);
        CHECK_EQ_I(at + n + unit, pos);          /* what is left is E2u */
    }
}

int main(void)
{
    test_trn1u();
    test_ja_on_line();
    test_downstream();
    test_ru_su();
    test_trn2u();
    T_REPORT("test_sig");
}
