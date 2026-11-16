/* V.32bis coding: scramblers (4), differential and trellis encoding and
 * signal mapping (2.3), rate sequences (5.3). */
#include "v32_priv.h"

/* reg bit 0 = the most recent scrambled bit, so x^-n is bit n-1 */
int v32_scramble(v32_scr_t *s, int answer, int di)
{
    int ds = (di ^ (int)(s->reg >> (answer ? 4 : 17)) ^ (int)(s->reg >> 22)) & 1;
    s->reg = (s->reg << 1) | (uint32_t)ds;
    return ds;
}

int v32_descramble(v32_scr_t *s, int answer, int ds)
{
    int dout = (ds ^ (int)(s->reg >> (answer ? 4 : 17)) ^ (int)(s->reg >> 22)) & 1;
    s->reg = (s->reg << 1) | (uint32_t)(ds & 1);
    return dout;
}

/* Figure 2-5: A 00, B 01, C 11, D 10 */
const uint8_t v32_abcd_label[4] = { 0, 1, 3, 2 };
const uint8_t v32_label_abcd[4] = { 0, 1, 3, 2 };

const int8_t (*v32_constel(int rate))[2]
{
    switch (rate) {
    case 0:  return v32_c4800;
    case 1:  return v32_c7200;
    case 2:  return v32_c9600;
    case 3:  return v32_c12000;
    default: return v32_c14400;
    }
}

void v32_point(int rate, int label, int *x, int *y)
{
    const int8_t (*c)[2] = v32_constel(rate);
    *x = c[label][0]; *y = c[label][1];
}

void v32_state_point(int abcd, int *x, int *y)
{
    v32_point(0, v32_abcd_label[abcd & 3], x, y);
}

/* Figure 1. Delay elements a, b, c from left to right, c = Y0:
 *   a' = c
 *   b' = a + Y1 + Y2 + c.(b + Y2)
 *   c' = b + Y2 + Y1.c                       (+ is exclusive-or, . is and) */
int v32_trellis_next(int st, int y12)
{
    int a = (st >> 2) & 1, b = (st >> 1) & 1, c = st & 1;
    int y1 = (y12 >> 1) & 1, y2 = y12 & 1;
    int p = b ^ y2;
    int nb = a ^ y1 ^ y2 ^ (c & p);
    int nc = p ^ (y1 & c);
    return (c << 2) | (nb << 1) | nc;
}

int v32_encode(v32_enc_t *e, int rate, int q, int *x, int *y)
{
    int n = 2 + rate, q12 = (q >> (n - 2)) & 3, rest = q & ((1 << (n - 2)) - 1);
    int label;
    if (rate == 0) {
        /* Table 2: phase quadrant change +90, 0, +180, +270 for 00, 01, 10, 11 */
        static const int adv[4] = { 1, 0, 2, 3 };
        int s = (v32_label_abcd[e->y] + adv[q12]) & 3;
        e->y = v32_abcd_label[s];
        label = e->y;
    } else {
        /* Table 1: with v = Y1 + 2.Y2, v[n] = v[n-1] + Q1 + 2.Q2 modulo 4 */
        int v = ((e->y >> 1) | ((e->y & 1) << 1)) + ((q12 >> 1) | ((q12 & 1) << 1));
        int y0 = e->st & 1;
        v &= 3;
        e->y = ((v & 1) << 1) | (v >> 1);
        label = (y0 << n) | (e->y << (n - 2)) | rest;
        e->st = v32_trellis_next(e->st, e->y);
    }
    v32_point(rate, label, x, y);
    return label;
}

uint16_t v32_rate_word(unsigned rates)
{
    uint16_t w = V32_RW_BASE;
    if (rates & V32_R4800)  w |= 1u << 10;      /* B5 */
    if (rates & V32_R9600)  w |= 1u << 9;       /* B6 */
    if (rates & V32_R7200)  w |= 1u << 6;       /* B9 */
    if (rates & V32_R12000) w |= 1u << 5;       /* B10 */
    if (rates & V32_R14400) w |= 1u << 3;       /* B12 */
    return w;
}

unsigned v32_word_rates(uint16_t w)
{
    unsigned r = 0;
    if (w & (1u << 10)) r |= V32_R4800;
    if (w & (1u << 9))  r |= V32_R9600;
    if (w & (1u << 6))  r |= V32_R7200;
    if (w & (1u << 5))  r |= V32_R12000;
    if (w & (1u << 3))  r |= V32_R14400;
    return r;
}

uint32_t v32_isqrt64(uint64_t v)
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return (uint32_t)r;
}
