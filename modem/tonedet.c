/* Single tone watch. See tonedet.h. */
#include <string.h>
#include "tonedet.h"
#include "sintab.h"

void tonedet_init(tonedet_t *t, uint32_t dph)
{
    memset(t, 0, sizeof *t);
    t->dph = dph;
}

int tonedet_tick(tonedet_t *t, int16_t x)
{
    int32_t c = sintab[((t->ph >> 22) + SIN_N / 4) & SIN_MASK], s = sintab[(t->ph >> 22) & SIN_MASK];
    t->ph += t->dph;
    t->re += (x * c) >> 15; t->im += (x * s) >> 15;
    t->pw += (int32_t)x * x;
    if (++t->n < TONEDET_BLOCK) return 0;
    {
        int64_t m = t->re * t->re + t->im * t->im;
        int on = t->pw > (int64_t)TONEDET_BLOCK * 400 && m * 4 > t->pw * TONEDET_BLOCK;     /* more than half */
        t->blocks = on ? t->blocks + 1 : 0;
        t->n = 0; t->re = t->im = t->pw = 0;
    }
    return t->blocks >= TONEDET_BLOCKS;
}
