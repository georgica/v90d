/* V.34 clause 7 scrambler. The expected sequences are worked out by hand from
 * the polynomials (see the comments), not produced by the code under test. */
#include "test.h"
#include "v90_scr.h"

/* run-length description of a bit string starting with `first` */
static int expand(const int *runs, int nruns, int first, int *out)
{
    int n = 0, i, k, b = first;
    for (i = 0; i < nruns; i++) { for (k = 0; k < runs[i]; k++) out[n++] = b; b ^= 1; }
    return n;
}

static uint32_t lcg(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 16; }

int main(void)
{
    v90_scr_t s;
    int exp[128], n, i;

    /* GPC, all-ones input from the zero state:
     *   out[n] = 1 ^ out[n-18] ^ out[n-23]
     *   n = 0..17   no history yet                       -> 1
     *   n = 18..22  1 ^ out[0..4]=1                      -> 0
     *   n = 23..35  1 ^ out[5..17]=1 ^ out[0..12]=1      -> 1
     *   n = 36..40  1 ^ out[18..22]=0 ^ out[13..17]=1    -> 0
     *   n = 41..45  1 ^ out[23..27]=1 ^ out[18..22]=0    -> 0
     *   n = 46..53  1 ^ out[28..35]=1 ^ out[23..30]=1    -> 1
     *   n = 54..58  1 ^ out[36..40]=0 ^ out[31..35]=1    -> 0
     *   n = 59..63  1 ^ out[41..45]=0 ^ out[36..40]=0    -> 1
     *   n = 64..68  1 ^ out[46..50]=1 ^ out[41..45]=0    -> 0
     *   n = 69..71  1 ^ out[51..53]=1 ^ out[46..48]=1    -> 1
     *   n = 72..76  1 ^ out[54..58]=0 ^ out[49..53]=1    -> 0
     *   n = 77..81  1 ^ out[59..63]=1 ^ out[54..58]=0    -> 0 */
    {
        static const int runs[] = { 18, 5, 13, 10, 8, 5, 5, 5, 3, 10 };
        n = expand(runs, 10, 1, exp);
        CHECK_EQ_I(n, 82);
        v90_scr_init(&s);
        for (i = 0; i < n; i++) CHECK_EQ_I(v90_scramble_gpc(&s, 1), exp[i]);
    }

    /* GPC impulse response = 1 / (1 + x^18 + x^23) over GF(2)
     *   = sum_k (x^18 + x^23)^k
     *   k=1: 18, 23   k=2: 36, 46   k=3: 54, 59, 64, 69   k=4: 72, 92
     *   k=5: 90, ...
     * so for n < 90 the ones are at 0,18,23,36,46,54,59,64,69,72. */
    {
        static const int ones[] = { 0, 18, 23, 36, 46, 54, 59, 64, 69, 72 };
        int h[90];
        for (i = 0; i < 90; i++) h[i] = 0;
        for (i = 0; i < 10; i++) h[ones[i]] = 1;
        v90_scr_init(&s);
        for (i = 0; i < 90; i++) CHECK_EQ_I(v90_scramble_gpc(&s, i == 0), h[i]);
    }

    /* GPA, all-ones input from the zero state:
     *   out[n] = 1 ^ out[n-5] ^ out[n-23]
     *   0..4 -> 1, 5..9 -> 0, 10..14 -> 1, 15..19 -> 0, 20..22 -> 1,
     *   23: 1^out[18]=0^out[0]=1 -> 0     24: 1^out[19]=0^out[1]=1 -> 0
     *   25: 1^out[20]=1^out[2]=1 -> 1     26, 27 likewise -> 1
     *   28: 1^out[23]=0^out[5]=0 -> 1     29: 1^out[24]=0^out[6]=0 -> 1
     *   30: 1^out[25]=1^out[7]=0 -> 0     31, 32: likewise -> 0
     *   33: 1^out[28]=1^out[10]=1 -> 1    34 -> 1
     *   35: 1^out[30]=0^out[12]=1 -> 0    36: 1^out[31]=0^out[13]=1 -> 0
     *   37: 1^out[32]=0^out[14]=1 -> 0    38: 1^out[33]=1^out[15]=0 -> 0
     *   39: 1^out[34]=1^out[16]=0 -> 0 */
    {
        static const int runs[] = { 5, 5, 5, 5, 3, 2, 5, 3, 2, 5 };
        n = expand(runs, 10, 1, exp);
        CHECK_EQ_I(n, 40);
        v90_scr_init(&s);
        for (i = 0; i < n; i++) CHECK_EQ_I(v90_scramble_gpa(&s, 1), exp[i]);
    }

    /* GPA impulse response = sum_k (x^5 + x^23)^k; for n < 50 the ones are at
     *   k=1: 5, 23  k=2: 10, 46  k=3: 15, 33 (51, 69)  k=4: 20  k=5: 25, 43
     *   k=6: 30  k=7: 35  k=8: 40  k=9: 45 */
    {
        static const int ones[] = { 0, 5, 10, 15, 20, 23, 25, 30, 33, 35, 40, 43, 45, 46 };
        int h[50];
        for (i = 0; i < 50; i++) h[i] = 0;
        for (i = 0; i < 14; i++) h[ones[i]] = 1;
        v90_scr_init(&s);
        for (i = 0; i < 50; i++) CHECK_EQ_I(v90_scramble_gpa(&s, i == 0), h[i]);
    }

    /* descrambler inverts the scrambler, and self-synchronises within 23 bits
     * when started from a different state */
    {
        v90_scr_t tx, rx, rx2;
        uint32_t seed = 12345;
        int bad = 0, bad2 = 0, pass;
        for (pass = 0; pass < 2; pass++) {
            v90_scr_init(&tx); v90_scr_init(&rx);
            rx2.reg = 0x5A5A5A5Au;
            bad = bad2 = 0;
            for (i = 0; i < 5000; i++) {
                int d = (int)(lcg(&seed) & 1), c, r, r2;
                if (pass == 0) {
                    c = v90_scramble_gpc(&tx, d);
                    r = v90_descramble_gpc(&rx, c); r2 = v90_descramble_gpc(&rx2, c);
                } else {
                    c = v90_scramble_gpa(&tx, d);
                    r = v90_descramble_gpa(&rx, c); r2 = v90_descramble_gpa(&rx2, c);
                }
                if (r != d) bad++;
                if (i >= 23 && r2 != d) bad2++;
            }
            CHECK_EQ_I(bad, 0);
            CHECK_EQ_I(bad2, 0);
        }
    }
    /* the two polynomials differ */
    {
        v90_scr_t a, b; int diff = 0;
        v90_scr_init(&a); v90_scr_init(&b);
        for (i = 0; i < 64; i++) diff += v90_scramble_gpc(&a, 1) != v90_scramble_gpa(&b, 1);
        CHECK(diff > 0);
    }
    T_REPORT("test_scr");
}
