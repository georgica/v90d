/* HDLC bit-level framing for V.42 LAPM (ITU-T V.42 clause 8.1). */
#include <string.h>
#include "v42_hdlc.h"

#define FLAG 0x7E

/* ------------------------------------------------------------------- FCS */
/* Both generators are run least significant bit first, which is the order
 * the bits go on the line (8.1.2.2), so the register is the bit reversal of
 * the polynomial arithmetic written out in 8.1.1.6. */
uint16_t v42_fcs16(uint16_t fcs, const uint8_t *data, size_t len)
{
    size_t i; int b;
    for (i = 0; i < len; i++) {
        fcs ^= data[i];
        for (b = 0; b < 8; b++)
            fcs = (uint16_t)((fcs & 1) ? (fcs >> 1) ^ 0x8408u : fcs >> 1);
    }
    return fcs;
}

uint32_t v42_fcs32(uint32_t fcs, const uint8_t *data, size_t len)
{
    size_t i; int b;
    for (i = 0; i < len; i++) {
        fcs ^= data[i];
        for (b = 0; b < 8; b++)
            fcs = (fcs & 1) ? (fcs >> 1) ^ 0xEDB88320u : fcs >> 1;
    }
    return fcs;
}

/* -------------------------------------------------------------- transmit */
void v42hdlc_tx_init(v42hdlc_tx_t *t)
{
    memset(t, 0, sizeof *t);
}

int v42hdlc_tx_need_frame(const v42hdlc_tx_t *t)
{
    return t->len == 0 && !t->in_frame && t->nbits == 0;
}

int v42hdlc_tx_busy(const v42hdlc_tx_t *t)
{
    return t->len != 0 || t->in_frame;
}

int v42hdlc_tx_frame(v42hdlc_tx_t *t, const uint8_t *data, int len, int fcs32)
{
    if (t->len != 0 || len < 1 || len > V42HDLC_MAX_PAYLOAD) return -1;
    memcpy(t->buf, data, (size_t)len);
    if (fcs32) {
        uint32_t f = ~v42_fcs32(V42_FCS32_INIT, data, (size_t)len);
        t->buf[len++] = (uint8_t)f;
        t->buf[len++] = (uint8_t)(f >> 8);
        t->buf[len++] = (uint8_t)(f >> 16);
        t->buf[len++] = (uint8_t)(f >> 24);
    } else {
        uint16_t f = (uint16_t)~v42_fcs16(V42_FCS16_INIT, data, (size_t)len);
        t->buf[len++] = (uint8_t)f;
        t->buf[len++] = (uint8_t)(f >> 8);
    }
    t->len = len;
    t->pos = 0;
    return 0;
}

void v42hdlc_tx_flags(v42hdlc_tx_t *t, int n)
{
    if (n > t->min_flags) t->min_flags = n;
}

void v42hdlc_tx_abort(v42hdlc_tx_t *t)
{
    if (t->in_frame) t->abort_req = 1;
}

static void tx_raw(v42hdlc_tx_t *t, uint8_t octet)
{
    t->cur = octet; t->nbits = 8; t->raw = 1; t->ones = 0;
}

int v42hdlc_tx_bit(v42hdlc_tx_t *t)
{
    int bit;

    if (t->abort_req && t->in_frame) {
        /* eight ones abort the frame; flags follow */
        t->abort_req = 0; t->in_frame = 0; t->len = 0; t->pos = 0;
        t->flag_done = 0; t->aborts++;
        tx_raw(t, 0xFF);
    }
    if (t->in_frame && !t->raw && t->ones == 5) {
        t->ones = 0;                      /* zero insertion (8.1.1.2) */
        return 0;
    }
    if (t->nbits == 0) {
        if (t->in_frame && t->pos < t->len) {
            t->cur = t->buf[t->pos++]; t->nbits = 8; t->raw = 0;
        } else if (t->in_frame) {
            /* frame data exhausted: closing flag */
            t->in_frame = 0; t->len = 0; t->pos = 0; t->frames++;
            tx_raw(t, FLAG);
            t->flag_done = 1;
            if (t->min_flags > 0) t->min_flags--;
        } else if (t->len != 0 && t->flag_done && t->min_flags == 0) {
            /* the flag just sent serves as the opening flag */
            t->in_frame = 1; t->flag_done = 0; t->ones = 0;
            t->cur = t->buf[t->pos++]; t->nbits = 8; t->raw = 0;
        } else {
            tx_raw(t, FLAG);              /* interframe time fill (8.1.5) */
            t->flag_done = 1;
            if (t->min_flags > 0) t->min_flags--;
        }
    }
    bit = t->cur & 1;
    t->cur >>= 1;
    t->nbits--;
    if (!t->raw) {
        if (bit) t->ones++; else t->ones = 0;
    }
    return bit;
}

