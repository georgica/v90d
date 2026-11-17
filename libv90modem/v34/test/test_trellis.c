/* Trellis encoder (V.34 9.6.3), precoder (9.6.2) and the complete data
 * mode encoder against vectors derived by hand from the Recommendation. */
#include "test.h"
#include "testutil.h"
#include "v34_priv.h"

static int ones(void *u) { (void)u; return 1; }

static v34_params_t p;
static void setup(unsigned S, unsigned R, int trellis, int expanded, const int16_t h[3][2])
{
    v34_cfg_t c;
    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R;
    c.trellis = (uint8_t)trellis; c.expanded = (uint8_t)expanded; c.scrambler = V34_GPA;
    if (h) memcpy(c.h, h, sizeof c.h);
    CHECK_EQ_I(v34_params_init(&p, &c), 0);
}

/* bit inversion pattern of Table 12 as printed, for the test's own V0 */
static int v0_ref(unsigned J, unsigned P, unsigned long m)
{
    static const char p7[] = "01110111111110", p8[] = "0111011111111010";
    unsigned long half;
    if (m % (2 * P)) return 0;
    half = m / (2 * P);                 /* 0, 1 are B1 = last frame of a superframe */
    half = (half + 2 * J - 2) % (2 * J);
    return (J == 8 ? p8 : p7)[half] - '0';
}

/* Is the y sequence a code sequence? Run the convolutional encoder over the
 * subset labels from state `start` and compare its output with the parity
 * the two labels carry: lsb(s(2m)) ^ lsb(s(2m+1)) = Y0(m) ^ V0(m).
 * Returns the number of violations. */
static unsigned syndrome(const v34_pt_t *y, unsigned long n4d, int trellis, unsigned start,
                         unsigned J, unsigned P)
{
    unsigned st = start, bad = 0;
    for (unsigned long m = 0; m < n4d; m++) {
        unsigned s1 = v34_subset(y[2 * m].re / V34_ONE, y[2 * m].im / V34_ONE);
        unsigned s2 = v34_subset(y[2 * m + 1].re / V34_ONE, y[2 * m + 1].im / V34_ONE);
        if (((s1 ^ s2) & 1) != (v34_conv_y0(trellis, st) ^ (unsigned)v0_ref(J, P, m))) bad++;
        st = v34_conv_next(trellis, st, v34_tab13[s1][s2]);
    }
    return bad;
}

#define NSYM 4096
static v34_pt_t xs[NSYM], ys[NSYM];

