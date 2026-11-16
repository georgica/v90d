/* V.32bis coding against the Recommendation's tables and figures. */
#include <string.h>
#include "test.h"
#include "v32_priv.h"
#include "v32spec.h"

static const v32spec_row *spec_of(int rate)
{
    switch (rate) {
    case 0: return v32spec_4800;
    case 1: return v32spec_7200;
    case 2: return v32spec_9600;
    case 3: return v32spec_12000;
    default: return v32spec_14400;
    }
}

/* figure coordinates -> the modem's common scale */
static int scale_of(int rate) { return rate >= 3 ? 1 : 2; }

static void test_scrambler(void)
{
    static const char *bits[2] = { V32SPEC_GPC_BITS, V32SPEC_GPA_BITS };
    int ans, i;
    for (ans = 0; ans < 2; ans++) {
        v32_scr_t s = { 0 }, d = { 0 };
        uint32_t lfsr = 0x1234567u;
        for (i = 0; i < (int)strlen(bits[ans]); i++)
            CHECK_EQ_I(v32_scramble(&s, ans, 1), bits[ans][i] - '0');
        /* the descrambler undoes it, from any starting state after 23 bits */
        s.reg = 0x5a5a5; d.reg = 0;
        for (i = 0; i < 2000; i++) {
            int b, o;
            lfsr = lfsr * 1103515245u + 12345u; b = (lfsr >> 16) & 1;
            o = v32_descramble(&d, ans, v32_scramble(&s, ans, b));
            if (i >= 23) CHECK_EQ_I(o, b);
        }
    }
    /* the two directions differ */
    { v32_scr_t a = { 0 }, c = { 0 }; int diff = 0;
      for (i = 0; i < 64; i++) diff += v32_scramble(&a, 1, 1) != v32_scramble(&c, 0, 1);
      CHECK(diff > 10); }
}

static void test_tables(void)
{
    int rate, n, i;
    for (rate = 0; rate < V32_NRATES; rate++) {
        const v32spec_row *row = spec_of(rate);
        int nb = rate == 0 ? 2 : 3 + rate, seen = 0, sc = scale_of(rate);
        long pw = 0;
        for (; row->labels; row++) {
            const char *p = row->labels;
            int re = row->re0;
            while (*p) {
                int label = 0, x, y;
                for (i = 0; i < nb; i++) label = (label << 1) | (*p++ - '0');
                if (*p == ' ') p++;
                v32_point(rate, label, &x, &y);
                CHECK_EQ_I(x, re * sc);
                CHECK_EQ_I(y, row->im * sc);
                pw += x * x + y * y;
                re += row->step; seen++;
            }
        }
        CHECK_EQ_I(seen, 1 << nb);
        printf("  %5d bit/s: %3d points, mean power %.2f\n", V32_BITRATE(rate), seen, (double)pw / seen);
        /* all rates within 0.25 dB of the 4800 bit/s synchronising states (40) */
        CHECK_NEAR((double)pw / seen, 41.0, 1.01);
    }
    /* synchronising states: A B C D each a quarter turn on */
    { int x, y, px, py;
      v32_state_point(0, &px, &py); CHECK_EQ_I(px, -6); CHECK_EQ_I(py, -2);
      for (n = 1; n < 4; n++) { v32_state_point(n, &x, &y); CHECK_EQ_I(x, -py); CHECK_EQ_I(y, px); px = x; py = y; } }
}

/* the eight subsets Y0 Y1 Y2: points of one subset are 8 times the squared
 * minimum distance apart, the property the trellis code relies on */
static void test_subsets(void)
{
    int rate;
    for (rate = 1; rate < V32_NRATES; rate++) {
        int n = 1 << (3 + rate), i, j, dmin = 1 << 30, smin = 1 << 30, x, y, u, v;
        for (i = 0; i < n; i++) for (j = i + 1; j < n; j++) {
            int d;
            v32_point(rate, i, &x, &y); v32_point(rate, j, &u, &v);
            d = (x - u) * (x - u) + (y - v) * (y - v);
            if (d < dmin) dmin = d;
            if ((i >> rate) == (j >> rate) && d < smin) smin = d;
        }
        CHECK_EQ_I(smin, 8 * dmin);
    }
}

