/* Start-stop characters over a synchronous bit stream (V.14 basic mode):
 * 1 start bit (0), 8 data bits LSB first, 1 stop bit (1), mark idle.
 * The receiver tolerates the stop-bit deletion V.14 allows the far converter
 * for overspeed terminals (a 0 where the stop bit belongs is taken as the next
 * character's start bit). Fixed-size queues, no allocation. */
#ifndef V90D_MODEM_V14_H
#define V90D_MODEM_V14_H
#include <stdint.h>

#define V14_QSIZE 512            /* power of two */

typedef struct { uint8_t q[V14_QSIZE]; unsigned head, tail; } v14q_t;

typedef struct {
    v14q_t   txq, rxq;
    uint16_t tx_sh; int tx_n;    /* character being shifted out */
    int      rx_state;           /* 0 hunting for a start bit, else bits collected + 1 */
    uint8_t  rx_sh;
    uint32_t rx_frame_errs;
} v14_t;

void v14_init(v14_t *v);
/* line side: called once per line bit by the modem */
int  v14_getbit(void *v);
void v14_putbit(void *v, int bit);
/* terminal side */
int  v14_write(v14_t *v, const uint8_t *buf, int n);   /* returns octets accepted */
int  v14_read(v14_t *v, uint8_t *buf, int max);        /* returns octets delivered */
int  v14_tx_pending(const v14_t *v);                   /* octets queued or in flight */

#endif
