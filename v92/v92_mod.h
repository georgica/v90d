/* V.92 6.4.1 modulus encoder and its inverse.
 *
 * The encoder turns the K scrambled bits of a data frame into twelve numbers
 * K0..K11, 0 <= Ki < Mi, by mixed-radix conversion of R0, where R0 is the
 * bits read as an integer R (b0 = LSB) or its complement M - 1 - R, chosen
 * by a differentially encoded "sign" so that a polarity inversion of the
 * line (every index eta becoming -eta - 1, hence every Ki becoming
 * Mi - 1 - Ki and R0 becoming M - 1 - R0) does not change the decoded bits.
 *
 * One v92_mod_t serves either an encoder or a decoder. M is at most
 * 255^12 < 2^96, held in three 32-bit limbs, least significant first.
 */
#ifndef V90D_V92_MOD_H
#define V90D_V92_MOD_H
#include "v92.h"

typedef struct {
    uint8_t  K;
    uint8_t  M[V92_DFRAME];
    uint8_t  odd;         /* M is odd: R0 = (M - 1) / 2 is its own complement */
    uint8_t  d;           /* d(f - 1) */
    uint32_t m1[3];       /* M - 1 */
    uint32_t half[3];     /* floor((M - 1) / 2) */
} v92_mod_t;

/* Returns 0, or V92_ERR_RANGE if K is not 1..72, a modulus is 0 or
 * 2^K > M. d(f - 1) starts at 0 (8.7.1: memories zero before B1u). */
int  v92_mod_init(v92_mod_t *s, const uint8_t M[V92_DFRAME], int K);
void v92_mod_reset(v92_mod_t *s);

/* bits[0..K-1], one bit per byte, b0 (first in time) first -> k[0..11]. */
void v92_mod_encode(v92_mod_t *s, const uint8_t *bits, uint8_t k[V92_DFRAME]);

/* k[0..11] -> bits[0..K-1]. Returns 0 if the frame is a possible encoder
 * output, 1 if not (some Ki >= Mi, or R >= 2^K: an uncorrected error or lost
 * frame alignment); bits then hold the low K bits of what came out.
 *
 * The decoder takes d(f - 1) from the sign of the previous frame's R0, as
 * the encoder does, so it follows the encoder from the common zero start,
 * recovers by itself after an error, and is indifferent to a polarity
 * inversion from the second frame on. */
int  v92_mod_decode(v92_mod_t *s, const uint8_t k[V92_DFRAME], uint8_t *bits);

#endif
