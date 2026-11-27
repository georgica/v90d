/* See ppp_mppe.h. RFC 3078, stateless and stateful, the Linux kernel's key schedule. */
#include "ppp_mppe.h"
#include <string.h>

static void dir_begin(ppp_mppe_dir_t *d, const uint8_t *start, size_t keylen, int force)
{
    memcpy(d->start, start, keylen);
    ppp_mppe_initial_key(d->start, d->session, keylen, force);
    d->keylen = keylen;
    d->force = force;
    d->coherency = PPP_MPPE_COUNT;      /* so that the first packet is number 0 */
    d->stateful = 0;
    d->discard = 0;
    d->credit = PPP_MPPE_CREDIT_MAX;
}

static int is_flag(unsigned count)
{
    return (count & 0xffu) == 0xffu;    /* RFC 3078 7.2: the key changes here */
}

static void dir_stateful(ppp_mppe_dir_t *d)
{
    d->stateful = 1;
    d->discard = 0;
    ppp_rc4_init(&d->rc4, d->session, d->keylen);
}

void ppp_mppe_stateful(ppp_mppe_t *m, int tx, int rx)
{
    if (tx) dir_stateful(&m->tx);
    if (rx) dir_stateful(&m->rx);
}

void ppp_mppe_tx_resync(ppp_mppe_t *m)
{
    unsigned next = (m->tx.coherency + 1u) & PPP_MPPE_COUNT;

    if (!m->tx.stateful || is_flag(next))
        return;
    /* the packet after this count is the next flag packet */
    m->tx.coherency = (uint16_t)(((next | 0xffu) - 1u) & PPP_MPPE_COUNT);
}

int ppp_mppe_rx_out_of_step(const ppp_mppe_t *m)
{
    return m->rx.stateful && m->rx.discard;
}

void ppp_mppe_rx_lost(ppp_mppe_t *m)
{
    if (m->rx.stateful)
        m->rx.discard = 1;
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
    ppp_wipe(m, sizeof *m);
}

unsigned ppp_mppe_rx_late(const ppp_mppe_t *m)
{
    return m->rx.late;
}

size_t ppp_mppe_encrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
    ppp_rc4_t r;
    uint16_t hdr;

    if (len + 2 > max)
        return 0;
    if (m->tx.stateful) {
        /* RFC 3078 7.2: the stream runs on; a flag packet changes the key
         * and starts RC4 from it, and says so with FLUSHED (Windows takes
         * that either way, the Linux kernel wants it) */
        unsigned c = (m->tx.coherency + 1u) & PPP_MPPE_COUNT;
        m->tx.coherency = (uint16_t)c;
        hdr = (uint16_t)(PPP_MPPE_ENCRYPT | c);
        if (is_flag(c)) {
            ppp_mppe_change_key(m->tx.start, m->tx.session, m->tx.keylen, m->tx.force);
            ppp_rc4_init(&m->tx.rc4, m->tx.session, m->tx.keylen);
            hdr |= PPP_MPPE_FLUSHED;
        }
        out[0] = (uint8_t)(hdr >> 8);
        out[1] = (uint8_t)hdr;
        ppp_rc4(&m->tx.rc4, in, out + 2, len);
        return len + 2;
    }
    /* stateless: the next count, and a key change before every packet */
    m->tx.coherency = (uint16_t)((m->tx.coherency + 1) & PPP_MPPE_COUNT);
    ppp_mppe_change_key(m->tx.start, m->tx.session, m->tx.keylen, m->tx.force);
    ppp_rc4_init(&r, m->tx.session, m->tx.keylen);
    hdr = (uint16_t)(PPP_MPPE_FLUSHED | PPP_MPPE_ENCRYPT | m->tx.coherency);
    out[0] = (uint8_t)(hdr >> 8);
    out[1] = (uint8_t)hdr;
    ppp_rc4(&r, in, out + 2, len);
    ppp_wipe(&r, sizeof r);
    return len + 2;
}

