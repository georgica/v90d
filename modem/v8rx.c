#include <string.h>
#include "v8rx.h"

#define PREAMBLE_BITS 8    /* >= this much mark before a character = new sequence
                              (10 ONEs + half a stop bit are sent; margin for timing) */

void v8rx_init(v8rx_t *r, int fs)
{
    arx_init(&r->u, fs, 300);
    r->ncur = r->in_msg = r->overflow = 0;
    r->nmsg = 0; r->matches = 0; r->zeros = 0;
}

static int finalize(v8rx_t *r)
{
    r->in_msg = 0;
    if (r->ncur == 0 || r->overflow) return 0;
    if (r->ncur == r->nmsg && !memcmp(r->cur, r->msg, (size_t)r->ncur)) {
        r->matches++;
    } else {
        memcpy(r->msg, r->cur, (size_t)r->ncur);
        r->nmsg = r->ncur;
        r->matches = 1;
    }
    return r->matches >= 2 ? V8RX_MENU : 0;
}

int v8rx_tick(v8rx_t *r, int level)
{
    int ev = 0;
    int was_idle_bits = arx_idle_bits(&r->u);
    int c = arx_tick(&r->u, level);

    if (c == ARX_FRAMEERR) { r->in_msg = 0; r->zeros = 0; return 0; }
    if (c == ARX_NONE) {
        /* the mark run just reached preamble length: the sequence has ended */
        if (r->in_msg && was_idle_bits < PREAMBLE_BITS && arx_idle_bits(&r->u) >= PREAMBLE_BITS)
            ev |= finalize(r);
        return ev;
    }
    if (c == 0) { if (++r->zeros == 3) ev |= V8RX_CJ; } else r->zeros = 0;

    if (arx_pre_bits(&r->u) >= PREAMBLE_BITS) {
        if (r->in_msg) ev |= finalize(r);
        r->in_msg = (c == V8_SYNC_CM_JM);
        r->ncur = 0; r->overflow = 0;
    } else if (r->in_msg) {
        if (r->ncur < V8_MAX_OCTETS) r->cur[r->ncur++] = (uint8_t)c;
        else r->overflow = 1;
    }
    return ev;
}
