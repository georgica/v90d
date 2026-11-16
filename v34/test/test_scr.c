/* V.34 clause 7 scrambler / descrambler. Expected values derived by hand
 * from the polynomials: scrambler out(n) = in(n) ^ out(n-a) ^ out(n-23),
 * descrambler out(n) = in(n) ^ in(n-a) ^ in(n-23), a = 18 (GPC) or 5 (GPA). */
#include "test.h"
#include "testutil.h"

int main(void)
{
    v34_scr_t s, d;
    int i;

    /* GPA, ones in, zero state: 11111 00000 11111 00000 111 then
     * n=23: 1^out(18)^out(0) = 1^0^1 = 0; n=24: 1^0^1 = 0; n=25: 1^1^1 = 1 */
    {
        static const char want[] = "11111000001111100000111001";
        v34_scr_init(&s, V34_GPA);
        for (i = 0; want[i]; i++)
            CHECK_EQ_I(v34_scramble(&s, 1), want[i] - '0');
    }
    /* GPC, ones in: 18 ones, 5 zeros (n=18..22: 1^1^0), ones for n=23..35
     * (1^out(n-18)^out(n-23) = 1^1^1), n=36: 1^out(18)^out(13) = 1^0^1 = 0 */
    {
        v34_scr_init(&s, V34_GPC);
        for (i = 0; i < 37; i++) {
            int want = (i < 18) ? 1 : (i < 23) ? 0 : (i < 36) ? 1 : 0;
            CHECK_EQ_I(v34_scramble(&s, 1), want);
        }
    }
    /* descrambler impulse response is the polynomial itself */
    for (int poly = 0; poly < 2; poly++) {
        int a = poly == V34_GPA ? 5 : 18;
        v34_scr_init(&d, poly);
        for (i = 0; i < 60; i++)
            CHECK_EQ_I(v34_descramble(&d, i == 0), (i == 0 || i == a || i == 23));
    }
    /* scrambler impulse response, GPA: out(n) = out(n-5) ^ out(n-23) */
    {
        int o[100];
        v34_scr_init(&s, V34_GPA);
        for (i = 0; i < 100; i++) {
            o[i] = v34_scramble(&s, i == 0);
            int want = (i == 0) ^ (i >= 5 ? o[i - 5] : 0) ^ (i >= 23 ? o[i - 23] : 0);
            CHECK_EQ_I(o[i], want);
        }
        CHECK_EQ_I(o[5], 1); CHECK_EQ_I(o[18], 0); CHECK_EQ_I(o[23], 1);
    }
    /* round trip, and self-synchronisation after 23 bits from any state */
    for (int poly = 0; poly < 2; poly++) {
        int errs = 0, late = 0;
        v34_scr_init(&s, poly);
        v34_scr_init(&d, poly);
        for (i = 0; i < 5000; i++) {
            int b = tu_rand() & 1;
            if (v34_descramble(&d, v34_scramble(&s, b)) != b) errs++;
        }
        CHECK_EQ_I(errs, 0);
        d.sr = 0x2A5A5A;                 /* corrupt the descrambler */
        for (i = 0; i < 5000; i++) {
            int b = tu_rand() & 1;
            if (v34_descramble(&d, v34_scramble(&s, b)) != b && i >= 23) late++;
        }
        CHECK_EQ_I(late, 0);
    }
    /* the two polynomials differ */
    {
        int diff = 0;
        v34_scr_init(&s, V34_GPA); v34_scr_init(&d, V34_GPC);
        for (i = 0; i < 64; i++) diff += v34_scramble(&s, 1) != v34_scramble(&d, 1);
        CHECK(diff > 0);
    }
    T_REPORT("test_scr");
}
