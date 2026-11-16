/* PPP Deflate (RFC 1979) on the deflate format of RFC 1951. See
 * ppp_deflate.h. Written from the two RFCs; section numbers below are
 * RFC 1951's unless they say otherwise.
 *
 * Compressor, one call per packet:
 *   1. the packet is appended to the history (a linear buffer of the
 *      window plus some room; when the room is used up the window is moved
 *      down);
 *   2. it is turned into literals and <length, distance> pairs with hash
 *      chains over three-octet strings and lazy matching (section 4);
 *   3. the result is written as one block, with the dynamic code built for
 *      this packet (3.2.7) or the fixed one (3.2.6), whichever is shorter;
 *   4. the header of an empty stored block and the padding follow
 *      (RFC 1979 2.1); its 00 00 FF FF is not written.
 * Stored blocks are never written: a packet that does not shrink goes out
 * in its native form (RFC 1979 section 2, "Data Expansion").
 *
 * Positions in the hash tables are stream positions modulo 65536. An entry
 * is never trusted: a candidate is used only if its distance lies inside
 * the history and the octets there compare equal, so entries left over
 * from before a reset or from more than 65536 octets ago do no harm and
 * the tables never need clearing. */
#include <string.h>
#include "ppp_deflate.h"

#define PMASK      (PPP_DEFLATE_WSIZE - 1u)
#define MIN_MATCH  3u
#define MAX_MATCH  258u
#define NLIT       286          /* literal/length symbols 0..285 */
#define NDIST      30
#define NCL        19
#define EOB        256
#define BT_FIXED   1u
#define BT_DYNAMIC 2u
/* a three-octet match this far away costs more than three literals do */
#define FAR_3      4096u
/* packets with fewer symbols than this are not worth a code of their own */
#define DYN_MIN    24

/* 3.2.5: base length / distance and extra bits of each code */
static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
    67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
/* 3.2.7: the order in which the code length code lengths are sent */
static const uint8_t cl_order[NCL] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* ================================================================== */
/* prefix codes                                                        */

/* Huffman codes go out most significant bit first into a stream that is
 * filled from the least significant bit (3.1.1): keep them reversed. */
static unsigned bit_reverse(unsigned v, unsigned n)
{
    static const uint8_t r4[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
    unsigned r = (unsigned)r4[v & 15] << 12 | (unsigned)r4[v >> 4 & 15] << 8 |
                 (unsigned)r4[v >> 8 & 15] << 4 | r4[v >> 12 & 15];
    return r >> (16 - n);
}

/* 3.2.2: the codes that belong to a list of code lengths */
static void huff_codes(const uint8_t *len, int n, uint16_t *code)
{
    uint16_t count[16], next[16];
    unsigned c = 0;
    int i;

    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++)
        count[len[i]]++;
    count[0] = 0;
    next[0] = 0;
    for (i = 1; i <= 15; i++) {
        c = (c + count[i - 1]) << 1;
        next[i] = (uint16_t)c;
    }
    for (i = 0; i < n; i++)
        if (len[i])
            code[i] = (uint16_t)bit_reverse(next[len[i]]++, len[i]);
}

/* Code lengths of a Huffman code for the frequencies freq[0..n), n <= 286,
 * with no length above limit. Symbols that do not occur get length 0. The
 * tree is built from the sorted leaves with two queues; if it comes out
 * too deep the frequencies are halved (never to zero) and it is built
 * again, which ends at the latest with a balanced tree. The result is
 * always a complete code when two or more symbols occur. */
