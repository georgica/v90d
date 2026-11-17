#include "v14.h"

#define QMASK (V14_QSIZE - 1)
static unsigned q_len(const v14q_t *q) { return (q->head - q->tail) & QMASK; }
static int q_put(v14q_t *q, uint8_t c) { if (q_len(q) == QMASK) return 0; q->q[q->head] = c; q->head = (q->head + 1) & QMASK; return 1; }
static int q_get(v14q_t *q, uint8_t *c) { if (q->head == q->tail) return 0; *c = q->q[q->tail]; q->tail = (q->tail + 1) & QMASK; return 1; }

void v14_init(v14_t *v)
{
    v->txq.head = v->txq.tail = 0; v->rxq.head = v->rxq.tail = 0;
    v->tx_sh = 0; v->tx_n = 0;
    v->rx_state = 0; v->rx_sh = 0; v->rx_frame_errs = 0;
}

int v14_getbit(void *p)
{
    v14_t *v = p;
    int b;
    if (v->tx_n == 0) {
        uint8_t c;
        if (!q_get(&v->txq, &c)) return 1;                 /* idle mark */
        v->tx_sh = (uint16_t)(((unsigned)c << 1) | 0x200); /* start(0), 8 data, stop(1) */
        v->tx_n = 10;
    }
    b = v->tx_sh & 1; v->tx_sh >>= 1; v->tx_n--;
    return b;
}

void v14_putbit(void *p, int bit)
{
    v14_t *v = p;
    bit &= 1;
    if (v->rx_state == 0) {                 /* hunting */
        if (!bit) { v->rx_state = 1; v->rx_sh = 0; }
        return;
    }
    if (v->rx_state == -1) {                /* after a framing error: wait for mark */
        if (bit) v->rx_state = 0;
        return;
    }
    if (v->rx_state <= 8) {
        v->rx_sh = (uint8_t)((v->rx_sh >> 1) | (bit ? 0x80 : 0));
        v->rx_state++;
        return;
    }
    /* stop bit position */
    if (bit) { q_put(&v->rxq, v->rx_sh); v->rx_state = 0; return; }
    if (v->rx_sh == 0) {                    /* all space: a break, not data */
        v->rx_frame_errs++; v->rx_state = -1; return;
    }
    /* stop bit deleted by the far converter: this 0 is the next start bit */
    q_put(&v->rxq, v->rx_sh);
    v->rx_state = 1; v->rx_sh = 0;
}

int v14_write(v14_t *v, const uint8_t *buf, int n)
{
    int i;
    for (i = 0; i < n; i++) if (!q_put(&v->txq, buf[i])) break;
    return i;
}

int v14_read(v14_t *v, uint8_t *buf, int max)
{
    int i;
    for (i = 0; i < max; i++) if (!q_get(&v->rxq, &buf[i])) break;
    return i;
}

int v14_tx_pending(const v14_t *v) { return (int)q_len(&v->txq) + (v->tx_n > 0); }