int main(void)
{
    unsigned i;

    /* ---- Table 12 ---- */
    {
        static const char p7[] = "01110111111110", p8[] = "0111011111111010";
        for (i = 0; i < 28; i++) CHECK_EQ_I(v34_v0(7, i), p7[i % 14] - '0');
        for (i = 0; i < 32; i++) CHECK_EQ_I(v34_v0(8, i), p8[i % 16] - '0');
    }

    /* ---- Figure 10, 16 states. By hand, state written d1 d2 d3 d4 and
     * Y0 = d4: from 0000 an input Y1 = 1 gives 0001; then with zero input
     * 1100, 0110, 0011, 1101. So Y0(1..5) = 1 0 0 1 1. ---- */
    {
        unsigned st = 0, y0[8];
        static const unsigned want_state[5] = { 0x8, 0x3, 0x6, 0xC, 0xB };  /* bit i-1 = d_i */
        static const unsigned want_y0[5] = { 1, 0, 0, 1, 1 };
        for (i = 0; i < 5; i++) {
            st = v34_conv_next(V34_TRELLIS_16, st, i == 0 ? 0x1 : 0);
            y0[i] = v34_conv_y0(V34_TRELLIS_16, st);
            CHECK_EQ_I(st, want_state[i]);
            CHECK_EQ_I(y0[i], want_y0[i]);
        }
        /* Y4 and Y3 are not used */
        for (st = 0; st < 16; st++)
            for (i = 0; i < 16; i++)
                CHECK_EQ_I(v34_conv_next(V34_TRELLIS_16, st, i), v34_conv_next(V34_TRELLIS_16, st, i & 3));
    }
    /* The same figures read as parity-check recursions (derived by walking
     * the delay line backwards from Y0):
     * 16 states: Y0(m) = Y0(m-4) + Y0(m-3) + Y2(m-3) + Y2(m-2) + Y1(m-1)
     * 32 states: Y0(m) = Y0(m-5) + Y2(m-4) + Y1(m-3) + Y4(m-2) + Y2(m-1) */
    {
        enum { N = 400 };
        static unsigned y[N], o16[N], o32[N];
        unsigned s16 = 0, s32 = 0, bad16 = 0, bad32 = 0, used32 = 0;
        for (i = 0; i < N; i++) {
            y[i] = tu_rand() & 15;
            o16[i] = v34_conv_y0(V34_TRELLIS_16, s16);
            o32[i] = v34_conv_y0(V34_TRELLIS_32, s32);
            if (v34_conv_next(V34_TRELLIS_32, s32, y[i]) != v34_conv_next(V34_TRELLIS_32, s32, y[i] ^ 4)) used32++;
            s16 = v34_conv_next(V34_TRELLIS_16, s16, y[i]);
            s32 = v34_conv_next(V34_TRELLIS_32, s32, y[i]);
            if (i >= 5) {
#define Y1(k) (y[k] & 1)
#define Y2(k) ((y[k] >> 1) & 1)
#define Y4(k) ((y[k] >> 3) & 1)
                if (o16[i] != (o16[i - 4] ^ o16[i - 3] ^ Y2(i - 3) ^ Y2(i - 2) ^ Y1(i - 1))) bad16++;
                if (o32[i] != (o32[i - 5] ^ Y2(i - 4) ^ Y1(i - 3) ^ Y4(i - 2) ^ Y2(i - 1))) bad32++;
            }
        }
        CHECK_EQ_I(bad16, 0);
        CHECK_EQ_I(bad32, 0);
        CHECK_EQ_I(used32, 0);          /* Y3 is not used by the 32-state code */
    }
    /* Figure 12, 64 states: every state has 16 distinct successors (the
     * four inputs are all used), each state is reached from 16 states, and
     * Y0 does not depend on the current input (it is state bit d6) */
    {
        unsigned st, in, indeg[64];
        memset(indeg, 0, sizeof indeg);
        for (st = 0; st < 64; st++) {
            unsigned seen[64], distinct = 0;
            memset(seen, 0, sizeof seen);
            for (in = 0; in < 16; in++) {
                unsigned ns = v34_conv_next(V34_TRELLIS_64, st, in);
                CHECK(ns < 64);
                if (!seen[ns]++) distinct++;
                indeg[ns]++;
            }
            CHECK_EQ_I(distinct, 16);
        }
        for (st = 0; st < 64; st++) CHECK_EQ_I(indeg[st], 16);
    }

    /* ---- precoder arithmetic, 9.6.2 items 1) to 3) ---- */
    {
        static const int16_t h_half[3][2] = { { 8192, 0 }, { 0, 0 }, { 0, 0 } };   /* h(1) = 0.5 */
        static const int16_t h_j[3][2] = { { 0, 0 }, { 0, 16384 }, { 0, 0 } };     /* h(2) = j */
        static const int16_t h_one[3][2] = { { 16384, 0 }, { 0, 0 }, { 0, 0 } };   /* h(1) = 1 */
        int32_t xh[3][2], pn[2], cn[2];
        memset(xh, 0, sizeof xh);
        /* rounding to 2^-7, a tie going to the smaller magnitude */
        setup(3200, 9600, 0, 0, h_half);
        xh[0][0] = 1;  xh[0][1] = -1; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], 0);  CHECK_EQ_I(pn[1], 0);
        xh[0][0] = 3;  xh[0][1] = -3; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], 1);  CHECK_EQ_I(pn[1], -1);
        xh[0][0] = 4;  xh[0][1] = -4; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], 2);  CHECK_EQ_I(pn[1], -2);
        xh[0][0] = 5;  xh[0][1] = -5; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], 2);  CHECK_EQ_I(pn[1], -2);
        /* complex product: j * (1 + 0j) = j, j * j = -1 */
        setup(3200, 9600, 0, 0, h_j);
        memset(xh, 0, sizeof xh);
        xh[1][0] = 128; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], 0);    CHECK_EQ_I(pn[1], 128);
        xh[1][0] = 0; xh[1][1] = 128; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(pn[0], -128); CHECK_EQ_I(pn[1], 0);
        /* quantising p to 2^w: w = 1 for b < 56 (3200 / 9600: b = 24) */
        setup(3200, 9600, 0, 0, h_one);
        CHECK_EQ_I(p.map.w, 1);
        memset(xh, 0, sizeof xh);
        xh[0][0] = 128;  xh[0][1] = -128; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 0);  CHECK_EQ_I(cn[1], 0);   /* 1.0: tie */
        xh[0][0] = 129;  xh[0][1] = -129; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 2);  CHECK_EQ_I(cn[1], -2);
        xh[0][0] = 384;  xh[0][1] = -384; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 2);  CHECK_EQ_I(cn[1], -2);  /* 3.0: tie */
        xh[0][0] = 385;  xh[0][1] = 127;  v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 4);  CHECK_EQ_I(cn[1], 0);
        /* w = 2 for b >= 56 (3200 / 24000: b = 60) */
        setup(3200, 24000, 0, 0, h_one);
        CHECK_EQ_I(p.map.w, 2);
        xh[0][0] = 256;  xh[0][1] = -256; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 0);  CHECK_EQ_I(cn[1], 0);   /* 2.0: tie */
        xh[0][0] = 257;  xh[0][1] = -257; v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 4);  CHECK_EQ_I(cn[1], -4);
        xh[0][0] = 768;  xh[0][1] = 769;  v34_precode_pc(&p, xh, pn, cn); CHECK_EQ_I(cn[0], 4);  CHECK_EQ_I(cn[1], 8);
    }

    /* ---- the whole encoder, by hand: 3200 sym/s, 4800 bit/s, GPA, 16
     * states, no precoding. b = 12, K = 0, q = 0: every 4D symbol carries
     * (I1, I2, I3) and uses point 0 = (1,1) only.
     * B1 = scrambled ones from the zero state: 11111 00000 11111 00000 1110..
     * mapping frame 0: (1,1,1) (1,1,0) (0,0,0) (0,1,1); frame 1 starts (1,1,1).
     * m=0: I = I2 + 2 I3 = 3, Z = 3: u(0) = (1,1) turned 270 cw = (-1,1).
     *      Y0 = 0; V0 = 1 (B1 counts as the last data frame of a superframe:
     *      pattern bit 12 of 01110111111110); U0 = 1.
     *      u(1): turn Z + 2 I1 + U0 = 6 = 2 -> (-1,-1).
     *      labels 011, 010 -> Table 13: 0111 -> Y2 = 1, Y1 = 1 -> state 0111.
     * m=1: I = 1, Z = 0: u(2) = (1,1). Y0 = 1, V0 = 0, U0 = 1.
     *      u(3): turn 0 + 2 + 1 = 3 -> (-1,1). labels 000, 011 -> 0001 ->
     *      Y2 = 0, Y1 = 1 -> state 1110.
     * m=2: I = 0, Z = 0: u(4) = (1,1). Y0 = 0, U0 = 0. u(5) = (1,1).
     *      labels 000, 000 -> 0000 -> state 0111.
     * m=3: I = 3, Z = 3: u(6) = (-1,1). Y0 = 1, U0 = 1.
     *      u(7): turn 3 + 0 + 1 = 0 -> (1,1). labels 011, 000 -> 0110 ->
     *      Y2 = 1, Y1 = 0 -> state 1001.
     * m=4: I = 3, Z = 2: u(8) = (-1,-1). Y0 = 1, U0 = 1.
     *      u(9): turn 2 + 2 + 1 = 1 -> (1,-1). ---- */
    {
        static const signed char want[10][2] = {
            { -1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 }, { 1, 1 }, { 1, 1 }, { -1, 1 }, { 1, 1 },
            { -1, -1 }, { 1, -1 } };
        v34_tx_t tx;
        setup(3200, 4800, V34_TRELLIS_16, 0, NULL);
        v34_tx_init(&tx, &p, ones, NULL);
        for (i = 0; i < 10; i++) {
            v34_pt_t x, y;
            v34_tx_symbol(&tx, &x, &y);
            CHECK_EQ_I(x.re, want[i][0] * V34_ONE);
            CHECK_EQ_I(x.im, want[i][1] * V34_ONE);
            CHECK(x.re == y.re && x.im == y.im);
            CHECK_EQ_I(v34_tx_in_b1(&tx), 1);
        }
        /* B1 is one data frame = 8 * P symbols */
        for (; i < 8u * p.map.P; i++) { v34_pt_t x; CHECK_EQ_I(v34_tx_in_b1(&tx), 1); v34_tx_symbol(&tx, &x, NULL); }
        { v34_pt_t x; v34_tx_symbol(&tx, &x, NULL); CHECK_EQ_I(v34_tx_in_b1(&tx), 0); }
    }

    /* ---- every output is a code sequence: all three trellises, a small,
     * a medium and a large constellation, without and with precoding (the
     * modulo encoder C0 is what keeps y = u + c on the code). Also the
     * identities of 9.6.2: u = y - c is a constellation point, and when
     * there is no precoding x = y = u. ---- */
    {
        static const int16_t hs[3][3][2] = {
            { { 0, 0 }, { 0, 0 }, { 0, 0 } },
            { { -9830, 3277 }, { 4096, -1638 }, { -819, 410 } },   /* -0.6+0.2j, 0.25-0.1j, -0.05+0.025j */
            { { 21299, -8192 }, { -13107, 9830 }, { 6554, 3277 } } };  /* 1.3-0.5j, -0.8+0.6j, 0.4+0.2j */
        static const unsigned rates[4] = { 4800, 12000, 21600, 31200 };
        for (int tr = 0; tr < 3; tr++)
            for (unsigned r = 0; r < 4; r++)
                for (unsigned hc = 0; hc < 3; hc++) {
                    v34_tx_t tx;
                    tu_src_t src = { 0x9E3779B9u + r, 0 };
                    unsigned off_grid = 0, differs = 0, maxy = 0;
                    setup(3200, rates[r], tr, (int)(r & 1), hs[hc]);
                    v34_tx_init(&tx, &p, tu_getbit, &src);
                    for (i = 0; i < NSYM; i++) {
                        v34_tx_symbol(&tx, &xs[i], &ys[i]);
                        if ((ys[i].re % V34_ONE) || (ys[i].im % V34_ONE) ||
                            !((ys[i].re / V34_ONE) & 1) || !((ys[i].im / V34_ONE) & 1)) off_grid++;
                        if (xs[i].re != ys[i].re || xs[i].im != ys[i].im) differs++;
                        if ((unsigned)abs(ys[i].re / V34_ONE) > maxy) maxy = (unsigned)abs(ys[i].re / V34_ONE);
                        if ((unsigned)abs(ys[i].im / V34_ONE) > maxy) maxy = (unsigned)abs(ys[i].im / V34_ONE);
                    }
                    CHECK_EQ_I(off_grid, 0);
                    CHECK_EQ_I(syndrome(ys, NSYM / 2, tr, 0, p.map.J, p.map.P), 0);
                    CHECK(maxy <= p.ymax);
                    if (hc == 0) CHECK_EQ_I(differs, 0);
                    else CHECK(differs > NSYM / 2);
                }
    }

    /* ---- rotational invariance: turning every point of a code sequence by
     * 90 degrees gives another code sequence (from some other encoder
     * state). This is the property the differential encoding relies on, and
     * for the 64-state code it is a check of the reading of Figure 12 with
     * its two AND gates: a misread connection would not be invariant. A
     * sequence that is NOT a code sequence (one point moved to a
     * neighbouring grid point) must fail from every state. ---- */
    for (int tr = 0; tr < 3; tr++) {
        v34_tx_t tx;
        tu_src_t src = { 0xC0FFEEu, 0 };
        unsigned st, ok_states = 0, ok_broken = 0, nst = 16u << tr;
        setup(3200, 19200, tr, 0, NULL);
        v34_tx_init(&tx, &p, tu_getbit, &src);
        for (i = 0; i < NSYM; i++) {
            v34_pt_t x;
            v34_tx_symbol(&tx, &x, NULL);
            ys[i].re = x.im; ys[i].im = (int16_t)-x.re;      /* 90 degrees clockwise */
        }
        for (st = 0; st < nst; st++)
            if (syndrome(ys, NSYM / 2, tr, st, p.map.J, p.map.P) == 0) ok_states++;
        CHECK_EQ_I(ok_states, 1);
        ys[1001].re = (int16_t)(ys[1001].re + 2 * V34_ONE);
        for (st = 0; st < nst; st++)
            if (syndrome(ys, NSYM / 2, tr, st, p.map.J, p.map.P) == 0) ok_broken++;
        CHECK_EQ_I(ok_broken, 0);
    }
    T_REPORT("test_trellis");
}