/* --------------------------------------------------------------- receive */
void v42hdlc_rx_init(v42hdlc_rx_t *r)
{
    memset(r, 0, sizeof *r);
    r->max_len = V42HDLC_MAX_FRAME;
    r->fcs_mode = V42HDLC_FCS16;
}

void v42hdlc_rx_set_fcs(v42hdlc_rx_t *r, int fcs_mode)
{
    r->fcs_mode = fcs_mode;
}

void v42hdlc_rx_set_max(v42hdlc_rx_t *r, int max_len)
{
    if (max_len < 4) max_len = 4;
    if (max_len > V42HDLC_MAX_FRAME) max_len = V42HDLC_MAX_FRAME;
    r->max_len = max_len;
}

static void rx_restart(v42hdlc_rx_t *r)
{
    r->len = 0; r->nbits = 0; r->cur = 0; r->overflow = 0;
}

/* A flag has just completed. The accumulator holds the frame octets plus
 * the flag's leading zero and five of its ones (six stray bits). */
static int rx_flag(v42hdlc_rx_t *r)
{
    int len = r->len, nbits = r->nbits, overflow = r->overflow;
    int was_synced = r->synced;

    rx_restart(r);
    r->synced = 1;
    if (!was_synced) { r->flag_run = 1; return 0; }
    if (len == 0 && nbits == 6) { r->flag_run++; return 0; }
    r->flag_run = 1;
    if (overflow) { r->too_long++; return V42HDLC_RX_LONG; }
    if (nbits != 6) {
        if (len == 0) return 0;           /* debris between flags */
        r->misaligned++; return V42HDLC_RX_ALIGN;
    }
    /* 8.1.3 b): at least address, control and FCS */
    if (len < 4) { r->runts++; return V42HDLC_RX_SHORT; }
    if (r->fcs_mode != V42HDLC_FCS16 && len >= 6 &&
        v42_fcs32(V42_FCS32_INIT, r->buf, (size_t)len) == V42_FCS32_GOOD) {
        r->last_fcs32 = 1; r->good++;
        return len - 4;
    }
    if (r->fcs_mode != V42HDLC_FCS32 &&
        v42_fcs16(V42_FCS16_INIT, r->buf, (size_t)len) == V42_FCS16_GOOD) {
        r->last_fcs32 = 0; r->good++;
        return len - 2;
    }
    if (r->fcs_mode == V42HDLC_FCS32 && len < 6) { r->runts++; return V42HDLC_RX_SHORT; }
    r->bad_fcs++;
    return V42HDLC_RX_BAD_FCS;
}

int v42hdlc_rx_bit(v42hdlc_rx_t *r, int bit)
{
    if (bit & 1) {
        if (r->ones < 7) r->ones++;
        if (r->ones == 7) {
            /* seven contiguous ones: abort (8.1.4); wait for a flag */
            int had = r->synced && r->len > 0;
            r->ones = 8;                  /* saturate; report once */
            r->synced = 0; r->flag_run = 0;
            rx_restart(r);
            if (had) { r->aborts++; return V42HDLC_RX_ABORT; }
            return 0;
        }
        if (r->ones >= 6) return 0;       /* flag or abort decided by next bit */
    } else {
        int ones = r->ones;
        r->ones = 0;
        if (ones == 5) return 0;          /* inserted zero: delete (8.1.1.2) */
        if (ones == 6) return rx_flag(r);
        if (ones >= 7) return 0;          /* still hunting for a flag */
    }
    if (!r->synced) return 0;
    r->cur = (uint8_t)(r->cur | ((bit & 1) << r->nbits));
    if (++r->nbits == 8) {
        if (r->len < r->max_len) r->buf[r->len++] = r->cur;
        else r->overflow = 1;
        r->nbits = 0; r->cur = 0;
    }
    return 0;
}
