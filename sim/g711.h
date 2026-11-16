/* G.711 mu-law codec and V.90 Ucode table, integer only.
 *
 * Ucode numbering follows V.90 Table 1: a Ucode magnitude u (0..127) maps to a
 * linear value on the mu-law decision grid (16-bit full scale, max 32124).
 * A Ucode with positive sign is mu-law octet 0xFF-u, negative is 0x7F-u.
 *
 * Used by the channel simulator (sim) and by the modem's downstream encoder.
 */
#ifndef V90D_SIM_G711_H
#define V90D_SIM_G711_H
#include <stdint.h>

/* Linear value of a Ucode magnitude u (0..127), V.90 Table 1 scale. */
int32_t ucode_linear(unsigned u);

/* mu-law octet for Ucode magnitude u with given sign (positive != 0). */
uint8_t ucode_to_ulaw(unsigned u, int positive);

/* Decode a mu-law octet to its linear value (signed). */
int32_t ulaw_to_linear(uint8_t b);

/* Encode a linear value to the nearest mu-law octet. */
uint8_t linear_to_ulaw(int32_t v);

#endif
