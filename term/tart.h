/* v90d/term - the art of the terminal menu's opening screen, in half-block
 * pixels (tscr_dot): the planet with its ring and the NETSTALGIA wordmark,
 * as Netstalgia's approved mock-up draws them. Call tscr_flush() after.
 * Portable C99, no allocation, no globals. Ours; written for this project. */
#ifndef V90D_TART_H
#define V90D_TART_H
#include "tscr.h"

/* a planet of radius r pixels at (cx, cy), its ring in front and behind */
void tart_planet(tscr *s, int cx, int cy, int r);
/* the wordmark "NETSTALGIA" from pixel (x, y), 7 pixels high with a shadow;
 * returns its width in pixels */
int  tart_wordmark(tscr *s, int x, int y);

#endif
