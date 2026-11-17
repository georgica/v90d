/* V.42 detection phase patterns and start-stop framing (V.42 7.2.1). */
#include <string.h>
#include "v42_detect.h"

/* ------------------------------------------------ start-stop characters */
enum { AS_IDLE = 0, AS_DATA, AS_STOP, AS_WAIT_MARK };

void v42async_rx_init(v42async_rx_t *r)
{
    memset(r, 0, sizeof *r);
}

int v42async_rx_bit(v42async_rx_t *r, int bit)
{
    bit &= 1;
    switch (r->state) {
    case AS_IDLE:
        if (bit) { if (r->ones < 30000) r->ones++; }
        else { r->state = AS_DATA; r->nbits = 0; r->ch = 0; }
        break;
    case AS_DATA:
        r->ch = (uint8_t)(r->ch | (bit << r->nbits));
        if (++r->nbits == 8) r->state = AS_STOP;
        break;
    case AS_STOP:
        if (bit) {
            r->gap = r->ones; r->ones = 0; r->state = AS_IDLE;
            return r->ch;
        }
        /* no stop bit: drop it and wait for the line to return to mark so
         * a long space is reported once, not as a run of characters */
        r->ones = 0; r->state = AS_WAIT_MARK;
        return V42ASYNC_FRAMING;
    default:
        if (bit) { r->state = AS_IDLE; r->ones = 0; }
        break;
    }
    return V42ASYNC_NONE;
}

void v42async_tx_init(v42async_tx_t *t)
{
    t->sr = 0; t->nbits = 0;
}

int v42async_tx_idle(const v42async_tx_t *t)
{
    return t->nbits == 0;
}

void v42async_tx_char(v42async_tx_t *t, uint8_t c)
{
    /* start bit 0, eight data bits lsb first, stop bit 1 */
    t->sr = (uint16_t)(((unsigned)c << 1) | 0x200u);
    t->nbits = 10;
}

int v42async_tx_bit(v42async_tx_t *t)
{
    int bit;
    if (t->nbits == 0) return 1;
    bit = t->sr & 1;
    t->sr >>= 1;
    t->nbits--;
    return bit;
}

/* ------------------------------------------------------ pattern sender */
#define DC1_EVEN 0x11       /* 0 1000 1000 1 on the line */
#define DC1_ODD  0x91       /* 0 1000 1001 1 on the line */

void v42det_tx_init(v42det_tx_t *t, int pattern, int gap)
{
    memset(t, 0, sizeof *t);
    if (gap < 8) gap = 8;
    if (gap > 16) gap = 16;
    t->gap = gap;
    switch (pattern) {
    case V42DET_PAT_ADP_V42:  t->ch[0] = 'E'; t->ch[1] = 'C'; break;
    case V42DET_PAT_ADP_NOEC: t->ch[0] = 'E'; t->ch[1] = 0;   break;
    default:                  t->ch[0] = DC1_EVEN; t->ch[1] = DC1_ODD; break;
    }
}

int v42det_tx_boundary(const v42det_tx_t *t)
{
    return t->which == 0 && t->pos == 0;
}

int v42det_tx_bit(v42det_tx_t *t)
{
    int bit, p = t->pos;

    if (p == 0) bit = 0;                               /* start */
    else if (p <= 8) bit = (t->ch[t->which] >> (p - 1)) & 1;
    else bit = 1;                                      /* stop, then mark */
    if (++t->pos == 10 + t->gap) {
        t->pos = 0;
        if (t->which) t->patterns++;
        t->which ^= 1;
    }
    return bit;
}

/* ---------------------------------------------------- pattern detector */
void v42det_rx_init(v42det_rx_t *d, int look_for_adp)
{
    memset(d, 0, sizeof *d);
    v42async_rx_init(&d->rx);
    d->look_for_adp = look_for_adp;
    d->last_char = -1;
}

int v42det_rx_bit(v42det_rx_t *d, int bit)
{
    int c = v42async_rx_bit(&d->rx, bit);
    const uint8_t *h = d->hist;

    d->last_char = c >= 0 ? c : -1;
    if (c == V42ASYNC_FRAMING) { d->run = 0; return V42DET_NONE; }
    if (c < 0) return V42DET_NONE;

    /* characters must be separated by 8 to 16 ones (7.2.1.1). That is held
     * to for the ADP; for the ODP any number of ones will do: a Conexant
     * CX93001 leaves 14, 22 or none between its DC1s. */
    if (d->run > 0 && (!d->look_for_adp || (d->rx.gap >= 8 && d->rx.gap <= 16))) {
        if (d->run < 4) d->run++;
    } else {
        d->run = 1;
    }
    d->hist[3] = d->hist[2]; d->hist[2] = d->hist[1]; d->hist[1] = d->hist[0];
    d->hist[0] = (uint8_t)c;
    if (d->result != V42DET_NONE || d->run < 4) return V42DET_NONE;
    /* two repetitions of a two character pattern */
    if (h[0] != h[2] || h[1] != h[3] || h[0] == h[1]) return V42DET_NONE;

    if (!d->look_for_adp) {
        /* 7.2.1.3: at least four DC1s of alternating parity */
        if ((h[0] == DC1_EVEN && h[1] == DC1_ODD) ||
            (h[0] == DC1_ODD && h[1] == DC1_EVEN))
            d->result = V42DET_ODP;
    } else {
        /* 7.2.1.2: the characters of at least two adjacent ADPs. Only the
         * two assigned patterns of Table 3 are acted upon. */
        uint8_t other;
        if (h[0] == 'E') other = h[1];
        else if (h[1] == 'E') other = h[0];
        else return V42DET_NONE;
        if (other == 'C') d->result = V42DET_ADP_V42;
        else if (other == 0) d->result = V42DET_ADP_NOEC;
    }
    return d->result;
}
