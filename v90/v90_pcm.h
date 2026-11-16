/* V.90 Table 1: the universal set of PCM codewords.
 *
 * A Ucode (0..127) names one positive PCM codeword of either law. The octet
 * columns of Table 1 are the positive codewords (polarity bit, the MSB, set);
 * the negative codeword of the same Ucode has the MSB cleared (V.90 3.6).
 * "Linear" is the Table 1 linear value on the 16-bit scale, max 32124 for
 * mu-law and 32256 for A-law.
 *
 * Throughout this library a sign bit of 1 means a positive voltage and 0 a
 * negative voltage (V.90 5.4.6), and `alaw` is 0 for mu-law, nonzero for
 * A-law. All functions are pure and integer only; there are no stored tables.
 */
#ifndef V90D_V90_PCM_H
#define V90D_V90_PCM_H
#include <stdint.h>

/* PCM octet to put on the digital interface for a Ucode and sign. */
uint8_t v90_pcm_octet(int alaw, unsigned ucode, int positive);

/* Table 1 linear value of a Ucode magnitude (always >= 0). */
int32_t v90_ucode_linear(int alaw, unsigned ucode);

/* Signed linear value of a Ucode with sign (negative of the above when
 * positive == 0; mu-law Ucode 0 is 0 for both signs). */
int32_t v90_pcm_linear(int alaw, unsigned ucode, int positive);

/* Inverse mapping of an octet. */
unsigned v90_octet_ucode(int alaw, uint8_t octet);   /* 0..127 */
int      v90_octet_sign(uint8_t octet);              /* 1 = positive */
int32_t  v90_octet_linear(int alaw, uint8_t octet);  /* signed linear */

#endif
