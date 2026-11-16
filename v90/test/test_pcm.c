/* V.90 Table 1 codeword tables, checked row by row against the table as
 * printed in the Recommendation (test/table1.inc). */
#include "test.h"
#include "v90_pcm.h"

static const struct { int u; int mu; int mulin; int a; int alin; } tab[] = {
#include "table1.inc"
};

int main(void)
{
    int n = (int)(sizeof tab / sizeof tab[0]);
    int i;
    CHECK_EQ_I(n, 128);
    for (i = 0; i < n; i++) {
        unsigned u = (unsigned)tab[i].u;
        CHECK_EQ_I(tab[i].u, i);
        /* positive codewords are the table octets */
        CHECK_EQ_I(v90_pcm_octet(0, u, 1), tab[i].mu);
        CHECK_EQ_I(v90_pcm_octet(1, u, 1), tab[i].a);
        /* the polarity bit is the MSB (V.90 3.6) */
        CHECK_EQ_I(v90_pcm_octet(0, u, 0), tab[i].mu & 0x7F);
        CHECK_EQ_I(v90_pcm_octet(1, u, 0), tab[i].a & 0x7F);
        CHECK_EQ_I(v90_ucode_linear(0, u), tab[i].mulin);
        CHECK_EQ_I(v90_ucode_linear(1, u), tab[i].alin);
        CHECK_EQ_I(v90_pcm_linear(0, u, 1), tab[i].mulin);
        CHECK_EQ_I(v90_pcm_linear(0, u, 0), -tab[i].mulin);
        CHECK_EQ_I(v90_pcm_linear(1, u, 1), tab[i].alin);
        CHECK_EQ_I(v90_pcm_linear(1, u, 0), -tab[i].alin);
        /* inverse */
        CHECK_EQ_I(v90_octet_ucode(0, (uint8_t)tab[i].mu), i);
        CHECK_EQ_I(v90_octet_ucode(0, (uint8_t)(tab[i].mu & 0x7F)), i);
        CHECK_EQ_I(v90_octet_ucode(1, (uint8_t)tab[i].a), i);
        CHECK_EQ_I(v90_octet_ucode(1, (uint8_t)(tab[i].a & 0x7F)), i);
        CHECK_EQ_I(v90_octet_sign((uint8_t)tab[i].mu), 1);
        CHECK_EQ_I(v90_octet_sign((uint8_t)(tab[i].a & 0x7F)), 0);
        CHECK_EQ_I(v90_octet_linear(0, (uint8_t)(tab[i].mu & 0x7F)), -tab[i].mulin);
        CHECK_EQ_I(v90_octet_linear(1, (uint8_t)tab[i].a), tab[i].alin);
    }
    /* a few rows typed in by hand from the printed table */
    CHECK_EQ_I(v90_pcm_octet(0, 0, 1), 0xFF);   CHECK_EQ_I(v90_pcm_octet(1, 0, 1), 0xD5);
    CHECK_EQ_I(v90_pcm_octet(0, 42, 1), 0xD5);  CHECK_EQ_I(v90_pcm_octet(1, 42, 1), 0xFF);
    CHECK_EQ_I(v90_ucode_linear(0, 42), 716);   CHECK_EQ_I(v90_ucode_linear(1, 42), 848);
    CHECK_EQ_I(v90_pcm_octet(0, 127, 1), 0x80); CHECK_EQ_I(v90_pcm_octet(1, 127, 1), 0xAA);
    CHECK_EQ_I(v90_ucode_linear(0, 127), 32124); CHECK_EQ_I(v90_ucode_linear(1, 127), 32256);
    CHECK_EQ_I(v90_ucode_linear(0, 16), 132);   CHECK_EQ_I(v90_ucode_linear(1, 15), 248);
    T_REPORT("test_pcm");
}
