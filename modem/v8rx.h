/* V.8 sequence receiver: assembles CM/JM sequences from a line-level stream and
 * reports (a) two identical consecutive sequences, (b) CJ (three zero octets). */
#ifndef V90D_MODEM_V8RX_H
#define V90D_MODEM_V8RX_H
#include "async.h"
#include "v8msg.h"

#define V8RX_MENU 1   /* >= 2 identical sequences received; see msg/nmsg */
#define V8RX_CJ   2

typedef struct {
    arx_t   u;
    uint8_t cur[V8_MAX_OCTETS]; int ncur, in_msg, overflow;
    uint8_t msg[V8_MAX_OCTETS]; int nmsg;    /* last completed sequence */
    int     matches;                         /* consecutive identical count */
    int     zeros;
} v8rx_t;

void v8rx_init(v8rx_t *r, int fs);
/* one sample of line level; returns a V8RX_* event mask (0 = nothing) */
int  v8rx_tick(v8rx_t *r, int level);

#endif
