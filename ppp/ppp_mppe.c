/* See ppp_mppe.h. RFC 3078, stateless mode. */
#include "ppp_mppe.h"
#include <string.h>

static void dir_begin(ppp_mppe_dir_t *d, const uint8_t *start, size_t keylen, int force)
{
    memcpy(d->start, start, keylen);
    memcpy(d->session, start, keylen);   /* SessionKey = StartKey before the first change */
    d->keylen = keylen;
    d->force = force;
    d->coherency = 0;
}

void ppp_mppe_begin(ppp_mppe_t *m, const uint8_t send_start[16], const uint8_t recv_start[16],
                    size_t keylen, int force)
{
    dir_begin(&m->tx, send_start, keylen, force);
    dir_begin(&m->rx, recv_start, keylen, force);
    m->on = 1;
}

void ppp_mppe_reset(ppp_mppe_t *m)
{
    memset(m, 0, sizeof *m);
}

size_t ppp_mppe_encrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
    ppp_rc4_t r;
    uint16_t hdr;

    if (len + 2 > max)
        return 0;
    /* stateless: a fresh key for every packet (sender changes before encrypting) */
    ppp_mppe_new_key(m->tx.start, m->tx.session, m->tx.keylen, m->tx.force);
    ppp_rc4_init(&r, m->tx.session, m->tx.keylen);
    hdr = (uint16_t)(PPP_MPPE_FLUSHED | PPP_MPPE_ENCRYPT | (m->tx.coherency & PPP_MPPE_COUNT));
    out[0] = (uint8_t)(hdr >> 8);
    out[1] = (uint8_t)hdr;
    ppp_rc4(&r, in, out + 2, len);
    m->tx.coherency = (uint16_t)((m->tx.coherency + 1) & PPP_MPPE_COUNT);
    return len + 2;
}

int ppp_mppe_decrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
    ppp_rc4_t r;
    uint16_t hdr, count;
    unsigned skip, i;

    if (len < 2 || len - 2 > max)
        return -1;
    hdr = (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
    if (!(hdr & PPP_MPPE_ENCRYPT))
        return -1;
    count = (uint16_t)(hdr & PPP_MPPE_COUNT);
    /* stateless: advance our key to this packet's, one change per packet
     * since the one we expected (so lost packets are skipped over). A count
     * far ahead of ours is a reorder or a desync we will not follow. */
    skip = (unsigned)((count - m->rx.coherency) & PPP_MPPE_COUNT);
    if (skip > 256)
        return -1;
    for (i = 0; i <= skip; i++)
        ppp_mppe_new_key(m->rx.start, m->rx.session, m->rx.keylen, m->rx.force);
    m->rx.coherency = (uint16_t)((count + 1) & PPP_MPPE_COUNT);
    ppp_rc4_init(&r, m->rx.session, m->rx.keylen);
    ppp_rc4(&r, in + 2, out, len - 2);
    return (int)(len - 2);
}
