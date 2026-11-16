/* Shell mapper, V.34 9.4. Hand-derived vectors (derivations in the
 * comments follow steps 1 to 6.2 and (9-17) to (9-24) literally), then
 * structural properties and the inverse. */
#include "test.h"
#include "testutil.h"

static v34_params_t p;

static void setup(unsigned S, unsigned R, int expanded)
{
    v34_cfg_t c;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R; c.expanded = (uint8_t)expanded;
    CHECK_EQ_I(v34_params_init(&p, &c), 0);
}

static void expect(uint32_t r0, const char *want)
{
    uint8_t m[8];
    int i;
    v34_shell_map(&p, r0, m);
    for (i = 0; i < 8; i++)
        CHECK_EQ_I(m[i], want[i] - '0');
    CHECK_EQ_I(v34_shell_unmap(&p, m), r0);
}

int main(void)
{
    unsigned i;

    /* M = 2 (3200 sym/s, 7200 bit/s: K = 6).
     * g2 = 1 2 1; g4 = 1 4 6 4 1; z8 = 0 1 9 37 93 ...
     *
     * R0 = 0: A = 0, everything zero.
     * R0 = 1: A = 1 (z8(1) = 1 <= 1 < 9). R1 = 0; B = 1 would need
     *   0 - g4(0)g4(1) = -4, so B = 0. R2 = 0, R3 = 0. C = 0. D = 1 would
     *   need 0 - g2(0)g2(1) = -2, so D = 0, R5 = 0. E = F = G = H = 0.
     *   A-B-D = 1 < M: m30 = H = 0, m31 = 1.
     * R0 = 2: A = 1, R1 = 1, B = 0, R2 = 1 mod g4(0) = 0, R3 = 1. D = 0
     *   (1 - 2 < 0), R5 = 1, G = 1 mod g2(0) = 0, H = 1: m30 = 1, m31 = 0.
     * R0 = 3: A = 1, R1 = 2, B = 0, R3 = 2. D = 1: R5 = 2 - g2(0)g2(1) = 0;
     *   D = 2 would need 0 - g2(1)g2(0) = -2. G = 0 mod g2(1) = 0, H = 0:
     *   m20 = 0, m21 = 1; A-B-D = 0: m30 = m31 = 0.
     * R0 = 50: A = 3 (37 <= 50 < 93), R0 - z8(3) = 13. B = 1: 13 - g4(0)g4(3)
     *   = 9; B = 2 would need 9 - g4(1)g4(2) = -15. R1 = 9, R2 = 9 mod 4 = 1,
     *   R3 = 2. C = 1 would need 1 - g2(0)g2(1) = -1: C = 0, R4 = 1.
     *   D = 1: 2 - g2(0)g2(2) = 1; D = 2 would need 1 - g2(1)g2(1) = -3:
     *   D = 1, R5 = 1. E = 1 mod g2(0) = 0, F = 1; G = 1 mod g2(1) = 1, H = 0.
     *   m00 = 0, m01 = 0; B-C = 1: m10 = F = 1, m11 = 0; D = 1: m20 = G = 1,
     *   m21 = 0; A-B-D = 1: m30 = H = 0, m31 = 1.
     * R0 = 255 (the last word): A = 8, R1 = 0, B = 4, C = 2, D = 2, all of
     *   E..H zero; every "sum >= M" branch gives m = M-1 = 1 twice. */
    setup(3200, 7200, 0);
    CHECK_EQ_I(p.map.M, 2);
    expect(0,   "00000000");
    expect(1,   "00000001");
    expect(2,   "00000010");
    expect(3,   "00000100");
    expect(50,  "00101001");
    expect(255, "11111111");

    /* M = 3 (2743 sym/s, 7200 bit/s: K = 9). g2 = 1 2 3 2 1,
     * g4 = 1 4 10 16 19 16 10 4 1, g8(0..2) = 1 8 36, z8 = 0 1 9 45 ...
     * R0 = 9 is the first word of shell A = 2: B = C = D = 0, R1..R5 = 0,
     *   A-B-D = 2 < 3: m30 = H = 0, m31 = 2.
     * R0 = 12: A = 2, R1 = 3; B = 1 would need 3 - g4(0)g4(2) = -7: B = 0,
     *   R3 = 3. D = 1: 3 - g2(0)g2(2) = 0; D = 2 would need 0 - g2(1)g2(1)
     *   = -4: D = 1, R5 = 0, G = 0, H = 0: m20 = 0, m21 = 1; A-B-D = 1:
     *   m30 = 0, m31 = 1.
     * R0 = 44 is the last word of shell 2: R1 = 35; B = 1: 35 - 10 = 25;
     *   B = 2: 25 - g4(1)g4(1) = 9; B = 3 is impossible (B <= A). R2 = 9 mod
     *   g4(2) = 9, R3 = 0. C = 1: 9 - g2(0)g2(2) = 6; C = 2: 6 - g2(1)g2(1)
     *   = 2; R4 = 2, E = 2 mod g2(2) = 2, F = 0. D = 0, G = H = 0.
     *   C = 2 < 3: m00 = E = 2, m01 = 0; the rest zero. */
    setup(2743, 7200, 0);
    CHECK_EQ_I(p.map.M, 3);
    expect(9,  "00000002");
    expect(12, "00000101");
    expect(44, "20000000");

    /* exhaustive, small cases: every K-bit input gives a different 8-tuple
     * of rings < M, the ring sum never decreases with R0 (shells are filled
     * in order of weight) and the inverse recovers R0 */
    {
        static const struct { unsigned S, R, exp; } cs[] = {
            { 3000, 4800, 0 }, { 3200, 7200, 0 }, { 3000, 7200, 1 },
            { 2743, 7200, 0 }, { 3200, 9600, 1 }, { 3000, 9600, 0 }, { 3200, 12000, 1 } };
        for (i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            uint32_t r0, n, prev_code = 0;
            unsigned prev_sum = 0, bad_range = 0, bad_inv = 0, bad_order = 0, dup = 0;
            setup(cs[i].S, cs[i].R, cs[i].exp);
            n = (uint32_t)1 << p.map.K;
            for (r0 = 0; r0 < n; r0++) {
                uint8_t m[8];
                unsigned sum = 0, k;
                uint32_t code = 0;
                v34_shell_map(&p, r0, m);
                for (k = 0; k < 8; k++) {
                    if (m[k] >= p.map.M) bad_range++;
                    sum += m[k];
                    code = code * p.map.M + m[k];
                }
                if (sum < prev_sum) bad_order++;
                if (r0 && code == prev_code) dup++;
                if (v34_shell_unmap(&p, m) != r0) bad_inv++;
                prev_sum = sum; prev_code = code;
            }
            CHECK_EQ_I(bad_range, 0); CHECK_EQ_I(bad_order, 0);
            CHECK_EQ_I(bad_inv, 0);   CHECK_EQ_I(dup, 0);
        }
    }
    /* all supported 3000 / 3200 rates, both shaping choices: random inputs,
     * the extremes, and the largest index of a low frame */
    {
        unsigned s, R, e;
        for (s = 3; s < 6; s++)
            for (R = 4800; R <= tu_max_rate(tu_srates[s]); R += 2400)
                for (e = 0; e < 2; e++) {
                    unsigned bad = 0, t;
                    uint32_t mask;
                    uint8_t m[8];
                    setup(tu_srates[s], R, (int)e);
                    mask = (uint32_t)(((uint64_t)1 << p.map.K) - 1);
                    for (t = 0; t < 3000; t++) {
                        uint32_t r0 = t == 0 ? 0 : t == 1 ? mask : t == 2 ? mask >> 1 : (tu_rand() & mask);
                        unsigned k;
                        v34_shell_map(&p, r0, m);
                        for (k = 0; k < 8; k++) if (m[k] >= p.map.M) bad++;
                        if (v34_shell_unmap(&p, m) != r0) bad++;
                    }
                    CHECK_EQ_I(bad, 0);
                }
    }
    T_REPORT("test_shell");
}
