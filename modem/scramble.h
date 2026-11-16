/* Self-synchronising scrambler / descrambler, V.22bis clause 5:
 * generating polynomial 1 + x^-14 + x^-17, with the 64-consecutive-ones guard. */
#ifndef V90D_MODEM_SCRAMBLE_H
#define V90D_MODEM_SCRAMBLE_H
#include <stdint.h>

typedef struct { uint32_t reg; int ones; int invert; } scr_t;

void scr_init(scr_t *s);
int  v22_scramble(scr_t *s, int di);     /* returns Ds */
int  v22_descramble(scr_t *s, int ds);   /* returns Do */

#endif
