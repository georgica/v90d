/* Constellation (V.34 9.1, Figure 5), mapper rotation (9.6.1), subset
 * labels (Figure 9), Table 13 structure and the plain slicer. */
#include "test.h"
#include "testutil.h"
#include "v34_priv.h"
#include "fig5_data.h"

/* brute-force nearest constellation point; returns squared distance (Q14) */
static long brute(const v34_params_t *p, v34_pt_t r, int *bre, int *bim)
{
    long best = -1;
    for (unsigned n = 0; n < p->npts; n++)
        for (unsigned rot = 0; rot < 4; rot++) {
            int re, im;
            v34_map_point(n, rot, &re, &im);
            long dx = r.re - re * V34_ONE, dy = r.im - im * V34_ONE;
            long d = dx * dx + dy * dy;
            if (best < 0 || d < best) { best = d; *bre = re; *bim = im; }
        }
    return best;
}

int main(void)
{
    unsigned n, rot, i;
    int re, im;

    /* the library table (built from the ordering rule of 9.1) against the
     * figure as printed */
    for (n = 0; n < 416; n++) {
        CHECK_EQ_I(v34_constel[n][0], fig5[n][0]);
        CHECK_EQ_I(v34_constel[n][1], fig5[n][1]);
    }
    /* spot values read directly off Figure 5 */
    v34_map_point(0, 0, &re, &im);   CHECK(re == 1 && im == 1);
    v34_map_point(1, 0, &re, &im);   CHECK(re == -3 && im == 1);
    v34_map_point(2, 0, &re, &im);   CHECK(re == 1 && im == -3);
    v34_map_point(3, 0, &re, &im);   CHECK(re == -3 && im == -3);
    v34_map_point(4, 0, &re, &im);   CHECK(re == 1 && im == 5);
    v34_map_point(5, 0, &re, &im);   CHECK(re == 5 && im == 1);
    v34_map_point(408, 0, &re, &im); CHECK(re == -7 && im == 45);
    v34_map_point(394, 0, &re, &im); CHECK(re == 1 && im == 45);
    v34_map_point(415, 0, &re, &im); CHECK(re == 45 && im == 9);
    v34_map_point(393, 0, &re, &im); CHECK(re == 13 && im == -43);
    v34_map_point(392, 0, &re, &im); CHECK(re == -43 && im == 13);
    /* clockwise rotation: (1,1) -> (1,-1) -> (-1,-1) -> (-1,1) */
    v34_map_point(0, 1, &re, &im);   CHECK(re == 1 && im == -1);
    v34_map_point(0, 2, &re, &im);   CHECK(re == -1 && im == -1);
    v34_map_point(0, 3, &re, &im);   CHECK(re == -1 && im == 1);
    v34_map_point(5, 1, &re, &im);   CHECK(re == 1 && im == -5);

    /* map / unmap are inverse over the whole 1664-point superconstellation,
     * and the four rotations do not overlap */
    {
        static unsigned char seen[46][46];
        unsigned count = 0;
        for (n = 0; n < 416; n++)
            for (rot = 0; rot < 4; rot++) {
                unsigned l2 = 999, r2 = 9;
                v34_map_point(n, rot, &re, &im);
                CHECK_EQ_I(v34_unmap_point(re, im, &l2, &r2), 0);
                CHECK_EQ_I(l2, n);
                CHECK_EQ_I(r2, rot);
                if (!seen[(im + 45) / 2][(re + 45) / 2]++) count++;
            }
        CHECK_EQ_I(count, 1664);
        {
            unsigned l2, r2;
            CHECK(v34_unmap_point(45, 45, &l2, &r2) < 0);   /* corner: outside */
            CHECK(v34_unmap_point(2, 1, &l2, &r2) < 0);     /* off grid */
            CHECK(v34_unmap_point(47, 1, &l2, &r2) < 0);
        }
    }

    /* Figure 9: the 16 labels as printed (rows im = 3, 1, -1, -3; columns
     * re = -3, -1, 1, 3) */
    {
        static const unsigned char f9[4][4] = {
            { 1, 6, 5, 2 },     /* 001 110 101 010 */
            { 4, 3, 0, 7 },     /* 100 011 000 111 */
            { 5, 2, 1, 6 },     /* 101 010 001 110 */
            { 0, 7, 4, 3 },     /* 000 111 100 011 */
        };
        for (i = 0; i < 16; i++)
            CHECK_EQ_I(v34_subset(-3 + 2 * (int)(i % 4), 3 - 2 * (int)(i / 4)), f9[i / 4][i % 4]);
        /* 8-way partition: translating by (4,4), (4,-4), (8,0), (0,8) keeps
         * the subset, translating by (4,0) or (0,4) flips the top bit, and a
         * clockwise quarter turn adds one to the two low bits */
        for (re = -45; re <= 45; re += 2)
            for (im = -45; im <= 45; im += 2) {
                unsigned s = v34_subset(re, im);
                CHECK_EQ_I(v34_subset(re + 4, im + 4), s);
                CHECK_EQ_I(v34_subset(re + 4, im - 4), s);
                CHECK_EQ_I(v34_subset(re + 8, im), s);
                CHECK_EQ_I(v34_subset(re, im - 8), s);
                CHECK_EQ_I(v34_subset(re + 4, im), s ^ 4);
                CHECK_EQ_I(v34_subset(re, im + 4), s ^ 4);
                CHECK_EQ_I(v34_subset(im, -re), (s & 4) | ((s + 1) & 3));
                CHECK(v34_subset(re + 2, im) != s && v34_subset(re, im + 2) != s);
            }
        for (i = 0; i < 8; i++)
            CHECK_EQ_I(v34_subset(v34_subset_rep[i][0], v34_subset_rep[i][1]), i);
        /* quarter constellation points all have low bits 00 */
        for (n = 0; n < 416; n++)
            CHECK_EQ_I(v34_subset(v34_constel[n][0], v34_constel[n][1]) & 3, 0);
    }

    /* Table 13 as printed: spot entries, and its structure. With
     * s = (a, l), a the top bit and l the two low bits:
     * Y4 = a1 ^ a2, [Y3 Y2] = l1, Y1 = bit 1 of (l2 - l1) mod 4; every
     * 4-bit value occurs four times, twice with each parity of l1 ^ l2. */
    {
        unsigned cnt[16][2];
        memset(cnt, 0, sizeof cnt);
        CHECK_EQ_I(v34_tab13[0][0], 0x0); CHECK_EQ_I(v34_tab13[1][0], 0x3);
        CHECK_EQ_I(v34_tab13[3][2], 0x7); CHECK_EQ_I(v34_tab13[0][3], 0x1);
        CHECK_EQ_I(v34_tab13[7][7], 0x6); CHECK_EQ_I(v34_tab13[5][4], 0x3);
        CHECK_EQ_I(v34_tab13[2][5], 0xD); CHECK_EQ_I(v34_tab13[6][1], 0xD);
        for (unsigned s1 = 0; s1 < 8; s1++)
            for (unsigned s2 = 0; s2 < 8; s2++) {
                unsigned a1 = s1 >> 2, l1 = s1 & 3, a2 = s2 >> 2, l2 = s2 & 3;
                unsigned want = (a1 ^ a2) << 3 | l1 << 1 | (((l2 - l1) & 3) >> 1);
                CHECK_EQ_I(v34_tab13[s1][s2], want);
                cnt[v34_tab13[s1][s2]][(l1 ^ l2) & 1]++;
            }
        for (i = 0; i < 16; i++) { CHECK_EQ_I(cnt[i][0], 2); CHECK_EQ_I(cnt[i][1], 2); }
    }

    /* slicer: exact against brute force, for small and large constellations,
     * for points near the constellation and up to 4 units outside its
     * bounding box (inputs further out are clamped to that box first) */
    {
        static const struct { unsigned S, R, exp; } cs[] = {
            { 3200, 4800, 0 }, { 3200, 7200, 0 }, { 3200, 9600, 0 }, { 3200, 9600, 1 },
            { 3200, 12000, 0 }, { 3200, 14400, 1 }, { 3200, 19200, 0 }, { 3000, 21600, 1 },
            { 3200, 26400, 0 }, { 3000, 28800, 0 }, { 3200, 31200, 1 }, { 3429, 33600, 1 } };
        for (i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            static v34_params_t p;
            v34_cfg_t c;
            unsigned bad = 0, invalid = 0, trials = 4000, t;
            double scale;
            memset(&c, 0, sizeof c);
            c.symbol_rate = (uint16_t)cs[i].S; c.data_rate = (uint16_t)cs[i].R;
            c.expanded = (uint8_t)cs[i].exp;
            CHECK_EQ_I(v34_params_init(&p, &c), 0);
            scale = p.ymax + 4.0;
            for (t = 0; t < trials; t++) {
                v34_pt_t r, s;
                int bre = 0, bim = 0;
                long d, bd;
                unsigned l2, r2;
                if (t & 1) {              /* anywhere in a box around it */
                    r.re = (int16_t)lrint((2 * tu_uniform() - 1) * scale * V34_ONE);
                    r.im = (int16_t)lrint((2 * tu_uniform() - 1) * scale * V34_ONE);
                } else {                  /* a constellation point plus noise */
                    v34_map_point(tu_rand() % p.npts, tu_rand() & 3, &re, &im);
                    r.re = (int16_t)(re * V34_ONE); r.im = (int16_t)(im * V34_ONE);
                    r = tu_add_noise(r, 0.8);
                }
                s = v34_slice(&p, r);
                if ((s.re % V34_ONE) || (s.im % V34_ONE) ||
                    v34_unmap_point(s.re / V34_ONE, s.im / V34_ONE, &l2, &r2) || l2 >= p.npts) {
                    invalid++;
                    continue;
                }
                bd = brute(&p, r, &bre, &bim);
                d = (long)(r.re - s.re) * (r.re - s.re) + (long)(r.im - s.im) * (r.im - s.im);
                if (d != bd) bad++;
            }
            CHECK_EQ_I(invalid, 0);
            CHECK_EQ_I(bad, 0);
            if (bad) printf("  slicer: L=%u: %u of %u not nearest\n", p.map.L, bad, trials);
        }
        /* the per-subset slicer the Viterbi decoder uses: for each of the 8
         * subsets of Figure 9 the nearest constellation point of that
         * subset, exact against brute force; "no such point" when the
         * constellation has none (small constellations) */
        for (i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            static v34_params_t p;
            v34_cfg_t c;
            unsigned bad = 0, bad4 = 0, wrong_subset = 0, t, empty = 0;
            memset(&c, 0, sizeof c);
            c.symbol_rate = (uint16_t)cs[i].S; c.data_rate = (uint16_t)cs[i].R;
            c.expanded = (uint8_t)cs[i].exp;
            CHECK_EQ_I(v34_params_init(&p, &c), 0);
            for (t = 0; t < 1500; t++) {
                v34_pt_t r;
                unsigned sset;
                long bestd[8];
                if (t & 1) {
                    r.re = (int16_t)lrint((2 * tu_uniform() - 1) * (p.ymax + 3.0) * V34_ONE);
                    r.im = (int16_t)lrint((2 * tu_uniform() - 1) * (p.ymax + 3.0) * V34_ONE);
                } else {
                    v34_map_point(tu_rand() % p.npts, tu_rand() & 3, &re, &im);
                    r.re = (int16_t)(re * V34_ONE); r.im = (int16_t)(im * V34_ONE);
                    r = tu_add_noise(r, 0.6);
                }
                for (sset = 0; sset < 8; sset++) bestd[sset] = -1;
                for (n = 0; n < p.npts; n++)
                    for (rot = 0; rot < 4; rot++) {
                        long dx, dy, d;
                        v34_map_point(n, rot, &re, &im);
                        sset = v34_subset(re, im);
                        dx = r.re - re * V34_ONE; dy = r.im - im * V34_ONE;
                        d = dx * dx + dy * dy;
                        if (bestd[sset] < 0 || d < bestd[sset]) bestd[sset] = d;
                    }
                /* the 4-way partition used for the 16-state code: class l is
                 * subsets l and l + 4 together */
                for (sset = 0; sset < 4; sset++) {
                    int16_t pt[2] = { 0, 0 };
                    uint32_t m = v34_slice_class(&p, r.re, r.im, sset, pt);
                    long want = bestd[sset], dx, dy;
                    if (want < 0 || (bestd[sset + 4] >= 0 && bestd[sset + 4] < want)) want = bestd[sset + 4];
                    if ((v34_subset(pt[0], pt[1]) & 3) != sset || !v34_inside(&p, pt[0], pt[1])) { wrong_subset++; continue; }
                    dx = r.re - pt[0] * V34_ONE; dy = r.im - pt[1] * V34_ONE;
                    if (dx * dx + dy * dy != want) bad++;
                    if (m != (uint32_t)((dx * dx + dy * dy) >> V34_M_SHIFT)) bad++;
                }
                /* the decoder's all-four-at-once version: same answers */
                {
                    uint32_t m4[4];
                    int16_t pt4[4][2];
                    v34_slice_classes(&p, r.re, r.im, m4, pt4);
                    for (sset = 0; sset < 4; sset++) {
                        long want = bestd[sset], dx, dy;
                        if (want < 0 || (bestd[sset + 4] >= 0 && bestd[sset + 4] < want)) want = bestd[sset + 4];
                        if ((v34_subset(pt4[sset][0], pt4[sset][1]) & 3) != sset ||
                            !v34_inside(&p, pt4[sset][0], pt4[sset][1])) { wrong_subset++; continue; }
                        dx = r.re - pt4[sset][0] * V34_ONE; dy = r.im - pt4[sset][1] * V34_ONE;
                        if (dx * dx + dy * dy != want) bad4++;
                        if (m4[sset] != (uint32_t)((dx * dx + dy * dy) >> V34_M_SHIFT)) bad4++;
                    }
                }
                for (sset = 0; sset < 8; sset++) {
                    int16_t pt[2] = { 0, 0 };
                    uint32_t m = v34_slice_subset(&p, r.re, r.im, sset, pt);
                    if (bestd[sset] < 0) {
                        empty++;
                        if (m != V34_M_BIG) bad++;
                        continue;
                    }
                    if (v34_subset(pt[0], pt[1]) != sset || !v34_inside(&p, pt[0], pt[1])) { wrong_subset++; continue; }
                    {
                        long dx = r.re - pt[0] * V34_ONE, dy = r.im - pt[1] * V34_ONE;
                        if (dx * dx + dy * dy != bestd[sset]) bad++;
                        if (m != (uint32_t)((dx * dx + dy * dy) >> V34_M_SHIFT)) bad++;
                    }
                }
            }
            CHECK_EQ_I(wrong_subset, 0);
            CHECK_EQ_I(bad4, 0);
            CHECK_EQ_I(bad, 0);
            if (p.map.L == 4) CHECK_EQ_I(empty, 4 * 1500);   /* only 4 subsets exist */
        }
        /* far outside the grid still gives a valid point */
        {
            static v34_params_t p;
            v34_cfg_t c;
            v34_pt_t r = { 32767, -32768 }, s;
            unsigned l2, r2;
            memset(&c, 0, sizeof c);
            c.symbol_rate = 3200; c.data_rate = 19200;
            CHECK_EQ_I(v34_params_init(&p, &c), 0);
            s = v34_slice(&p, r);
            CHECK_EQ_I(v34_unmap_point(s.re / V34_ONE, s.im / V34_ONE, &l2, &r2), 0);
            CHECK(l2 < p.npts);
            CHECK(s.re > 0 && s.im < 0);
        }
    }
    T_REPORT("test_constel");
}
