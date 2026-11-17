/* V.32bis transmitter: the line signals of 5.2, 5.3 and 2.3 as symbols, then
 * a root raised cosine pulse and the 1800 Hz carrier in one pass-band
 * polyphase filter. 2400 symbols/s at 8000 samples/s is 10/3 samples per
 * symbol: the pulse is held at 10 phases a third of a sample apart and each
 * sample advances the phase by 3. The carrier turns 3/4 of a cycle per
 * symbol, so it is applied to the symbols as a rotation by quarter turns
 * (the echo canceller works on the same rotated symbols). */
#include "v32_priv.h"

void v32tx_init(v32tx_t *t, int answer, int level, v32_getbit_fn getbit, void *user)
{
    int i;
    t->answer = answer;
    t->mode = V32_TX_SILENCE;
    for (i = 0; i < V32_TXL + V32_TXD; i++) t->br[i] = t->bi[i] = 0;
    t->acc = 10; t->rot = 0; t->ref = 0; t->newsym = 0;
    t->gain = (int32_t)(((int64_t)level << 12) / v32_tx_rms_q12);
    t->scr.reg = 0;
    t->enc.y = 0; t->enc.st = 0;
    t->rate = 0; t->rword = 0; t->rbit = 0; t->abcd = 0;
    t->nsym = 0;
    t->getbit = getbit; t->user = user;
}

int v32tx_symbol_due(const v32tx_t *t) { return t->acc >= 10; }

void v32tx_set_mode(v32tx_t *t, v32_txmode mode)
{
    if (mode != t->mode) {
        /* scrambled ones run straight on into data: same encoder, same count */
        if (!(mode == V32_TX_DATA && t->mode == V32_TX_ONES)) t->nsym = 0;
        t->mode = mode;
    }
}

static int sbit(v32tx_t *t, int b) { return v32_scramble(&t->scr, t->answer, b); }

static void next_symbol(v32tx_t *t)
{
    int x = 0, y = 0, i, q, n, s = -1, tmp;
    for (i = V32_TXL + V32_TXD - 1; i > 0; i--) { t->br[i] = t->br[i - 1]; t->bi[i] = t->bi[i - 1]; }
    switch (t->mode) {
    case V32_TX_SILENCE: break;
    case V32_TX_AC:   s = (t->nsym & 1) ? 2 : 0; break;
    case V32_TX_CA:   s = (t->nsym & 1) ? 0 : 2; break;
    case V32_TX_AA:   s = 0; break;
    case V32_TX_CC:   s = 2; break;
    case V32_TX_S:    s = (t->nsym & 1) ? 1 : 0; break;
    case V32_TX_SBAR: s = (t->nsym & 1) ? 3 : 2; break;
    case V32_TX_TRN:
        /* 5.2.3: scrambler from zero, ones in; 256 symbols of A / C from the
         * first bit of each dibit, then Table 4 */
        if (t->nsym == 0) t->scr.reg = 0;
        q = sbit(t, 1) << 1; q |= sbit(t, 1);
        s = t->nsym < 256 ? ((q & 2) ? 2 : 0) : v32_label_abcd[q];
        break;
    case V32_TX_RATE:
        /* 5.3: 16-bit sequence, scrambled, differentially encoded from the last state */
        if (t->nsym == 0) t->rbit = 0;
        q = sbit(t, (t->rword >> (15 - t->rbit)) & 1) << 1;
        q |= sbit(t, (t->rword >> (14 - t->rbit)) & 1);
        t->rbit = (t->rbit + 2) & 15;
        t->enc.y = v32_abcd_label[t->abcd];
        v32_encode(&t->enc, 0, q, &x, &y);
        s = v32_label_abcd[t->enc.y];
        break;
    case V32_TX_ONES:
    case V32_TX_DATA:
        if (t->nsym == 0) { t->enc.st = 0; t->enc.y = v32_abcd_label[t->abcd]; }
        n = 2 + t->rate;
        for (q = 0, i = 0; i < n; i++)
            q = (q << 1) | sbit(t, t->mode == V32_TX_DATA ? (t->getbit(t->user) & 1) : 1);
        v32_encode(&t->enc, t->rate, q, &x, &y);
        if (t->rate == 0) t->abcd = v32_label_abcd[t->enc.y];
        break;
    }
    if (s >= 0) { t->abcd = s; v32_state_point(s, &x, &y); }
    /* carrier: (-j)^k */
    for (i = 0; i < t->rot; i++) { tmp = x; x = y; y = -tmp; }
    t->rot = (t->rot + 1) & 3;
    t->br[0] = (int8_t)x; t->bi[0] = (int8_t)y;
    t->nsym++;
}

int16_t v32tx_tick(v32tx_t *t)
{
    const int16_t (*h)[2], (*w)[2];
    int32_t acc = 0, ref = 0;
    int64_t v;
    int j;
    t->newsym = 0;
    if (t->acc >= 10) { t->acc -= 10; next_symbol(t); t->newsym = 1; }
    h = v32_txh[t->acc]; w = v32_ech[t->acc];
    for (j = 0; j < V32_TXL; j++) {
        acc += t->br[j + V32_TXD] * h[j][0] - t->bi[j + V32_TXD] * h[j][1];
        ref += t->br[j] * w[j][0] - t->bi[j] * w[j][1];
    }
    v = ((int64_t)ref * t->gain + (1 << 13)) >> 14;
    t->ref = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    v = ((int64_t)acc * t->gain + (1 << 13)) >> 14;
    t->acc += 3;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    return (int16_t)v;
}
