/* V.34 clause 7 scrambler. See v90_scr.h. */
#include "v90_scr.h"

void v90_scr_init(v90_scr_t *s) { s->reg = 0; }

/* reg bit 0 is the previous scrambled bit, so x^-k is reg bit k-1 */
static int scr(v90_scr_t *s, int bit, int tap)
{
    int out = (bit ^ (int)(s->reg >> (tap - 1)) ^ (int)(s->reg >> 22)) & 1;
    s->reg = (s->reg << 1) | (uint32_t)out;
    return out;
}

static int descr(v90_scr_t *s, int bit, int tap)
{
    int out = (bit ^ (int)(s->reg >> (tap - 1)) ^ (int)(s->reg >> 22)) & 1;
    s->reg = (s->reg << 1) | (uint32_t)(bit & 1);
    return out;
}

int v90_scramble_gpc(v90_scr_t *s, int bit)   { return scr(s, bit, 18); }
int v90_descramble_gpc(v90_scr_t *s, int bit) { return descr(s, bit, 18); }
int v90_scramble_gpa(v90_scr_t *s, int bit)   { return scr(s, bit, 5); }
int v90_descramble_gpa(v90_scr_t *s, int bit) { return descr(s, bit, 5); }
