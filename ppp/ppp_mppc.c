/* MPPC (RFC 2118). See ppp_mppc.h. Written from the RFC; section numbers
 * below are its own.
 *
 * The history is 8192 octets. A packet is copied to the history at the
 * history pointer and coded there: literals (4.1) and copies of earlier
 * octets (4.2), bits most significant first. A packet that does not fit
 * behind the pointer goes to the front (bit B).
 *
 * Reading chosen for the history after such a move (the RFC is not clear,
 * see README.md): the compressor only copies from octets it has written
 * since the pointer was last at the front, so that a copy never reaches
 * back across the front of the buffer. That is valid whether the far end
 * takes the history as a ring or as a plain buffer. The decompressor takes
 * it as a ring, should a peer send such a copy. */
#include <string.h>
#include "ppp_mppc.h"

#define HMASK     (PPP_MPPC_HIST - 1u)
#define MIN_MATCH 3u

void ppp_mppc_enc_init(ppp_mppc_enc_t *e)
{
    memset(e, 0, sizeof *e);
    e->flushed = 1;
}

void ppp_mppc_enc_flush(ppp_mppc_enc_t *e)
{
    /* What the tables hold needs no clearing: a candidate is only used if
     * it lies before the current position, and everything before the
     * current position has been written since the pointer was at the
     * front. The octets themselves need not be zeroed either, because
     * nothing behind the pointer is ever referred to. */
    e->hp = 0;
    e->flushed = 1;
}

typedef struct {
    uint8_t *p;
    size_t   n, max;
    uint32_t acc;
    unsigned cnt;
    int      full;
} bitw_t;

/* most significant bit first */
static void put(bitw_t *w, unsigned v, unsigned nbits)
{
    w->acc = w->acc << nbits | v;
    w->cnt += nbits;
    while (w->cnt >= 8) {
        w->cnt -= 8;
        if (w->n < w->max)
            w->p[w->n++] = (uint8_t)(w->acc >> w->cnt);
        else
            w->full = 1;
    }
}

static unsigned hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    return (unsigned)((v * 0x9E3779B1u) >> (32 - PPP_MPPC_HASH_BITS));
}

size_t ppp_mppc_compress(ppp_mppc_enc_t *e,
                         const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn,
                         uint8_t *out, size_t max, unsigned *bits)
{
    size_t n = an + bn;
    unsigned flags = PPP_MPPC_COMPRESSED, i, end;
    uint8_t *h = e->hist;
    bitw_t w;

    *bits = e->flushed ? PPP_MPPC_FLUSHED : 0;
    if (n == 0 || n > PPP_MPPC_MAX_PKT) {
        ppp_mppc_enc_flush(e);
        return 0;
    }
    /* 3.1, bit B: no room behind the pointer, so to the front. After an
     * initialisation the packet is at the front as well, and says so. */
    if (e->flushed || e->hp + n > PPP_MPPC_HIST) {
        e->hp = 0;
        flags |= PPP_MPPC_AT_FRONT;
    }
    if (e->flushed)
        flags |= PPP_MPPC_FLUSHED;
    i = e->hp;
    end = i + (unsigned)n;
    if (an) memcpy(h + i, a, an);
    if (bn) memcpy(h + i + an, b, bn);

    w.p = out; w.n = 0; w.max = max; w.acc = 0; w.cnt = 0; w.full = 0;
    while (i < end && !w.full) {
        unsigned left = end - i, best = 0, off = 0, k;

        if (left >= MIN_MATCH) {
            unsigned hv = hash3(h + i), cand = e->head[hv], chain = PPP_MPPC_CHAIN, last = i;

            /* candidates lie before this position and get older along the chain */
            while (cand < last && chain-- > 0) {
                const uint8_t *m = h + cand, *s = h + i;
                if (m[best] == s[best] && m[0] == s[0] && m[1] == s[1]) {
                    unsigned len = 2;
                    while (len < left && m[len] == s[len])
                        len++;
                    if (len > best && len >= MIN_MATCH) {
                        best = len;
                        off = i - cand;
                        if (len == left)
                            break;
                    }
                }
                last = cand;
                cand = e->prev[cand];
            }
            e->prev[i] = e->head[hv];
            e->head[hv] = (uint16_t)i;
        }
        if (best < MIN_MATCH) {
            /* 4.1 */
            unsigned c = h[i++];
            if (c < 0x80) put(&w, c, 8);
            else put(&w, 0x100 | (c & 0x7f), 9);
            continue;
        }
        /* 4.2.1 */
        if (off < 64) put(&w, 0x3c0 | off, 10);
        else if (off < 320) put(&w, 0xe00 | (off - 64), 12);
        else put(&w, 0xc000 | (off - 320), 16);
        /* 4.2.2: for 2^(k+1) <= length < 2^(k+2), k ones, a zero, then the
         * low k+1 bits of the length; a single zero for 3 */
        if (best == 3) {
            put(&w, 0, 1);
        } else {
            for (k = 1; (4u << k) <= best; k++) ;
            put(&w, (1u << (k + 1)) - 2, k + 1);
            put(&w, best & ((2u << k) - 1), k + 1);
        }
        for (k = i + 1; k < i + best; k++) {
            if (end - k >= MIN_MATCH) {
                unsigned hv = hash3(h + k);
                e->prev[k] = e->head[hv];
                e->head[hv] = (uint16_t)k;
            }
        }
        i += best;
    }
    if (w.cnt)
        put(&w, 0, 8 - w.cnt);
    if (w.full || i < end) {
        /* section 3, "Data Expansion" */
        ppp_mppc_enc_flush(e);
        return 0;
    }
    e->hp = (uint16_t)end;             /* 8192: the next packet goes to the front */
    e->flushed = 0;
    *bits = flags;
    return w.n;
}