static void huff_lengths(const uint16_t *freq, int n, int limit, uint8_t *len)
{
    uint16_t f[NLIT], order[NLIT], par[2 * NLIT];
    uint32_t w[2 * NLIT];
    uint8_t dep[2 * NLIT];
    int used = 0, i, j, gap;

    for (i = 0; i < n; i++) {
        len[i] = 0;
        f[i] = freq[i];
        if (f[i])
            order[used++] = (uint16_t)i;
    }
    if (used == 0)
        return;
    if (used == 1) {
        len[order[0]] = 1;
        return;
    }
    for (;;) {
        int leaf = 0, inner = used, next = used, root = 2 * used - 2, deepest = 0;

        /* ascending by frequency, then by symbol (Shell's method) */
        for (gap = used / 2; gap > 0; gap = gap == 2 ? 1 : gap * 5 / 11) {
            for (i = gap; i < used; i++) {
                uint16_t s = order[i];
                for (j = i; j >= gap; j -= gap) {
                    uint16_t t = order[j - gap];
                    if (f[t] < f[s] || (f[t] == f[s] && t < s))
                        break;
                    order[j] = t;
                }
                order[j] = s;
            }
        }
        for (i = 0; i < used; i++)
            w[i] = f[order[i]];
        while (next <= root) {
            int k, pick[2];
            for (k = 0; k < 2; k++) {
                /* the lighter of the two queue heads; a leaf on a tie */
                if (leaf < used && (inner >= next || w[leaf] <= w[inner]))
                    pick[k] = leaf++;
                else
                    pick[k] = inner++;
            }
            w[next] = w[pick[0]] + w[pick[1]];
            par[pick[0]] = par[pick[1]] = (uint16_t)next;
            next++;
        }
        dep[root] = 0;
        for (i = root - 1; i >= 0; i--) {
            dep[i] = (uint8_t)(dep[par[i]] + 1);
            if (i < used && dep[i] > deepest)
                deepest = dep[i];
        }
        if (deepest <= limit)
            break;
        for (i = 0; i < used; i++)
            f[order[i]] = (uint16_t)((f[order[i]] + 1) >> 1);
    }
    for (i = 0; i < used; i++)
        len[order[i]] = dep[i];
}

/* ================================================================== */
/* compressor                                                          */

void ppp_deflate_effort_default(ppp_deflate_effort_t *e)
{
    e->max_chain = 24;
    e->good_len = 16;
    e->nice_len = 96;
    e->lazy_len = 24;
    e->work = 10000;
}

