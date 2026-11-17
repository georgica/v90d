/* V.34 Phase 3 / 4 sequence contents (V.34 10.1.3.3, 10.1.3.4, 10.1.3.9):
 * the J and J' bit patterns (Tables 18 and 19), the Modulation Parameter
 * sequences MP / MP' Type 0 and Type 1 (Tables 20 and 21) with their CRC,
 * and the data signalling rate decision of 11.4.1.1.3 / 11.4.1.2.3.
 *
 * Bits are handled one per byte (0 / 1), element 0 first in time, as in
 * modem/info.h. Integer only, no allocation. */
#ifndef V90D_MODEM_V34MP_H
#define V90D_MODEM_V34MP_H
#include <stdint.h>

/* Tables 18 and 19: 16 bits, element 0 first in time */
extern const uint8_t v34_j4[16];        /* J asking for 4-point TRN / MP / E */
extern const uint8_t v34_j16[16];       /* J asking for 16-point TRN / MP / E */
extern const uint8_t v34_jprime[16];    /* J' */

#define V34_MP0_BITS 88                 /* Type 0, fill bits 85:87 included */
#define V34_MP1_BITS 188                /* Type 1, fill bit 187 included */
#define V34_MP_MAX_BITS V34_MP1_BITS

typedef struct {
    uint8_t  type;          /* bit 18: 0, or 1 = carries precoder coefficients */
    uint8_t  max_ca;        /* bits 20:23: maximum call -> answer rate, N * 2400 (0 = cleardown, 11.7) */
    uint8_t  max_ac;        /* bits 24:27: maximum answer -> call rate, N * 2400 */
    uint8_t  aux;           /* bit 28: auxiliary channel */
    uint8_t  trellis;       /* bits 29:30: 0 = 16, 1 = 32, 2 = 64 states, for the REMOTE transmitter */
    uint8_t  nonlinear;     /* bit 31: non-linear encoder of the remote transmitter */
    uint8_t  shaping;       /* bit 32: 0 = minimum, 1 = expanded, remote transmitter */
    uint8_t  ack;           /* bit 33: 1 makes this MP' */
    uint16_t rates;         /* bits 35:48: bit 0 = 2400, bit 1 = 4800 ... bit 13 = 33600 */
    uint8_t  asym;          /* bit 50: asymmetric data signalling rates enabled */
    int16_t  h[3][2];       /* Type 1: h(1..3), [0] real, [1] imaginary, 2.14 format */
} v34_mp_t;

/* Build one sequence, frame sync first. Returns its length (88 or 188), or
 * -1 when a field is out of range. */
int v34_mp_build(const v34_mp_t *mp, uint8_t *bits);

#define V34_MP_SHORT  0     /* more bits are needed */
#define V34_MP_BAD   (-1)   /* frame sync, a start bit or the CRC is wrong */
/* bits[0..n) starting at the first bit of the frame sync. Returns the
 * length of the sequence when it is complete and correct. */
int v34_mp_parse(const uint8_t *bits, int n, v34_mp_t *mp);

/* 11.4.1.2.3: the data signalling rates in bit/s from the two MP sequences:
 * *ac answer -> call, *ca call -> answer; 0 when there is none in common. */
void v34_mp_rates(const v34_mp_t *ans, const v34_mp_t *call, int *ac, int *ca);

/* 1 when both sequences ask for zero in both directions (cleardown, 11.7) */
int v34_mp_is_cleardown(const v34_mp_t *mp);

#endif
