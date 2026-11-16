/* RFC 1662 HDLC-like async framing. See ppp_hdlc.h. */
#include "ppp_hdlc.h"

/* FCS-16, polynomial x^16 + x^12 + x^5 + 1, processed least significant
 * bit first (reflected constant 0x8408). This table holds the effect of
 * shifting four bits; each octet is handled as two nibbles. */
static const uint16_t fcs_nib[16] = {
    0x0000, 0x1081, 0x2102, 0x3183, 0x4204, 0x5285, 0x6306, 0x7387,
    0x8408, 0x9489, 0xa50a, 0xb58b, 0xc60c, 0xd68d, 0xe70e, 0xf78f
};

static uint16_t fcs_octet(uint16_t fcs, uint8_t b)
{
    fcs = (uint16_t)((fcs >> 4) ^ fcs_nib[(fcs ^ b) & 15]);
    fcs = (uint16_t)((fcs >> 4) ^ fcs_nib[(fcs ^ (b >> 4)) & 15]);
    return fcs;
}

uint16_t ppp_fcs16(uint16_t fcs, const uint8_t *p, size_t n)
{
    while (n--)
        fcs = fcs_octet(fcs, *p++);
    return fcs;
}

static int must_escape(uint8_t b, uint32_t accm)
{
    if (b == PPP_FLAG || b == PPP_ESCAPE)
        return 1;
    return b < 0x20 && ((accm >> b) & 1u);
}

static void tx_put(ppp_hdlc_tx_t *tx, uint8_t c)
{
    if (tx->n < tx->max)
        tx->out[tx->n] = c;
    tx->n++;                       /* keeps counting past max: size probe */
}

static void tx_stuff(ppp_hdlc_tx_t *tx, const uint8_t *p, size_t n)
{
    while (n--) {
        uint8_t c = *p++;
        if (must_escape(c, tx->accm)) {
            tx_put(tx, PPP_ESCAPE);
            c ^= 0x20;
        }
        tx_put(tx, c);
    }
}

void ppp_hdlc_tx_begin(ppp_hdlc_tx_t *tx, uint8_t *out, size_t max,
                       uint32_t accm, int open_flag)
{
    tx->out = out;
    tx->max = out ? max : 0;
    tx->n = 0;
    tx->accm = accm;
    tx->fcs = PPP_FCS_INIT;
    if (open_flag)
        tx_put(tx, PPP_FLAG);
}

void ppp_hdlc_tx_add(ppp_hdlc_tx_t *tx, const uint8_t *p, size_t n)
{
    tx->fcs = ppp_fcs16(tx->fcs, p, n);
    tx_stuff(tx, p, n);
}

/* finish the frame; returns the size it has (or would have had) */
static size_t tx_finish(ppp_hdlc_tx_t *tx)
{
    uint16_t fcs = (uint16_t)(tx->fcs ^ 0xffff);
    uint8_t tail[2];

    tail[0] = (uint8_t)(fcs & 0xff);   /* least significant octet first */
    tail[1] = (uint8_t)(fcs >> 8);
    tx_stuff(tx, tail, 2);
    tx_put(tx, PPP_FLAG);
    return tx->n;
}

size_t ppp_hdlc_tx_end(ppp_hdlc_tx_t *tx)
{
    size_t n = tx_finish(tx);
    return n <= tx->max ? n : 0;
}

size_t ppp_hdlc_encoded_size(const uint8_t *a, size_t alen,
                             const uint8_t *b, size_t blen,
                             uint32_t accm, int open_flag)
{
    ppp_hdlc_tx_t tx;

    ppp_hdlc_tx_begin(&tx, NULL, 0, accm, open_flag);
    ppp_hdlc_tx_add(&tx, a, alen);
    ppp_hdlc_tx_add(&tx, b, blen);
    return tx_finish(&tx);
}

size_t ppp_hdlc_encode(uint8_t *out, size_t max,
                       const uint8_t *a, size_t alen,
                       const uint8_t *b, size_t blen,
                       uint32_t accm, int open_flag)
{
    ppp_hdlc_tx_t tx;

    ppp_hdlc_tx_begin(&tx, out, max, accm, open_flag);
    ppp_hdlc_tx_add(&tx, a, alen);
    ppp_hdlc_tx_add(&tx, b, blen);
    return ppp_hdlc_tx_end(&tx);
}

void ppp_hdlc_rx_init(ppp_hdlc_rx_t *rx, size_t max_frame)
{
    if (max_frame > PPP_HDLC_BUF)
        max_frame = PPP_HDLC_BUF;
    rx->rx_accm = 0xffffffffu;
    rx->max_frame = (uint16_t)max_frame;
    rx->len = 0;
    rx->frames_ok = rx->bad_fcs = rx->runts = rx->overlong = rx->aborts = 0;
    rx->pos = 0;
    rx->fcs = PPP_FCS_INIT;
    rx->hunting = 1;
    rx->escaped = 0;
    rx->dropping = 0;
}

static void rx_restart(ppp_hdlc_rx_t *rx)
{
    rx->pos = 0;
    rx->fcs = PPP_FCS_INIT;
    rx->escaped = 0;
    rx->dropping = 0;
}

int ppp_hdlc_rx_byte(ppp_hdlc_rx_t *rx, uint8_t c)
{
    if (c == PPP_FLAG) {
        int good = 0;

        if (rx->hunting) {
            rx->hunting = 0;
        } else if (rx->escaped) {
            /* escape then flag: the sender aborted this frame */
            rx->aborts++;
        } else if (rx->dropping) {
            /* already counted when the limit was crossed */
        } else if (rx->pos == 0) {
            /* consecutive flags: inter-frame fill */
        } else if (rx->pos < 4) {
            rx->runts++;
        } else if (rx->fcs != PPP_FCS_GOOD) {
            rx->bad_fcs++;
        } else {
            rx->frames_ok++;
            rx->len = (uint16_t)(rx->pos - 2);
            good = 1;
        }
        rx_restart(rx);
        return good;
    }
    if (rx->hunting)
        return 0;
    /* RFC 1662 4.2: mapped control characters are removed first; they may
     * have been inserted by intervening equipment */
    if (c < 0x20 && ((rx->rx_accm >> c) & 1u))
        return 0;
    if (c == PPP_ESCAPE) {
        rx->escaped = 1;
        return 0;
    }
    if (rx->escaped) {
        rx->escaped = 0;
        c ^= 0x20;
    }
    if (rx->dropping)
        return 0;
    if (rx->pos >= rx->max_frame) {
        rx->dropping = 1;
        rx->overlong++;
        return 0;
    }
    rx->buf[rx->pos++] = c;
    rx->fcs = fcs_octet(rx->fcs, c);
    return 0;
}

int ppp_hdlc_parse(const uint8_t *f, size_t len, int acfc_ok, int pfc_ok,
                   uint16_t *proto, size_t *off)
{
    size_t i = 0;

    if (len >= 2 && f[0] == 0xff && f[1] == 0x03)
        i = 2;                     /* All-Stations address, UI control */
    else if (!acfc_ok)
        return -1;
    if (i >= len)
        return -1;
    if (f[i] & 1) {
        /* a one-octet protocol: only values below 0x100, only with PFC */
        if (!pfc_ok)
            return -1;
        *proto = f[i];
        i += 1;
    } else {
        /* two octets: the first even, the second odd (RFC 1661 section 2) */
        if (i + 1 >= len || !(f[i + 1] & 1))
            return -1;
        *proto = (uint16_t)((f[i] << 8) | f[i + 1]);
        i += 2;
    }
    *off = i;
    return 0;
}