void ppp_mppc_dec_init(ppp_mppc_dec_t *d)
{
    /* 3.1: histories are always initialised with all zeroes */
    memset(d, 0, sizeof *d);
}

typedef struct {
    const uint8_t *in;
    size_t nbits, at;
} bitr_t;

/* the next n bits, or -1 past the end */
static long get(bitr_t *r, unsigned n)
{
    long v = 0;

    if (r->nbits - r->at < n)
        return -1;
    while (n-- > 0) {
        v = v << 1 | (r->in[r->at >> 3] >> (7 - (r->at & 7)) & 1);
        r->at++;
    }
    return v;
}

int ppp_mppc_decompress(ppp_mppc_dec_t *d, int at_front,
                        const uint8_t *in, size_t n, uint8_t *out, size_t max)
{
    bitr_t r;
    unsigned hp, start;

    if (n == 0)
        return -1;
    if (at_front)
        d->hp = 0;
    hp = start = d->hp;
    r.in = in; r.nbits = n * 8; r.at = 0;
    /* every element is at least 8 bits long; fewer left over are padding */
    while (r.nbits - r.at >= 8) {
        long v, off, len;
        unsigned ones;
        /* 4.1: 0 + 7 bits, or 10 + 7 bits for the upper half (at least
         * eight bits are left, so these two are there) */
        unsigned high = 0, literal = 1;

        if (get(&r, 1) == 1) {
            if (get(&r, 1) == 0) high = 0x80;
            else literal = 0;
        }
        if (literal) {
            v = get(&r, 7);
            if (v < 0 || hp >= PPP_MPPC_HIST || hp - start >= max)
                return -1;
            d->hist[hp++] = (uint8_t)(high | (unsigned)v);
            continue;
        }
        /* 4.2.1: 110 + 13 bits, 1110 + 8, 1111 + 6 */
        v = get(&r, 1);
        if (v == 0) {
            off = get(&r, 13);
            if (off >= 0) off += 320;
        } else if (get(&r, 1) == 0) {
            off = get(&r, 8);
            if (off >= 0) off += 64;
        } else {
            off = get(&r, 6);
        }
        if (v < 0 || off <= 0)
            return -1;
        /* 4.2.2 */
        for (ones = 0; (v = get(&r, 1)) == 1; ones++)
            if (ones >= 11)
                return -1;
        if (v < 0)
            return -1;
        if (ones == 0) {
            len = 3;
        } else {
            len = get(&r, ones + 1);
            if (len < 0)
                return -1;
            len += 2L << ones;
        }
        if ((unsigned long)len > PPP_MPPC_HIST - hp || (unsigned long)len > max - (hp - start))
            return -1;
        while (len-- > 0) {
            d->hist[hp] = d->hist[(hp - (unsigned)off) & HMASK];
            hp++;
        }
    }
    d->hp = (uint16_t)hp;
    memcpy(out, d->hist + start, hp - start);
    return (int)(hp - start);
}