void ppp_deflate_enc_init(ppp_deflate_enc_t *e, int wbits,
                          const ppp_deflate_effort_t *effort)
{
    int i;

    memset(e, 0, sizeof *e);
    if (effort)
        e->effort = *effort;
    else
        ppp_deflate_effort_default(&e->effort);
    if (e->effort.max_chain < 1) e->effort.max_chain = 1;
    if (e->effort.nice_len < MIN_MATCH) e->effort.nice_len = MIN_MATCH;
    if (wbits < PPP_DEFLATE_MIN_WBITS) wbits = PPP_DEFLATE_MIN_WBITS;
    if (wbits > PPP_DEFLATE_MAX_WBITS) wbits = PPP_DEFLATE_MAX_WBITS;
    e->wbits = (uint8_t)wbits;
    e->wsize = 1u << wbits;
    /* 3.2.6 */
    for (i = 0; i < 288; i++)
        e->fix_llen[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
    huff_codes(e->fix_llen, 288, e->fix_lcode);
    for (i = 0; i < NDIST; i++)
        e->fix_dcode[i] = (uint8_t)bit_reverse((unsigned)i, 5);
}

void ppp_deflate_enc_reset(ppp_deflate_enc_t *e)
{
    /* the positions go on counting; what the tables still hold fails the
     * distance test from now on */
    e->off = (uint16_t)(e->off + e->pos);
    e->pos = 0;
}

static unsigned hash3(const uint8_t *p)
{
    uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
    return (unsigned)((v * 0x9E3779B1u) >> (32 - PPP_DEFLATE_HASH_BITS));
}

/* 3.2.5: code, number of extra bits and their value for a match length
 * (given as length - 3) and for a distance */
static unsigned len_symbol(unsigned l3, unsigned *xb, unsigned *xv)
{
    unsigned n = 3;

    *xb = 0; *xv = 0;
    if (l3 < 8) return 257 + l3;
    if (l3 == 255) return 285;
    while (l3 >> (n + 1)) n++;                  /* 2^n <= l3 */
    *xb = n - 2;
    *xv = l3 & ((1u << (n - 2)) - 1);
    return 257 + 4 * n - 4 + ((l3 >> (n - 2)) & 3);
}

static unsigned dist_symbol(unsigned d, unsigned *xb, unsigned *xv)
{
    unsigned d1 = d - 1, n = 2;

    *xb = 0; *xv = 0;
    if (d1 < 4) return d1;
    while (d1 >> (n + 1)) n++;
    *xb = n - 1;
    *xv = d1 & ((1u << (n - 1)) - 1);
    return 2 * n + ((d1 >> (n - 1)) & 1);
}

/* The longest string in the history that equals the one at win[cur], at
 * most maxlen long and longer than `beat`; 0 if there is none. `chain`
 * candidates at the most; *work counts them (and long comparisons). */
static unsigned longest(ppp_deflate_enc_t *e, uint32_t cur, unsigned h,
                        unsigned maxlen, unsigned beat, unsigned chain,
                        unsigned *dist, uint32_t *work)
{
    const uint8_t *s = e->win + cur;
    uint16_t cabs = (uint16_t)(cur + e->off);
    uint16_t cand = e->head[h];
    uint32_t limit = cur < e->wsize ? cur : e->wsize;
    unsigned best = beat, d = (uint16_t)(cabs - cand), last = 0;
    unsigned nice = e->effort.nice_len;

    /* distances along a chain grow; anything else is a stale entry */
    while (d > last && d <= limit && chain-- > 0) {
        const uint8_t *m = s - d;

        (*work)++;
        if (m[best] == s[best] && m[0] == s[0] && m[1] == s[1]) {
            unsigned len = 2;
            while (len < maxlen && m[len] == s[len])
                len++;
            *work += len >> 4;
            if (len > best && !(len == MIN_MATCH && d > FAR_3)) {
                best = len;
                *dist = d;
                if (len >= nice || len >= maxlen)
                    break;
            }
        }
        last = d;
        cand = e->prev[cand & PMASK];
        d = (uint16_t)(cabs - cand);
    }
    return best > beat ? best : 0;
}

static void insert(ppp_deflate_enc_t *e, uint32_t i, unsigned h)
{
    uint16_t a = (uint16_t)(i + e->off);

    e->prev[a & PMASK] = e->head[h];
    e->head[h] = a;
}

static void token(ppp_deflate_enc_t *e, unsigned dist, unsigned val)
{
    e->tok_dist[e->ntok] = (uint16_t)dist;
    e->tok_val[e->ntok] = (uint8_t)val;
    e->ntok++;
}

/* section 4: literals and matches for win[start..end), lazily: a match is
 * held back for one position and dropped for a literal if the next
 * position has a longer one */
static void tokenize(ppp_deflate_enc_t *e, uint32_t start, uint32_t end)
{
    const ppp_deflate_effort_t *ef = &e->effort;
    uint32_t i = start, work = 0, k;
    unsigned held = 0, held_dist = 0;

    e->ntok = 0;
    while (i < end) {
        uint32_t left = end - i;
        unsigned len = 0, dist = 0;

        if (left >= MIN_MATCH) {
            unsigned h = hash3(e->win + i);
            unsigned maxlen = left < MAX_MATCH ? (unsigned)left : MAX_MATCH;

            if (held < maxlen && !(held && held >= ef->lazy_len)) {
                unsigned chain = ef->max_chain;
                if (held >= ef->good_len) chain = (chain >> 2) + 1;
                if (work >= ef->work) chain = 1;      /* out of time: one look */
                len = longest(e, i, h, maxlen, held ? held : MIN_MATCH - 1, chain, &dist, &work);
            }
            insert(e, i, h);
        }
        if (held) {
            if (len > held) {
                /* the next position does better: the held one becomes a literal */
                token(e, 0, e->win[i - 1]);
                held = len; held_dist = dist;
                i++;
            } else {
                /* take the held match; it began one position back */
                uint32_t stop = i - 1 + held;
                token(e, held_dist, held - MIN_MATCH);
                for (k = i + 1; k < stop; k++)
                    if (end - k >= MIN_MATCH)
                        insert(e, k, hash3(e->win + k));
                i = stop;
                held = 0;
            }
        } else if (len >= MIN_MATCH) {
            held = len; held_dist = dist;
            i++;
        } else {
            token(e, 0, e->win[i]);
            i++;
        }
    }
    if (held)
        token(e, held_dist, held - MIN_MATCH);
    if (work >= ef->work) {
        e->cuts++;
        if (work > ef->work) work = ef->work;     /* the last look may run over */
    }
    e->last_work = work;
}

/* ---- output ---- */
typedef struct {
    uint8_t *p;
    size_t   n;
    uint32_t acc;
    unsigned cnt;
} bitw_t;

static void put(bitw_t *w, unsigned v, unsigned nbits)
{
    w->acc |= (uint32_t)v << w->cnt;
    w->cnt += nbits;
    while (w->cnt >= 8) {
        w->p[w->n++] = (uint8_t)w->acc;
        w->acc >>= 8;
        w->cnt -= 8;
    }
}

/* the literals and matches, then end-of-block, in the given code */
static void put_tokens(ppp_deflate_enc_t *e, bitw_t *w,
                       const uint16_t *lcode, const uint8_t *llen,
                       const uint16_t *dcode, const uint8_t *dlen,
                       const uint8_t *dcode5)
{
    unsigned i, xb, xv, s;

    for (i = 0; i < e->ntok; i++) {
        unsigned d = e->tok_dist[i];
        if (d == 0) {
            put(w, lcode[e->tok_val[i]], llen[e->tok_val[i]]);
            continue;
        }
        s = len_symbol(e->tok_val[i], &xb, &xv);
        put(w, lcode[s], llen[s]);
        if (xb) put(w, xv, xb);
        s = dist_symbol(d, &xb, &xv);
        if (dcode5) put(w, dcode5[s], 5);
        else put(w, dcode[s], dlen[s]);
        if (xb) put(w, xv, xb);
    }
    put(w, lcode[EOB], llen[EOB]);
}

size_t ppp_deflate_compress(ppp_deflate_enc_t *e,
                            const uint8_t *a, size_t an,
                            const uint8_t *b, size_t bn,
                            uint8_t *out, size_t max)
{
    /* 3.2.7: the code lengths of both codes as one run-length coded list */
    uint8_t lens[NLIT + NDIST], cl_sym[NLIT + NDIST], cl_xv[NLIT + NDIST];
    uint8_t cl_len[NCL];
    uint16_t cl_freq[NCL], cl_code[NCL];
    size_t n = an + bn;
    uint32_t start, fixed_bits, dyn_bits = 0, xbits = 0, bits;
    unsigned i, xb, xv, s, hlit = 0, hdist = 0, hclen = 0, nops = 0, total, ndist = 0;
    int dynamic = 0;
    bitw_t w;

    if (n == 0 || n > PPP_DEFLATE_MAX_PKT)
        return 0;
    /* room for the packet: move the window down when the buffer is full */
    if (e->pos + n > sizeof e->win) {
        uint32_t keep = e->pos < e->wsize ? e->pos : e->wsize;
        memmove(e->win, e->win + e->pos - keep, keep);
        e->off = (uint16_t)(e->off + (e->pos - keep));
        e->pos = keep;
    }
    start = e->pos;
    if (an) memcpy(e->win + start, a, an);
    if (bn) memcpy(e->win + start + an, b, bn);
    e->pos = start + (uint32_t)n;

    tokenize(e, start, e->pos);

    /* what the symbols cost in the fixed code */
    memset(e->lfreq, 0, sizeof e->lfreq);
    memset(e->dfreq, 0, sizeof e->dfreq);
    fixed_bits = 0;
    for (i = 0; i < e->ntok; i++) {
        if (e->tok_dist[i] == 0) {
            e->lfreq[e->tok_val[i]]++;
            fixed_bits += e->fix_llen[e->tok_val[i]];
            continue;
        }
        s = len_symbol(e->tok_val[i], &xb, &xv);
        e->lfreq[s]++;
        fixed_bits += e->fix_llen[s] + xb + 5u;
        xbits += xb;
        s = dist_symbol(e->tok_dist[i], &xb, &xv);
        e->dfreq[s]++;
        fixed_bits += xb;
        xbits += xb;
        ndist++;
    }
    e->lfreq[EOB] = 1;
    fixed_bits += 3u + e->fix_llen[EOB];

    if (e->ntok >= DYN_MIN) {
        huff_lengths(e->lfreq, NLIT, 15, e->llen);
        huff_lengths(e->dfreq, NDIST, 15, e->dlen);
        /* The distance code must be a complete one as well: with no
         * distance in use, or one, a second code of one bit fills it.
         * (3.2.7 also allows a single code there; this needs nothing
         * special from the far end.) */
        if (ndist == 0) {
            e->dlen[0] = e->dlen[1] = 1;
        } else {
            unsigned used = 0, only = 0;
            for (i = 0; i < NDIST; i++)
                if (e->dlen[i]) { used++; only = i; }
            if (used == 1)
                e->dlen[only == 0 ? 1 : 0] = 1;
        }
        for (hlit = NLIT; hlit > 257 && e->llen[hlit - 1] == 0; hlit--) ;
        for (hdist = NDIST; hdist > 1 && e->dlen[hdist - 1] == 0; hdist--) ;
        memcpy(lens, e->llen, hlit);
        memcpy(lens + hlit, e->dlen, hdist);
        total = hlit + hdist;

        /* runs: 16 = the previous length 3..6 more times, 17 = 3..10
         * zeros, 18 = 11..138 zeros */
        memset(cl_freq, 0, sizeof cl_freq);
        for (i = 0; i < total; ) {
            unsigned v = lens[i], run = 1;
            while (i + run < total && lens[i + run] == v)
                run++;
            i += run;
            if (v == 0) {
                while (run >= 11) {
                    unsigned r = run > 138 ? 138 : run;
                    cl_sym[nops] = 18; cl_xv[nops++] = (uint8_t)(r - 11); cl_freq[18]++;
                    run -= r;
                }
                if (run >= 3) {
                    cl_sym[nops] = 17; cl_xv[nops++] = (uint8_t)(run - 3); cl_freq[17]++;
                    run = 0;
                }
            } else {
                cl_sym[nops] = (uint8_t)v; cl_xv[nops++] = 0; cl_freq[v]++;
                run--;
                while (run >= 3) {
                    unsigned r = run > 6 ? 6 : run;
                    cl_sym[nops] = 16; cl_xv[nops++] = (uint8_t)(r - 3); cl_freq[16]++;
                    run -= r;
                }
            }
            while (run-- > 0) {
                cl_sym[nops] = (uint8_t)v; cl_xv[nops++] = 0; cl_freq[v]++;
            }
        }
        huff_lengths(cl_freq, NCL, 7, cl_len);
        {
            unsigned used = 0, only = 0;
            for (i = 0; i < NCL; i++)
                if (cl_len[i]) { used++; only = i; }
            if (used == 1)
                cl_len[only == 0 ? 1 : 0] = 1;
        }
        for (hclen = NCL; hclen > 4 && cl_len[cl_order[hclen - 1]] == 0; hclen--) ;

        dyn_bits = 3u + 5u + 5u + 4u + 3u * hclen + xbits;
        for (i = 0; i < nops; i++)
            dyn_bits += cl_len[cl_sym[i]] + (cl_sym[i] == 16 ? 2u : cl_sym[i] == 17 ? 3u : cl_sym[i] == 18 ? 7u : 0u);
        for (i = 0; i < NLIT; i++)
            dyn_bits += (uint32_t)e->lfreq[i] * e->llen[i];
        for (i = 0; i < NDIST; i++)
            dyn_bits += (uint32_t)e->dfreq[i] * e->dlen[i];
        dynamic = dyn_bits < fixed_bits;
    }

    /* the block, the three header bits of an empty stored block, padding */
    bits = (dynamic ? dyn_bits : fixed_bits) + 3u;
    if ((bits + 7u) / 8u > max)
        return 0;
    w.p = out; w.n = 0; w.acc = 0; w.cnt = 0;
    put(&w, 0, 1);                              /* BFINAL: there is always more */
    if (dynamic) {
        e->blocks_dynamic++;
        huff_codes(e->llen, NLIT, e->lcode);
        huff_codes(e->dlen, NDIST, e->dcode);
        huff_codes(cl_len, NCL, cl_code);
        put(&w, BT_DYNAMIC, 2);
        put(&w, hlit - 257, 5);
        put(&w, hdist - 1, 5);
        put(&w, hclen - 4, 4);
        for (i = 0; i < hclen; i++)
            put(&w, cl_len[cl_order[i]], 3);
        for (i = 0; i < nops; i++) {
            put(&w, cl_code[cl_sym[i]], cl_len[cl_sym[i]]);
            if (cl_sym[i] == 16) put(&w, cl_xv[i], 2);
            else if (cl_sym[i] == 17) put(&w, cl_xv[i], 3);
            else if (cl_sym[i] == 18) put(&w, cl_xv[i], 7);
        }
        put_tokens(e, &w, e->lcode, e->llen, e->dcode, e->dlen, NULL);
    } else {
        e->blocks_fixed++;
        put(&w, BT_FIXED, 2);
        put_tokens(e, &w, e->fix_lcode, e->fix_llen, NULL, NULL, e->fix_dcode);
    }
    put(&w, 0, 3);                              /* BFINAL 0, BTYPE 00 */
    if (w.cnt)
        put(&w, 0, 8 - w.cnt);
    return w.n;
}

/* ================================================================== */
/* decompressor                                                        */

void ppp_deflate_dec_init(ppp_deflate_dec_t *d)
{
    d->wpos = 0;
    d->whave = 0;
}

void ppp_deflate_dec_add(ppp_deflate_dec_t *d, const uint8_t *a, size_t an,
                         const uint8_t *b, size_t bn)
{
    int part;

    for (part = 0; part < 2; part++) {
        const uint8_t *p = part ? b : a;
        size_t n = part ? bn : an;

        if (n > PPP_DEFLATE_WSIZE) {            /* only the end can matter */
            p += n - PPP_DEFLATE_WSIZE;
            n = PPP_DEFLATE_WSIZE;
        }
        while (n > 0) {
            size_t k = PPP_DEFLATE_WSIZE - d->wpos;
            if (k > n) k = n;
            memcpy(d->win + d->wpos, p, k);
            d->wpos = (d->wpos + (uint32_t)k) & PMASK;
            d->whave += (uint32_t)k;
            p += k;
            n -= k;
        }
    }
    if (d->whave > PPP_DEFLATE_WSIZE)
        d->whave = PPP_DEFLATE_WSIZE;
}

/* The input: the packet, then the four octets the sender left off
 * (RFC 1979 2.1). Reading past those is an error. */
typedef struct {
    const uint8_t *in;
    size_t   n, pos;
    uint32_t acc;
    unsigned cnt;
    int      bad;
} bitr_t;

static unsigned get(bitr_t *r, unsigned nbits)
{
    static const uint8_t tail[4] = { 0x00, 0x00, 0xff, 0xff };
    unsigned v;

    while (r->cnt < nbits) {
        if (r->pos >= r->n + 4) {
            r->bad = 1;
            return 0;
        }
        r->acc |= (uint32_t)(r->pos < r->n ? r->in[r->pos] : tail[r->pos - r->n]) << r->cnt;
        r->pos++;
        r->cnt += 8;
    }
    v = r->acc & ((1u << nbits) - 1);
    r->acc >>= nbits;
    r->cnt -= nbits;
    return v;
}

/* 3.2.2 from the reader's side. Returns -1 for lengths that describe more
 * codes than there can be. A code with gaps is taken as it is (a single
 * distance code is one, 3.2.7); reading one of the gaps is an error then. */
static int code_build(ppp_deflate_code_t *c, uint16_t *sym, const uint8_t *len, int n)
{
    unsigned code = 0, at = 0;
    uint16_t fill[16];
    int i;

    memset(c->count, 0, sizeof c->count);
    for (i = 0; i < n; i++)
        c->count[len[i]]++;
    c->count[0] = 0;
    c->first[0] = 0; c->index[0] = 0;
    for (i = 1; i <= 15; i++) {
        code = (code + c->count[i - 1]) << 1;
        if (code + c->count[i] > (1u << i))
            return -1;
        c->first[i] = (uint16_t)code;
        c->index[i] = (uint16_t)at;
        fill[i] = (uint16_t)at;
        at += c->count[i];
    }
    for (i = 0; i < n; i++)
        if (len[i])
            sym[fill[len[i]]++] = (uint16_t)i;
    return 0;
}

static int code_read(bitr_t *r, const ppp_deflate_code_t *c, const uint16_t *sym)
{
    unsigned code = 0, len;

    for (len = 1; len <= 15; len++) {
        code = code << 1 | get(r, 1);
        if (r->bad)
            return -1;
        if (code >= c->first[len] && code - c->first[len] < c->count[len])
            return sym[c->index[len] + (code - c->first[len])];
    }
    return -1;
}

/* 3.2.7: the two codes of a dynamic block */
static int read_dynamic(ppp_deflate_dec_t *d, bitr_t *r)
{
    uint8_t lens[288 + 32], cl[NCL];
    ppp_deflate_code_t cc;
    uint16_t csym[NCL];
    unsigned hlit = get(r, 5) + 257, hdist = get(r, 5) + 1, hclen = get(r, 4) + 4, i, prev = 0;

    if (r->bad || hlit > NLIT || hdist > NDIST)
        return -1;
    memset(cl, 0, sizeof cl);
    for (i = 0; i < hclen; i++)
        cl[cl_order[i]] = (uint8_t)get(r, 3);
    if (r->bad || code_build(&cc, csym, cl, NCL) != 0)
        return -1;
    for (i = 0; i < hlit + hdist; ) {
        int s = code_read(r, &cc, csym);
        unsigned rep, v;

        if (s < 0)
            return -1;
        if (s < 16) {
            lens[i++] = (uint8_t)s;
            prev = (unsigned)s;
            continue;
        }
        if (s == 16) {
            if (i == 0)
                return -1;                      /* nothing to repeat */
            rep = 3 + get(r, 2); v = prev;
        } else if (s == 17) {
            rep = 3 + get(r, 3); v = 0; prev = 0;
        } else {
            rep = 11 + get(r, 7); v = 0; prev = 0;
        }
        if (r->bad || i + rep > hlit + hdist)
            return -1;
        while (rep-- > 0)
            lens[i++] = (uint8_t)v;
    }
    if (lens[EOB] == 0)
        return -1;                              /* the block could not end */
    if (code_build(&d->lc, d->lsym, lens, (int)hlit) != 0 ||
        code_build(&d->dc, d->dsym, lens + hlit, (int)hdist) != 0)
        return -1;
    return 0;
}

int ppp_deflate_decompress(ppp_deflate_dec_t *d, const uint8_t *in, size_t n,
                           uint8_t *out, size_t max)
{
    bitr_t r;
    size_t o = 0;

    if (n == 0)
        return -1;
    r.in = in; r.n = n; r.pos = 0; r.acc = 0; r.cnt = 0; r.bad = 0;
    for (;;) {
        unsigned final = get(&r, 1), type = get(&r, 2);

        /* 3.2.3. A final block would end the stream for good; a sender
         * that keeps its history (RFC 1979) never sets the bit. */
        if (r.bad || final || type == 3)
            return -1;
        if (type == 0) {
            unsigned len, nlen;

            get(&r, r.cnt);                     /* to the octet boundary */
            len = get(&r, 16);
            nlen = get(&r, 16);
            if (r.bad || (len ^ nlen) != 0xffffu)
                return -1;
            if (len == 0) {
                /* an empty stored block with nothing after it: the end of
                 * the packet (RFC 1979 2.1) */
                if (r.pos == r.n + 4)
                    break;
                continue;
            }
            /* the data must be the packet's own, not the four octets we added */
            if (r.pos + len > r.n || len > max - o)
                return -1;
            memcpy(out + o, in + r.pos, len);
            r.pos += len;
            o += len;
            continue;
        }
        if (type == BT_FIXED) {
            uint8_t lens[288];
            int i;

            for (i = 0; i < 288; i++)
                lens[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
            code_build(&d->lc, d->lsym, lens, 288);
            memset(lens, 5, 32);
            code_build(&d->dc, d->dsym, lens, 32);
        } else if (read_dynamic(d, &r) != 0) {
            return -1;
        }
        for (;;) {
            int s = code_read(&r, &d->lc, d->lsym);
            unsigned len, dist;

            if (s < 0)
                return -1;
            if (s < 256) {
                if (o >= max)
                    return -1;
                out[o++] = (uint8_t)s;
                continue;
            }
            if (s == EOB)
                break;
            if (s >= NLIT)
                return -1;
            len = len_base[s - 257] + get(&r, len_extra[s - 257]);
            s = code_read(&r, &d->dc, d->dsym);
            if (s < 0 || s >= NDIST)
                return -1;
            dist = dist_base[s] + get(&r, dist_extra[s]);
            if (r.bad || dist > o + d->whave || len > max - o)
                return -1;
            /* the part that lies before this packet comes from the window */
            while (dist > o && len > 0) {
                out[o] = d->win[(d->wpos - (uint32_t)(dist - o)) & PMASK];
                o++; len--;
            }
            while (len-- > 0) {
                out[o] = out[o - dist];
                o++;
            }
        }
    }
    ppp_deflate_dec_add(d, out, o, NULL, 0);
    return (int)o;
}
