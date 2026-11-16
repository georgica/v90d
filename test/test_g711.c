/* Tests for the G.711 mu-law codec / V.90 Ucode table. */
#include "test.h"
#include "../sim/g711.h"

int main(void)
{
    /* V.90 Table 1 anchor values (derived from the mu-law decision grid). */
    CHECK_EQ_I(ucode_linear(0), 0);       /* smallest step */
    CHECK_EQ_I(ucode_linear(1), 8);
    CHECK_EQ_I(ucode_linear(15), 120);    /* top of segment 0 */
    CHECK_EQ_I(ucode_linear(16), 132);    /* start of segment 1 */
    CHECK_EQ_I(ucode_linear(127), 32124); /* mu-law maximum magnitude */

    /* ucode magnitudes are monotonically increasing */
    {
        unsigned u;
        int ok = 1;
        for (u = 1; u < 128; u++)
            if (ucode_linear(u) <= ucode_linear(u - 1)) ok = 0;
        CHECK(ok);
    }

    /* sign/octet mapping (V.90): u=0 +/- -> 0xFF / 0x7F; u=127 + -> 0x80 */
    CHECK_EQ_I(ucode_to_ulaw(0, 1), 0xFF);
    CHECK_EQ_I(ucode_to_ulaw(0, 0), 0x7F);
    CHECK_EQ_I(ucode_to_ulaw(127, 1), 0x80);
    CHECK_EQ_I(ucode_to_ulaw(127, 0), 0x00);

    /* decode matches the Ucode grid, with sign */
    CHECK_EQ_I(ulaw_to_linear(0xFF), 0);        /* +0 */
    CHECK_EQ_I(ulaw_to_linear(0x7F), 0);        /* -0 */
    CHECK_EQ_I(ulaw_to_linear(0x80), 32124);    /* +max */
    CHECK_EQ_I(ulaw_to_linear(0x00), -32124);   /* -max */

    /* every octet round-trips except the -0 alias: mu-law has two zero codes
     * (0xFF=+0, 0x7F=-0) that both decode to 0, so -0 re-encodes to +0. */
    {
        int b, ok = 1;
        for (b = 0; b < 256; b++) {
            if ((uint8_t)b == 0x7F) continue; /* -0 aliases to +0 */
            if (linear_to_ulaw(ulaw_to_linear((uint8_t)b)) != (uint8_t)b) ok = 0;
        }
        CHECK(ok);
    }
    CHECK_EQ_I(linear_to_ulaw(ulaw_to_linear(0x7F)), 0xFF); /* -0 -> +0 */

    /* encode clamps and snaps to grid: full-scale in -> max code */
    CHECK_EQ_I(linear_to_ulaw(100000), 0x80);
    CHECK_EQ_I(linear_to_ulaw(-100000), 0x00);
    CHECK_EQ_I(linear_to_ulaw(0), 0xFF);

    T_REPORT("g711");
}
