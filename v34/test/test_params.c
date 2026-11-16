/* Framing and mapping parameters: the formulas of V.34 8.2 and 9.2 against
 * every entry of Tables 7, 8 and 10 as printed (test/spec_tables.h). */
#include "test.h"
#include "testutil.h"
#include "spec_tables.h"

int main(void)
{
    unsigned i, n8 = sizeof spec_t8 / sizeof spec_t8[0];
    unsigned n10 = sizeof spec_t10 / sizeof spec_t10[0];
    v34_map_t m, e;

    CHECK_EQ_I(n8, 130);
    CHECK_EQ_I(n10, 130);

    /* Table 7 */
    {
        static const struct { unsigned S, J, P; } t7[6] = {
            { 2400, 7, 12 }, { 2743, 8, 12 }, { 2800, 7, 14 },
            { 3000, 7, 15 }, { 3200, 7, 16 }, { 3429, 8, 15 } };
        for (i = 0; i < 6; i++) {
            CHECK_EQ_I(v34_map_params(t7[i].S, 4800 + (t7[i].S == 2400 ? 0 : 0), 0, &m), 0);
            CHECK_EQ_I(m.J, t7[i].J);
            CHECK_EQ_I(m.P, t7[i].P);
        }
    }
    /* Table 8: b and SWP */
    for (i = 0; i < n8; i++) {
        int rc = v34_map_params(spec_t8[i].S, spec_t8[i].R, 0, &m);
        CHECK_EQ_I(rc, 0);
        if (rc) continue;
        CHECK_EQ_I(m.b, spec_t8[i].b);
        CHECK_EQ_I(m.swp, spec_t8[i].swp);
        /* (8-1), (8-2) */
        CHECK_EQ_I((unsigned long)m.N * m.J * 100, (unsigned long)spec_t8[i].R * 28);
        CHECK_EQ_I(m.r, m.N - (m.b - 1) * m.P);
        CHECK(m.r >= 1 && m.r <= m.P);
    }
    /* Table 10: K, M, L for both shaping choices */
    for (i = 0; i < n10; i++) {
        int rc = v34_map_params(spec_t10[i].S, spec_t10[i].R, 0, &m);
        rc |= v34_map_params(spec_t10[i].S, spec_t10[i].R, 1, &e);
        CHECK_EQ_I(rc, 0);
        if (rc) continue;
        CHECK_EQ_I(m.K, spec_t10[i].K);
        CHECK_EQ_I(e.K, spec_t10[i].K);
        CHECK_EQ_I(m.M, spec_t10[i].Mmin);
        CHECK_EQ_I(e.M, spec_t10[i].Mexp);
        CHECK_EQ_I(m.L, spec_t10[i].Lmin);
        CHECK_EQ_I(e.L, spec_t10[i].Lexp);
        /* (9-1), (9-2), (9-29) */
        CHECK_EQ_I(m.K, m.b <= 12 ? 0 : m.b - 12 - 8 * m.q);
        CHECK(m.K < 32);
        CHECK_EQ_I(m.L, 4u * m.M << m.q);
        CHECK_EQ_I(m.w, m.b < 56 ? 1 : 2);
    }
    /* pairs that are not in Table 8 */
    CHECK(v34_map_params(3200, 33600, 0, &m) != 0);
    CHECK(v34_map_params(3000, 31200, 0, &m) != 0);
    CHECK(v34_map_params(3200, 2400, 0, &m) != 0);
    CHECK(v34_map_params(3100, 9600, 0, &m) != 0);
    CHECK(v34_map_params(2400, 24000, 0, &m) != 0);

    /* v34_params_init: accepts multiples of 2400 only (no auxiliary channel) */
    {
        static v34_params_t p;
        v34_cfg_t c;
        memset(&c, 0, sizeof c);
        c.symbol_rate = 3200; c.data_rate = 19200; c.scrambler = V34_GPA;
        CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.map.b, 48); CHECK_EQ_I(p.map.K, 28); CHECK_EQ_I(p.map.M, 12);
        CHECK_EQ_I(p.npts, 24); CHECK_EQ_I(p.precode, 0); CHECK_EQ_I(p.nstates, 16);
        c.data_rate = 19400; CHECK_EQ_I(v34_params_init(&p, &c), -1);
        c.data_rate = 33600; CHECK_EQ_I(v34_params_init(&p, &c), -1);
        c.data_rate = 19200; c.trellis = 3; CHECK_EQ_I(v34_params_init(&p, &c), -2);
        c.trellis = V34_TRELLIS_64; CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.nstates, 64);
        c.trellis = 0; c.nonlinear = 1; CHECK_EQ_I(v34_params_init(&p, &c), 0);    /* 9.7: see test_nl */
        c.nonlinear = 0; c.h[1][1] = -5; CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.precode, 1);
    }
    /* shell mapper tables for M = 2, by hand from (9-3) to (9-6):
     * g2 = 1 2 1, g4 = 1 4 6 4 1, g8 = 1 8 28 56 70 56 28 8 1,
     * z8 = 0 1 9 37 93 163 219 247 255 */
    {
        static v34_params_t p;
        v34_cfg_t c;
        static const unsigned g4[5] = { 1, 4, 6, 4, 1 };
        static const unsigned z8[9] = { 0, 1, 9, 37, 93, 163, 219, 247, 255 };
        memset(&c, 0, sizeof c);
        c.symbol_rate = 3200; c.data_rate = 7200;      /* K = 6, M = 2, L = 8 */
        CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.map.M, 2); CHECK_EQ_I(p.map.K, 6);
        CHECK_EQ_I(p.g2[0], 1); CHECK_EQ_I(p.g2[1], 2); CHECK_EQ_I(p.g2[2], 1);
        for (i = 0; i < 5; i++) CHECK_EQ_I(p.g4[i], g4[i]);
        for (i = 0; i < 9; i++) CHECK_EQ_I(p.z8[i], z8[i]);
        /* Energy by hand: 2^6 = 64 codewords = shells A=0,1,2 complete
         * (1 + 8 + 28 = 37 words) + 27 words of shell 3. Ring 1 is used
         * 8*1 + 28*2 + 27*3 = 145 times out of 512; ring 0 is point 0
         * (energy 2), ring 1 is point 1 (energy 10):
         * E = 2 + 8 * 145/512 = 4.265625 */
        CHECK_NEAR(p.energy_q8 / 256.0, 4.265625, 0.01);
        /* 4-point constellation: energy 2 exactly */
        c.data_rate = 4800;
        CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.map.L, 4);
        CHECK_EQ_I(p.energy_q8, 512);
    }
    /* z8 never decreases and saturates instead of wrapping (M = 18) */
    {
        static v34_params_t p;
        v34_cfg_t c;
        memset(&c, 0, sizeof c);
        c.symbol_rate = 3200; c.data_rate = 16800; c.expanded = 1;  /* K=30, M=17 */
        CHECK_EQ_I(v34_params_init(&p, &c), 0);
        CHECK_EQ_I(p.map.M, 17);
        for (i = 1; i <= 8u * (p.map.M - 1) + 1; i++) CHECK(p.z8[i] >= p.z8[i - 1]);
        CHECK(p.z8[8 * (p.map.M - 1) + 1] == 0xFFFFFFFFu);
    }
    /* gain helper: sqrt(2 / E) in Q14 */
    CHECK_EQ_I(v34_gain_q14(V34_ENERGY_4PT_Q8), 16384);
    CHECK_NEAR(v34_gain_q14(V34_ENERGY_16PT_Q8), 16384 * sqrt(0.2), 2);
    CHECK_NEAR(v34_gain_q14(256 * 300), 16384 * sqrt(2.0 / 300), 2);

    T_REPORT("test_params");
}
