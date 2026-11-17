#include "scramble.h"

void scr_init(scr_t *s) { s->reg = 0; s->ones = 0; s->invert = 0; }

/* reg bit 0 = most recent Ds, so Ds.x^-14 is bit 13 and Ds.x^-17 is bit 16 */
int v22_scramble(scr_t *s, int di)
{
    int ds;
    if (s->invert) { di ^= 1; s->invert = 0; }
    ds = (di ^ (int)(s->reg >> 13) ^ (int)(s->reg >> 16)) & 1;
    s->reg = (s->reg << 1) | (uint32_t)ds;
    if (ds) { if (++s->ones == 64) { s->invert = 1; s->ones = 0; } }
    else s->ones = 0;
    return ds;
}

int v22_descramble(scr_t *s, int ds)
{
    int dout = (ds ^ (int)(s->reg >> 13) ^ (int)(s->reg >> 16)) & 1;
    ds &= 1;
    if (s->invert) { dout ^= 1; s->invert = 0; }
    s->reg = (s->reg << 1) | (uint32_t)ds;
    if (ds) { if (++s->ones == 64) { s->invert = 1; s->ones = 0; } }
    else s->ones = 0;
    return dout;
}
