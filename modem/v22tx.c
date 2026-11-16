/* V.22bis transmitter.
 *
 * Symbols (odd-integer points scaled by `gain`) are shaped by a 75 % root
 * raised cosine pulse held as 40 phases per symbol: one 600 baud symbol is
 * 40/3 samples at 8 kHz, so each output sample advances the pulse phase by 3.
 * The complex baseband is then moved to the carrier.
 */
#include "v22bis.h"
#include "v22_tabs.h"
#include "sintab.h"

void v22_encode(int *quadrant, int d, int e, int *x, int *y)
{
    /* Table 1: dibit -> quadrant advance (in units of +90 degrees) */
    static const int adv[4] = { 1 /*00*/, 0 /*01*/, 2 /*10*/, 3 /*11*/ };
    /* Figure 2, phase quadrant 1: element -> (x, y) */
    static const int ex[4] = { 1, 3, 1, 3 }, ey[4] = { 1, 1, 3, 3 };
    int q = (*quadrant + adv[d & 3]) & 3, px = ex[e & 3], py = ey[e & 3], t;
    *quadrant = q;
    while (q--) { t = px; px = -py; py = t; }    /* rotate +90 per quadrant */
    *x = px; *y = py;
}

void v22tx_init(v22tx_t *t, int fc, int level, v22_getbit_fn getbit, void *user)
{
    int i;
    t->mode = V22_TX_SILENCE;
    for (i = 0; i < 6; i++) t->si[i] = t->sq[i] = 0;
    t->acc = 40;
    t->cph = 0;
    t->dph = (uint32_t)(((uint64_t)(uint32_t)fc << 32) / 8000u);
    t->gain = (int32_t)(((int64_t)level << 12) / V22_TX_RMS_Q12);
    t->quadrant = 0; t->s1_flip = 0;
    scr_init(&t->scr);
    t->getbit = getbit; t->user = user;
    t->nsym = 0;
    t->fc = fc; t->level = level;
    t->gph = 0; t->gdph = 0; t->gamp = 0;
}

void v22tx_set_mode(v22tx_t *t, v22_txmode mode)
{
    if (mode != t->mode) { t->mode = mode; t->nsym = 0; }
}

static int sbit(v22tx_t *t, int data) { return v22_scramble(&t->scr, data ? t->getbit(t->user) & 1 : 1); }

static void next_symbol(v22tx_t *t)
{
    int d = 0, e = 1, x = 0, y = 0, i, data = 0;
    for (i = 5; i > 0; i--) { t->si[i] = t->si[i - 1]; t->sq[i] = t->sq[i - 1]; }
    switch (t->mode) {
    case V22_TX_SILENCE: t->si[0] = t->sq[0] = 0; t->nsym++; return;
    case V22_TX_USB1: d = 3; break;
    case V22_TX_S1: d = t->s1_flip ? 3 : 0; t->s1_flip ^= 1; break;
    case V22_TX_DATA_1200: data = 1; /* fall through */
    case V22_TX_SB1_1200:
        d = sbit(t, data) << 1; d |= sbit(t, data);
        break;
    case V22_TX_DATA_2400: data = 1; /* fall through */
    case V22_TX_SB1_2400:
        d = sbit(t, data) << 1; d |= sbit(t, data);
        e = sbit(t, data) << 1; e |= sbit(t, data);
        break;
    }
    v22_encode(&t->quadrant, d, e, &x, &y);
    t->si[0] = x * t->gain; t->sq[0] = y * t->gain;
    t->nsym++;
}

int16_t v22tx_tick(v22tx_t *t)
{
    int32_t bi = 0, bq = 0, c, s, v;
    int j;
    if (t->acc >= 40) { t->acc -= 40; next_symbol(t); }
    for (j = 0; j < 6; j++) {
        int32_t h = v22_txh[t->acc + 40 * j];
        bi += (t->si[j] * h) >> 6;
        bq += (t->sq[j] * h) >> 6;
    }
    bi >>= 8; bq >>= 8;                               /* Q14 pulse removed */
    s = sintab[(t->cph >> 22) & SIN_MASK];
    c = sintab[((t->cph >> 22) + SIN_N / 4) & SIN_MASK];
    v = (bi * c - bq * s) >> 15;
    if (t->gamp && t->mode != V22_TX_SILENCE) {
        v += (t->gamp * sintab[(t->gph >> 22) & SIN_MASK]) >> 15;
        t->gph += t->gdph;
    }
    t->cph += t->dph;
    t->acc += 3;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    return (int16_t)v;
}

/* Guard tone, high channel only (2.1). With the guard tone G dB below the
 * data signal and the total held at `level`:
 *   data RMS = level / sqrt(1 + 10^(-G/10)), tone RMS = data RMS * 10^(-G/20).
 * 1800 Hz, G = 6: data 0.8940, tone peak 0.6337 of level.
 *  550 Hz, G = 3: data 0.8162, tone peak 0.8172 of level. (Q15 below.) */
void v22tx_set_guard(v22tx_t *t, int hz)
{
    int32_t data_q15 = 32768, peak_q15 = 0;
    if (t->fc != V22_FC_HIGH) hz = 0;
    if (hz == 1800)     { data_q15 = 29295; peak_q15 = 20764; }
    else if (hz == 550) { data_q15 = 26745; peak_q15 = 26777; }
    else hz = 0;
    t->gain = (int32_t)((((int64_t)t->level * data_q15 >> 15) << 12) / V22_TX_RMS_Q12);
    t->gamp = (int32_t)((int64_t)t->level * peak_q15 >> 15);
    t->gdph = (uint32_t)(((uint64_t)(uint32_t)hz << 32) / 8000u);
    t->gph = 0;
}
