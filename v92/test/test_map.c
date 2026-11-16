/* Mapping parameter checks and the index <-> Ki relation of V.92 6.4.2. */
#include <string.h>
#include "test.h"
#include "testutil.h"

static const uint8_t M5[12] = { 5, 5, 5, 3, 5, 5, 5, 3, 5, 5, 5, 3 };

static void test_point(void)
{
    v92_map_t m;
    tu_map_uniform(&m, 8, 0, M5, 4, 100);     /* points +-100, 300, 500, 700 */
    /* "negative points have negative indices, positive points have
     * non-negative indices", indices in the order of the levels */
    CHECK_EQ_I(v92_point(&m, 0, 0), 100);
    CHECK_EQ_I(v92_point(&m, 0, 3), 700);
    CHECK_EQ_I(v92_point(&m, 0, -1), -100);
    CHECK_EQ_I(v92_point(&m, 0, -4), -700);
    CHECK_EQ_I(v92_lc(&m, 7), 4);
    /* a second set in intervals 1 and 7 */
    m.cidx[1] = 1; m.lc[1] = 2; m.pt[1][0] = 33; m.pt[1][1] = 99;
    CHECK_EQ_I(v92_point(&m, 1, 1), 99);
    CHECK_EQ_I(v92_point(&m, 7, -2), -99);
    CHECK_EQ_I(v92_point(&m, 6, -2), -300);
    CHECK_EQ_I(v92_lc(&m, 7), 2);
}

/* E(Ki) = { a(eta) | eta = Ki + z Mi } for k = 0, 1, 2 and
 *         { a(eta) | eta = 2 Ki + 2 z Mi + parity } for k = 3. */
static void test_eta_to_k(void)
{
    v92_map_t m;
    int i, ki, z, par;
    tu_map_uniform(&m, 8, 0, M5, 64, 10);
    /* by hand: interval 0 (k = 0, M = 5): -6 = 4 - 2*5, 7 = 2 + 5 */
    CHECK_EQ_I(v92_eta_to_k(&m, 0, -6), 4);
    CHECK_EQ_I(v92_eta_to_k(&m, 0, 7), 2);
    CHECK_EQ_I(v92_eta_to_k(&m, 0, -1), 4);
    CHECK_EQ_I(v92_eta_to_k(&m, 0, 0), 0);
    /* interval 3 (k = 3, M = 3): -1 = 2*2 - 2*3 + 1, 5 = 2*2 + 1,
     * -8 = 2*2 - 2*2*3 + 0, 6 = 2*0 + 2*3 + 0, 1 = 2*0 + 1 */
    CHECK_EQ_I(v92_eta_to_k(&m, 3, -1), 2);
    CHECK_EQ_I(v92_eta_to_k(&m, 3, 5), 2);
    CHECK_EQ_I(v92_eta_to_k(&m, 3, -8), 2);
    CHECK_EQ_I(v92_eta_to_k(&m, 3, 6), 0);
    CHECK_EQ_I(v92_eta_to_k(&m, 3, 1), 0);
    /* interval 11 is k = 3 as well, interval 9 is k = 1 */
    CHECK_EQ_I(v92_eta_to_k(&m, 11, -3), 1);      /* -3 = 2*1 - 6 + 1 */
    CHECK_EQ_I(v92_eta_to_k(&m, 9, -3), 2);       /* -3 = 2 - 5 */
    /* every member of every class, from the definition */
    for (i = 0; i < 12; i++)
        for (ki = 0; ki < m.M[i]; ki++)
            for (z = -6; z <= 6; z++)
                for (par = 0; par < 2; par++) {
                    int eta = (i % 4 == 3) ? 2 * ki + 2 * z * m.M[i] + par
                                           : ki + z * m.M[i];
                    if (eta < -64 || eta > 63) continue;
                    CHECK_EQ_I(v92_eta_to_k(&m, i, eta), ki);
                }
}

static void test_check(void)
{
    v92_map_t m, g;
    /* M = 5^9 * 3^3 = 52734375 >= 2^25 = 33554432, < 2^26 */
    tu_map_uniform(&g, 25, 0, M5, 4, 100);        /* N = 8 */
    CHECK_EQ_I(v92_map_check(&g), 0);
    m = g; m.K = 26; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.K = 0;  CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.trellis = 3; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.trellis = 2; CHECK_EQ_I(v92_map_check(&m), 0);
    m = g; m.M[4] = 0; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    /* k = 0..2 needs Mi <= N = 8; k = 3 needs 2 Mi <= 8 */
    m = g; m.M[0] = 8; CHECK_EQ_I(v92_map_check(&m), 0);
    m = g; m.M[0] = 9; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.M[3] = 4; CHECK_EQ_I(v92_map_check(&m), 0);
    m = g; m.M[7] = 5; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    /* constellation sets */
    m = g; m.cidx[2] = 1; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);   /* empty set */
    m = g; m.cidx[2] = 6; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.pt[0][0] = 0; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);  /* zero point */
    m = g; m.pt[0][2] = 300; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE); /* not ascending */
    m = g; m.pt[0][3] = 32768; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    m = g; m.pt[0][3] = 32767; CHECK_EQ_I(v92_map_check(&m), 0);
    m = g; m.lc[0] = 129; CHECK_EQ_I(v92_map_check(&m), V92_ERR_RANGE);
    /* an unused set is not looked at */
    m = g; m.lc[3] = 2; m.pt[3][0] = 9; m.pt[3][1] = 9; CHECK_EQ_I(v92_map_check(&m), 0);
}

static void test_filt(void)
{
    v92_filt_t f;
    memset(&f, 0, sizeof f);
    f.lz2 = 1; f.gain4 = 1; f.coef[0] = 16384;
    CHECK_EQ_I(v92_filt_check(&f), 0);
    f.lz2 = 0; CHECK_EQ_I(v92_filt_check(&f), V92_ERR_RANGE);
    f.lz2 = 1; f.gain4 = 0; CHECK_EQ_I(v92_filt_check(&f), V92_ERR_RANGE);
    f.gain4 = 1; f.lp1 = 321; CHECK_EQ_I(v92_filt_check(&f), V92_ERR_RANGE);
    f.lp1 = 320; f.lz1 = 63; CHECK_EQ_I(v92_filt_check(&f), 0);            /* 384 in all */
    f.lz1 = 64; CHECK_EQ_I(v92_filt_check(&f), V92_ERR_RANGE);
}

int main(void)
{
    test_point();
    test_eta_to_k();
    test_check();
    test_filt();
    T_REPORT("test_map");
}