static void test_differential(void)
{
    int i, rate;
    /* Table 1, through the trellis rates' encoder */
    for (rate = 1; rate < V32_NRATES; rate++)
        for (i = 0; i < 16; i++) {
            const char *row = v32spec_table1[i];
            v32_enc_t e; int x, y, n = 2 + rate, label;
            e.y = (row[2] - '0') << 1 | (row[3] - '0'); e.st = 0;
            label = v32_encode(&e, rate, ((row[0] - '0') << 1 | (row[1] - '0')) << (n - 2), &x, &y);
            CHECK_EQ_I(e.y, (row[5] - '0') << 1 | (row[6] - '0'));
            CHECK_EQ_I((label >> (n - 2)) & 3, e.y);
        }
    /* Table 2 */
    for (i = 0; i < 16; i++) {
        const char *row = v32spec_table2[i];
        v32_enc_t e; int x, y, sx, sy;
        e.y = (row[2] - '0') << 1 | (row[3] - '0'); e.st = 0;
        v32_encode(&e, 0, (row[0] - '0') << 1 | (row[1] - '0'), &x, &y);
        CHECK_EQ_I(e.y, (row[5] - '0') << 1 | (row[6] - '0'));
        v32_state_point(row[8] - 'A', &sx, &sy);
        CHECK_EQ_I(x, sx); CHECK_EQ_I(y, sy);
    }
}

/* Figure 1 worked through gate by gate, independently of v32_trellis_next */
static int fig1(int a, int b, int c, int y1, int y2, int *na, int *nb, int *nc)
{
    int x_top = y1 ^ y2;              /* the adder under the Y1, Y2 rails */
    int x3 = b ^ y2;                  /* third adder of the chain */
    int and1 = c & x3;                /* left gate: Y0 and the third adder */
    int and2 = y1 & c;                /* right gate: Y1 and Y0 */
    int x1 = a ^ x_top, x2 = x1 ^ and1, x4 = x3 ^ and2;
    *na = c; *nb = x2; *nc = x4;
    return c;
}

static void test_trellis(void)
{
    int st, y, uniq[8], i;
    for (i = 0; i < 8; i++) uniq[i] = 0;
    for (st = 0; st < 8; st++)
        for (y = 0; y < 4; y++) {
            int na, nb, nc, ns = v32_trellis_next(st, y);
            fig1(st >> 2, (st >> 1) & 1, st & 1, y >> 1, y & 1, &na, &nb, &nc);
            CHECK_EQ_I(ns, na << 2 | nb << 1 | nc);
            uniq[ns]++;
        }
    for (i = 0; i < 8; i++) CHECK_EQ_I(uniq[i], 4);      /* four branches into every state */
    /* Y0 is the redundant bit: the label carries the state's c */
    { v32_enc_t e = { 0, 0 }; int x, y2, k; uint32_t l = 99;
      for (k = 0; k < 500; k++) {
          int c = e.st & 1, label;
          l = l * 1103515245u + 12345u;
          label = v32_encode(&e, 4, (l >> 16) & 63, &x, &y2);
          CHECK_EQ_I(label >> 6, c);
          CHECK_EQ_I(label & 15, (l >> 16) & 15);
      } }
}

static void test_rate_words(void)
{
    CHECK_EQ_I(v32_rate_word(0), 0x0991);
    CHECK_EQ_I(v32_rate_word(V32_RALL), 0x0FF9);
    CHECK_EQ_I(v32_rate_word(V32_R14400), 0x0999);
    CHECK_EQ_I(v32_rate_word(V32_R4800), 0x0D91);
    CHECK_EQ_I(v32_rate_word(V32_R9600), 0x0B91);
    CHECK_EQ_I(v32_rate_word(V32_R7200), 0x09D1);
    CHECK_EQ_I(v32_rate_word(V32_R12000), 0x09B1);
    CHECK_EQ_I(v32_word_rates(0x0FF9), V32_RALL);
    CHECK_EQ_I(v32_rate_word(0) & V32_RW_SYNC, 0x0111);
}

int main(void)
{
    test_scrambler();
    test_tables();
    test_subsets();
    test_differential();
    test_trellis();
    test_rate_words();
    T_REPORT("test_code");
}
