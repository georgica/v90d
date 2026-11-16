/* v90d/v34 - data mode encoder, V.34 clauses 8 and 9: framing, parser,
 * shell mapper, differential encoder, mapper, precoder and trellis encoder.
 * Used by the test peer (the analogue modem) and usable on the server to
 * regenerate B1 as a known reference. Integer only. */
#include <string.h>
#include "v34_priv.h"

void v34_tx_init(v34_tx_t *tx, const v34_params_t *p, v34_getbit_fn get,
                 void *user)
{
    memset(tx, 0, sizeof *tx);
    tx->p = p;
    tx->get = get;
    tx->user = user;
    /* 10.1.3.1: scrambler, trellis encoder, differential encoder and
     * precoder delay line start from zero; B1 carries the bit inversions of
     * the last data frame of a superframe */
    v34_scr_init(&tx->scr, p->cfg.scrambler);
    tx->b1 = 1;
    tx->half = (uint8_t)(2 * p->map.J - 2);
    tx->pos = 8;                        /* nothing buffered yet */
}

int v34_tx_in_b1(const v34_tx_t *tx)
{
    return tx->pos < 8 ? tx->b1_buf : tx->b1;
}

static unsigned tx_bit(v34_tx_t *tx)
{
    int b = tx->b1 ? 1 : (tx->get(tx->user) & 1);
    return (unsigned)v34_scramble(&tx->scr, b);
}

static unsigned tx_bits(v34_tx_t *tx, unsigned n)   /* first bit = LSB */
{
    unsigned v = 0, i;
    for (i = 0; i < n; i++)
        v |= tx_bit(tx) << i;
    return v;
}

static int16_t sat16(int32_t v)
{
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* 9.6.2 item 4) for one symbol, then items 1) to 3) for the next one */
static void tx_precode(v34_tx_t *tx, int ur, int ui, v34_pt_t *x, v34_pt_t *y)
{
    int32_t yr = ur + tx->cn[0], yi = ui + tx->cn[1];          /* (9-30) */
    int32_t xr = yr * V34_ONE - tx->pn[0];                     /* (9-31) */
    int32_t xi = yi * V34_ONE - tx->pn[1];

    y->re = sat16(yr * V34_ONE); y->im = sat16(yi * V34_ONE);
    x->re = sat16(xr);           x->im = sat16(xi);
    tx->xh[2][0] = tx->xh[1][0]; tx->xh[2][1] = tx->xh[1][1];
    tx->xh[1][0] = tx->xh[0][0]; tx->xh[1][1] = tx->xh[0][1];
    tx->xh[0][0] = xr;           tx->xh[0][1] = xi;
    v34_precode_pc(tx->p, tx->xh, tx->pn, tx->cn);
}

/* one mapping frame: 8 symbols into tx->x / tx->y */
static void tx_frame(v34_tx_t *tx)
{
    const v34_params_t *p = tx->p;
    unsigned b = p->map.b, K = p->map.K, q = p->map.q, P = p->map.P;
    unsigned high, j, i1[4], i23[4], qv[8];
    uint8_t ring[8];

    /* 8.2: frame switching */
    tx->swc = (uint8_t)(tx->swc + p->map.r);
    high = tx->swc >= P;
    if (high)
        tx->swc = (uint8_t)(tx->swc - P);

    /* 9.3: parser */
    if (b > 12) {
        /* a low frame carries K-1 shell bits, S(i,K) = 0 */
        uint32_t r0 = tx_bits(tx, high ? K : K - 1);
        v34_shell_map(p, r0, ring);
        for (j = 0; j < 4; j++) {
            i1[j] = tx_bit(tx);
            i23[j] = tx_bits(tx, 2);                    /* I2 + 2 * I3 (9-25) */
            qv[2 * j] = tx_bits(tx, q);
            qv[2 * j + 1] = tx_bits(tx, q);
        }
    } else {
        /* 9.3.2: 8, 9, 11 or 12 bits; I3 exists in the first n - 8 groups */
        unsigned n = high ? b : b - 1;
        memset(ring, 0, sizeof ring);
        for (j = 0; j < 4; j++) {
            i1[j] = tx_bit(tx);
            i23[j] = tx_bit(tx);
            if (j + 8 < n)
                i23[j] |= tx_bit(tx) << 1;
            qv[2 * j] = qv[2 * j + 1] = 0;
        }
    }

    /* 9.5, 9.6: the nine steps of Table 11 for each 4D symbol */
    for (j = 0; j < 4; j++) {
        unsigned v0 = 0, Z, U0, C0, c0par, s1, s2;
        int ur, ui;
        v34_pt_t *x = &tx->x[2 * j], *y = &tx->y[2 * j];

        if (tx->hm == 0) {              /* start of a half data frame */
            v0 = v34_v0(p->map.J, tx->half);
            if (++tx->half == 2u * p->map.J)
                tx->half = 0;
        }
        if (++tx->hm == 2 * P)
            tx->hm = 0;
        Z = (tx->z + i23[j]) & 3;       /* differential encoder */
        tx->z = (uint8_t)Z;

        /* steps 1 to 3 */
        v34_map_point(qv[2 * j] + ((unsigned)ring[2 * j] << q), Z, &ur, &ui);
        c0par = (unsigned)((tx->cn[0] + tx->cn[1]) / 2) & 1;
        tx_precode(tx, ur, ui, &x[0], &y[0]);
        /* step 4: modulo encoder 9.6.3.3 */
        C0 = c0par ^ ((unsigned)((tx->cn[0] + tx->cn[1]) / 2) & 1);
        /* step 5: (9-32) */
        U0 = v34_conv_y0(p->cfg.trellis, tx->conv) ^ C0 ^ v0;
        /* steps 6 to 8 */
        v34_map_point(qv[2 * j + 1] + ((unsigned)ring[2 * j + 1] << q),
                      Z + 2 * i1[j] + U0, &ur, &ui);
        tx_precode(tx, ur, ui, &x[1], &y[1]);
        /* step 9: symbol-to-bit converter and convolutional encoder */
        s1 = v34_subset(y[0].re / V34_ONE, y[0].im / V34_ONE);
        s2 = v34_subset(y[1].re / V34_ONE, y[1].im / V34_ONE);
        tx->conv = (uint8_t)v34_conv_next(p->cfg.trellis, tx->conv, v34_tab13[s1][s2]);
    }

    if (++tx->mf == P) {                /* end of the data frame */
        tx->mf = 0;
        tx->swc = 0;
        tx->b1 = 0;
    }
}

void v34_tx_symbol(v34_tx_t *tx, v34_pt_t *x, v34_pt_t *y)
{
    if (tx->pos >= 8) {
        tx->b1_buf = tx->b1;
        tx_frame(tx);
        tx->pos = 0;
    }
    *x = v34_nl_encode(tx->p, tx->x[tx->pos]);
    if (y)
        *y = tx->y[tx->pos];
    tx->pos++;
}
