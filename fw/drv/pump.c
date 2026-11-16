/* See pump.h. */
#include "pump.h"

#define MASK (PUMP_RING - 1)

void pump_start(pump_t *p, uint8_t idle, unsigned latency)
{
    unsigned i;
    if (latency > PUMP_RING) latency = PUMP_RING;
    p->rx_w = p->rx_r = 0;
    p->tx_r = 0;
    for (i = 0; i < latency; i++) p->tx[i] = idle;
    p->tx_w = latency;
    p->idle = idle;
    p->underruns = p->overruns = 0;
}

void pump_tick(pump_t *p, const int16_t *rx, uint8_t *tx, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        /* nobody reads: the oldest whole block goes, so that the reader
         * stays on the line's block boundaries */
        if (p->rx_w - p->rx_r == PUMP_RING) { p->rx_r += PUMP_BLOCK; p->overruns += PUMP_BLOCK; }
        p->rx[p->rx_w++ & MASK] = rx[i];
        /* the answer is late: idle goes out in its place, and the place is
         * gone (tx_r runs ahead of tx_w; pump_write() skips what is past) */
        if ((int)(p->tx_w - p->tx_r) > 0) tx[i] = p->tx[p->tx_r & MASK];
        else { tx[i] = p->idle; p->underruns++; }
        p->tx_r++;
    }
}

int pump_ready(const pump_t *p) { return p->rx_w - p->rx_r >= PUMP_BLOCK; }

int pump_read(pump_t *p, int16_t out[PUMP_BLOCK])
{
    unsigned i, r = p->rx_r;
    if (p->rx_w - r < PUMP_BLOCK) return 0;
    for (i = 0; i < PUMP_BLOCK; i++) out[i] = p->rx[(r + i) & MASK];
    p->rx_r = r + PUMP_BLOCK;
    return 1;
}

int pump_write(pump_t *p, const uint8_t in[PUMP_BLOCK])
{
    unsigned i, w = p->tx_w;
    if ((int)(w - p->tx_r) > (int)(PUMP_RING - PUMP_BLOCK)) return -1;
    for (i = 0; i < PUMP_BLOCK; i++) p->tx[(w + i) & MASK] = in[i];
    p->tx_w = w + PUMP_BLOCK;
    return 0;
}

int16_t pump_lin16(int alaw, uint8_t cw)
{
    unsigned u = alaw ? (cw ^ 0x55u) & 0x7Fu : 0x7Fu - (cw & 0x7Fu);
    unsigned c = u >> 4, s = u & 15;
    int v;
    if (alaw) v = c == 0 ? (int)((2 * s + 1) << 3) : (int)((2 * s + 33) << (c + 2));
    else v = (int)((2 * s + 33) << (c + 2)) - 132;
    return (int16_t)((cw & 0x80) ? v : -v);
}

void pump_lin16_table(int alaw, int16_t t[256])
{
    unsigned i;
    for (i = 0; i < 256; i++) t[i] = pump_lin16(alaw, (uint8_t)i);
}

uint32_t pump_tx_word(const int16_t *lin, uint8_t cw)
{
    if (lin) return (uint32_t)(uint16_t)lin[cw] << 16;
    return ((uint32_t)cw << 24) | 0x00FF0000ul;
}

int16_t pump_rx_sample(int linear, uint32_t word)
{
    if (linear) return (int16_t)(uint16_t)(word >> 16);
    return (int16_t)(word >> 24);
}

unsigned pump_block_out(int hook, int linear, const int16_t s[PUMP_BLOCK], uint8_t out[PUMP_READ_LINEAR])
{
    unsigned i;
    out[0] = (uint8_t)(hook != 0);
    if (!linear) {
        for (i = 0; i < PUMP_BLOCK; i++) out[1 + i] = (uint8_t)s[i];
        return PUMP_READ_COMPANDED;
    }
    for (i = 0; i < PUMP_BLOCK; i++) {
        out[1 + 2 * i] = (uint8_t)((uint16_t)s[i] & 0xFF);
        out[2 + 2 * i] = (uint8_t)((uint16_t)s[i] >> 8);
    }
    return PUMP_READ_LINEAR;
}

void pump_meter_add(pump_meter *m, int16_t v)
{
    unsigned a = v < 0 ? (unsigned)-(int)v : (unsigned)v;
    m->acc += v;
    m->pow += (unsigned long)a * a;
    if (a > m->peak) m->peak = a;
    if (++m->n >= PUMP_METER) {
        /* 8192 = 2^13; the mean rounds towards minus infinity */
        m->dc = m->acc >= 0 ? m->acc >> 13 : -((-m->acc + 8191) >> 13);
        m->ms = (unsigned long)(m->pow >> 13);
        m->last_peak = m->peak;
        m->acc = 0; m->pow = 0; m->peak = 0; m->n = 0;
    }
}
