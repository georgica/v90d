/* V.92 training signal mappings. See v92_sig.h. */
#include <string.h>
#include "v92_sig.h"

/* x * c / 2^30, rounded to nearest, half away from zero */
static int32_t mul_q30(int32_t x, int32_t c)
{
    int64_t p = (int64_t)x * c;
    const int64_t h = (int64_t)1 << 29;
    return (int32_t)(p >= 0 ? (p + h) >> 30 : -((-p + h) >> 30));
}

#define Q30_SQRT_3_2     1315059792     /* sqrt(3/2) */
#define Q30_INV_SQRT_5    480191942     /* 1 / sqrt(5) */
#define Q30_INV_SQRT_21   234309675     /* 1 / sqrt(21) */

/* ---- two-point signals --------------------------------------------------- */

void v92_sgn_init(v92_sgn_t *s, int gpc)
{
    v90_scr_init(&s->scr);
    s->gpc = (uint8_t)(gpc != 0);
    s->last = 0;
}

void v92_sgn_scr_reset(v92_sgn_t *s)
{
    v90_scr_init(&s->scr);
}

static int sgn_scramble(v92_sgn_t *s, int bit)
{
    return s->gpc ? v90_scramble_gpc(&s->scr, bit) : v90_scramble_gpa(&s->scr, bit);
}

static int sgn_descramble(v92_sgn_t *s, int bit)
{
    return s->gpc ? v90_descramble_gpc(&s->scr, bit) : v90_descramble_gpa(&s->scr, bit);
}

int v92_sgn_tx_trn(v92_sgn_t *s)
{
    s->last = (uint8_t)sgn_scramble(s, 1);
    return s->last;
}

int v92_sgn_tx_bit(v92_sgn_t *s, int bit)
{
    s->last = (uint8_t)(sgn_scramble(s, bit & 1) ^ s->last);
    return s->last;
}

int v92_sgn_rx_trn(v92_sgn_t *s, int sbit)
{
    s->last = (uint8_t)(sbit & 1);
    return sgn_descramble(s, s->last);
}

int v92_sgn_rx_bit(v92_sgn_t *s, int sbit)
{
    int d = (sbit ^ s->last) & 1;
    s->last = (uint8_t)(sbit & 1);
    return sgn_descramble(s, d);
}

/* ---- Ru and Su ----------------------------------------------------------- */

int v92_ru_sbit(unsigned n, int bar)
{
    return (n % 6 >= 3) ^ (bar != 0);
}

int32_t v92_su_level(unsigned n, int bar, int32_t lu)
{
    static const int8_t pat[6] = { 1, 0, 1, -1, 0, -1 };
    int32_t a = mul_q30(lu, Q30_SQRT_3_2);
    int p = pat[n % 6];
    if (bar) p = -p;
    return p * a;
}

/* ---- TRN2u modulation ---------------------------------------------------- */

void v92_ml_init(v92_ml_t *s, int pt8, int last_sign)
{
    v90_scr_init(&s->scr);
    s->nbits = (uint8_t)(pt8 ? 3 : 2);
    s->msb_first = 0;
    s->last = (uint8_t)(last_sign & 1);
}

void v92_ml_set_msb_first(v92_ml_t *s, int on)
{
    s->msb_first = (uint8_t)(on != 0);
}

int v92_ml_tx(v92_ml_t *s, const uint8_t *bits)
{
    int b[3] = { 0, 0, 0 }, k, nb = s->nbits, sign, mag = 0, level;
    for (k = 0; k < nb; k++)
        b[k] = v90_scramble_gpa(&s->scr, bits ? bits[k] & 1 : 1);
    if (s->msb_first) {
        sign = b[0];
        for (k = 1; k < nb; k++) mag = (mag << 1) | b[k];
    } else {
        sign = b[nb - 1];
        for (k = nb - 2; k >= 0; k--) mag = (mag << 1) | b[k];
    }
    sign ^= s->last;
    s->last = (uint8_t)sign;
    level = 2 * mag + 1;
    return sign ? -level : level;
}

void v92_ml_rx(v92_ml_t *s, int level, uint8_t *bits)
{
    int b[3] = { 0, 0, 0 }, k, nb = s->nbits, sign = level < 0, mag, nmag = 1 << (nb - 1);
    mag = ((sign ? -level : level) - 1) / 2;
    if (mag < 0) mag = 0;
    if (mag >= nmag) mag = nmag - 1;
    k = sign ^ s->last;                     /* the sign bit as scrambled */
    s->last = (uint8_t)sign;
    if (s->msb_first) {
        b[0] = k;
        for (k = nb - 1; k >= 1; k--) { b[k] = mag & 1; mag >>= 1; }
    } else {
        b[nb - 1] = k;
        for (k = 0; k < nb - 1; k++) { b[k] = mag & 1; mag >>= 1; }
    }
    for (k = 0; k < nb; k++) bits[k] = (uint8_t)v90_descramble_gpa(&s->scr, b[k]);
}

int32_t v92_trn2u_level(int level, int pt8, int32_t lu)
{
    return mul_q30(level * lu, pt8 ? Q30_INV_SQRT_21 : Q30_INV_SQRT_5);
}

int v92_trn2u_slice(int32_t r, int pt8, int32_t lu)
{
    int max = pt8 ? 7 : 3, lev = 1, t;
    int32_t a = r < 0 ? (r == INT32_MIN ? INT32_MAX : -r) : r;
    /* decision thresholds lie at the even multiples of the unit */
    for (t = 2; t < max; t += 2)
        if (a >= v92_trn2u_level(t, pt8, lu)) lev = t + 1;
    return r < 0 ? -lev : lev;
}