/* RFC 3078 7.2 and 8.2 */
static int decrypt_stateful(ppp_mppe_dir_t *d, uint16_t hdr, const uint8_t *in, size_t len,
                            uint8_t *out)
{
    unsigned count = hdr & PPP_MPPE_COUNT, ahead, i, changes;
    int flushed = (hdr & PPP_MPPE_FLUSHED) != 0;

    ahead = (count - d->coherency) & PPP_MPPE_COUNT;
    if (!d->discard && ahead == 1) {
        /* the next in line */
        if (is_flag(count)) {
            ppp_mppe_change_key(d->start, d->session, d->keylen, d->force);
            ppp_rc4_init(&d->rc4, d->session, d->keylen);
        } else if (flushed) {
            ppp_rc4_init(&d->rc4, d->session, d->keylen);
        }
    } else if (ahead == 0 || ahead > PPP_MPPE_WINDOW) {
        /* a replay or a late packet: dropped, nothing changes */
        if (d->late < 0xffffu) d->late++;
        return -1;
    } else if (!flushed && !is_flag(count)) {
        /* a packet was lost and this one cannot be decrypted: out of step
         * until the peer starts afresh (the caller sends a Reset-Request) */
        d->discard = 1;
        return -1;
    } else {
        /* back in step: one key change for every flag packet since the
         * last one decrypted, this one included, then RC4 from that key */
        changes = 0;
        for (i = 1; i <= ahead; i++)
            if (is_flag((d->coherency + i) & PPP_MPPE_COUNT))
                changes++;
        while (changes--)
            ppp_mppe_change_key(d->start, d->session, d->keylen, d->force);
        ppp_rc4_init(&d->rc4, d->session, d->keylen);
        d->discard = 0;
    }
    d->coherency = (uint16_t)count;
    d->late = 0;
    ppp_rc4(&d->rc4, in, out, len);
    return (int)len;
}

int ppp_mppe_decrypt(ppp_mppe_t *m, const uint8_t *in, size_t len, uint8_t *out, size_t max)
{
    ppp_rc4_t r;
    uint16_t hdr, count;
    unsigned steps, i;

    if (len < 2 || len - 2 > max)
        return -1;
    hdr = (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
    if (!(hdr & PPP_MPPE_ENCRYPT))
        return -1;
    count = (uint16_t)(hdr & PPP_MPPE_COUNT);
    if (m->rx.stateful)
        return decrypt_stateful(&m->rx, hdr, in + 2, len - 2, out);
    /* RFC 3078 8.1: one key change per count since the last packet, so lost
     * packets are stepped over. Zero steps is a replay, more than the window
     * a late packet (or a desync we will not follow): dropped, keys kept. */
    steps = (unsigned)((count - m->rx.coherency) & PPP_MPPE_COUNT);
    if (steps == 0 || steps > PPP_MPPE_WINDOW) {
        if (m->rx.late < 0xffffu) m->rx.late++;
        return -1;
    }
    m->rx.credit = (uint16_t)(m->rx.credit + PPP_MPPE_CREDIT_STEP > PPP_MPPE_CREDIT_MAX ? PPP_MPPE_CREDIT_MAX
                                                                                       : m->rx.credit + PPP_MPPE_CREDIT_STEP);
    if (steps - 1 > m->rx.credit) {
        /* more catching up than this peer has earned: spend what it has,
         * step that far towards it and drop the packet. The credit grows
         * faster than the count does, so the next packets get there (a
         * long burst of loss is followed, never given up on), and a peer
         * that jumps ahead in every packet still costs only the credit. */
        for (i = 0; i < m->rx.credit; i++)
            ppp_mppe_change_key(m->rx.start, m->rx.session, m->rx.keylen, m->rx.force);
        m->rx.coherency = (uint16_t)((m->rx.coherency + m->rx.credit) & PPP_MPPE_COUNT);
        m->rx.credit = 0;
        return -1;
    }
    m->rx.credit = (uint16_t)(m->rx.credit - (steps - 1));
    for (i = 0; i < steps; i++)
        ppp_mppe_change_key(m->rx.start, m->rx.session, m->rx.keylen, m->rx.force);
    m->rx.coherency = count;
    m->rx.late = 0;
    ppp_rc4_init(&r, m->rx.session, m->rx.keylen);
    ppp_rc4(&r, in + 2, out, len - 2);
    ppp_wipe(&r, sizeof r);
    return (int)(len - 2);
}
