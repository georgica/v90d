/* V.90 start-up sequence codecs. See v90_seq.h.
 *
 * All four framed sequences share one layout: 17 ones of frame sync, then
 * 16-bit words each preceded by a start bit 0, the last word being the CRC,
 * then fill. The writer and reader below walk that layout; a "word" call
 * always accounts for exactly 16 information bits.
 *
 * CRC reading (10.1.2.3.2/V.34, Figure 14): the register is drawn with cells
 * 15..0 and the data entering at the bit 0 end. Read as: feedback = data XOR
 * cell 0, the register shifts towards cell 0, the feedback enters cells 15,
 * 10 and 3. This is the reading modem/info.c uses for the INFO sequences. */
#include <string.h>
#include "v90_seq.h"

uint16_t v90_crc_step(uint16_t crc, int bit)
{
    int fb = (bit ^ crc) & 1;
    crc >>= 1;
    if (fb) crc ^= 0x8408;
    return crc;
}

uint16_t v90_crc(const uint8_t *bits, int n)
{
    uint16_t c = 0xFFFF;
    int k;
    for (k = 0; k < n; k++) c = v90_crc_step(c, bits[k]);
    return c;
}

int v90_find_sync(const uint8_t *bits, int n, int from)
{
    int i, run = 0;
    if (from < 0) from = 0;
    /* count the run of ones that ends just before `from` so that a sync
     * straddling it is judged on its true length */
    for (i = from; i > 0 && bits[i - 1]; i--) run++;
    for (i = from; i < n; i++) {
        if (bits[i]) { run++; continue; }
        if (run == 17 && i - 17 >= from) return i - 17;
        run = 0;
    }
    return -1;
}

/* ---- bit writer --------------------------------------------------------- */

typedef struct { uint8_t *b; int n; uint16_t crc; } wr_t;

static void w_raw(wr_t *w, unsigned v, int nbits)
{
    int k;
    for (k = 0; k < nbits; k++) w->b[w->n++] = (uint8_t)((v >> k) & 1);
}

static void w_info(wr_t *w, unsigned v, int nbits)
{
    int k;
    for (k = 0; k < nbits; k++) {
        int bit = (int)((v >> k) & 1);
        w->b[w->n++] = (uint8_t)bit;
        w->crc = v90_crc_step(w->crc, bit);
    }
}

static void w_head(wr_t *w, uint8_t *bits)
{
    w->b = bits; w->n = 0; w->crc = 0xFFFF;
    w_raw(w, 0x1FFFF, 17);
}

static void w_start(wr_t *w) { w_raw(w, 0, 1); }

static void w_word(wr_t *w, unsigned v) { w_start(w); w_info(w, v, 16); }

/* start bit, CRC, `fill` zero fill bits; returns the total length */
static int w_tail(wr_t *w, int fill)
{
    uint16_t c = w->crc;
    w_start(w);
    w_raw(w, c, 16);
    w_raw(w, 0, fill);
    return w->n;
}

/* ---- bit reader --------------------------------------------------------- */

typedef struct { const uint8_t *b; int n, pos; uint16_t crc; int err; } rd_t;

static unsigned r_info(rd_t *r, int nbits)
{
    unsigned v = 0;
    int k;
    if (r->err) return 0;
    if (r->pos + nbits > r->n) { r->err = V90_ERR_SHORT; return 0; }
    for (k = 0; k < nbits; k++) {
        int bit = r->b[r->pos++] & 1;
        v |= (unsigned)bit << k;
        r->crc = v90_crc_step(r->crc, bit);
    }
    return v;
}

/* expect `nbits` bits all equal to `val` (start and fill bits, frame sync) */
static void r_fixed(rd_t *r, int val, int nbits)
{
    int k;
    if (r->err) return;
    if (r->pos + nbits > r->n) { r->err = V90_ERR_SHORT; return; }
    for (k = 0; k < nbits; k++)
        if ((r->b[r->pos++] & 1) != val) { r->err = V90_ERR_SYNC; return; }
}

static void r_head(rd_t *r, const uint8_t *bits, int n)
{
    r->b = bits; r->n = n; r->pos = 0; r->crc = 0xFFFF; r->err = 0;
    r_fixed(r, 1, 17);
}

static void r_start(rd_t *r) { r_fixed(r, 0, 1); }

static unsigned r_word(rd_t *r) { r_start(r); return r_info(r, 16); }

/* start bit, CRC, fill. Returns the length or the error. A wrong start or
 * fill bit is reported in preference to a CRC error only if it comes first. */
static int r_tail(rd_t *r, int fill)
{
    uint16_t want = r->crc;
    unsigned got;
    r_start(r);
    got = r_info(r, 16);
    if (!r->err && got != want) r->err = V90_ERR_CRC;
    r_fixed(r, 0, fill);
    return r->err ? r->err : r->pos;
}

/* ---- Jd ------------------------------------------------------------------ */

int v90_jd_build(const v90_jd_t *jd, uint8_t *bits)
{
    wr_t w;
    if (jd->rates >> V90_NUM_RATES) return V90_ERR_RANGE;
    if (jd->max_ld > 3) return V90_ERR_RANGE;
    w_head(&w, bits);
    w_word(&w, jd->rates & 0xFFFF);                      /* 18:33 */
    w_start(&w);                                         /* 34 */
    w_info(&w, (jd->rates >> 16) & 0x3F, 6);             /* 35:40 */
    w_info(&w, 0, 6);                                    /* 41:46 reserved */
    w_info(&w, jd->cp16_train ? 1 : 0, 1);               /* 47 */
    w_info(&w, jd->cp16_reneg ? 1 : 0, 1);               /* 48 */
    w_info(&w, jd->max_ld, 2);                           /* 49:50 */
    return w_tail(&w, 4);                                /* 51, 52:67, 68:71 */
}

int v90_jd_parse(const uint8_t *bits, int n, v90_jd_t *jd)
{
    rd_t r;
    v90_jd_t t;
    int len;
    memset(&t, 0, sizeof t);
    r_head(&r, bits, n);
    t.rates = r_word(&r);
    r_start(&r);
    t.rates |= (uint32_t)r_info(&r, 6) << 16;
    (void)r_info(&r, 6);
    t.cp16_train = (uint8_t)r_info(&r, 1);
    t.cp16_reneg = (uint8_t)r_info(&r, 1);
    t.max_ld = (uint8_t)r_info(&r, 2);
    len = r_tail(&r, 4);
    if (len > 0) *jd = t;
    return len;
}

/* ---- MP ------------------------------------------------------------------ */

int v90_mp_build(const v90_mp_t *mp, uint8_t *bits)
{
    wr_t w;
    int i;
    if (mp->type > 1 || mp->max_rate > 15 || mp->trellis > 3 || mp->rates > 0x1FFF)
        return V90_ERR_RANGE;
    w_head(&w, bits);
    w_start(&w);                                         /* 17 */
    w_info(&w, mp->type, 1);                             /* 18 */
    w_info(&w, 0, 5);                                    /* 19:23 reserved */
    w_info(&w, mp->max_rate, 4);                         /* 24:27 */
    w_info(&w, 0, 1);                                    /* 28 reserved */
    w_info(&w, mp->trellis, 2);                          /* 29:30 */
    w_info(&w, mp->nonlinear ? 1 : 0, 1);                /* 31 */
    w_info(&w, mp->shaping ? 1 : 0, 1);                  /* 32 */
    w_info(&w, mp->ack ? 1 : 0, 1);                      /* 33 */
    w_start(&w);                                         /* 34 */
    w_info(&w, 0, 1);                                    /* 35 reserved */
    w_info(&w, mp->rates, 13);                           /* 36:48 */
    w_info(&w, 0, 2);                                    /* 49, 50 reserved */
    if (mp->type) {
        for (i = 0; i < 3; i++) {
            w_word(&w, (uint16_t)mp->h[i][0]);
            w_word(&w, (uint16_t)mp->h[i][1]);
        }
    }
    w_word(&w, 0);                                       /* reserved */
    return w_tail(&w, 1);
}

int v90_mp_parse(const uint8_t *bits, int n, v90_mp_t *mp)
{
    rd_t r;
    v90_mp_t t;
    int len, i;
    memset(&t, 0, sizeof t);
    r_head(&r, bits, n);
    r_start(&r);
    t.type = (uint8_t)r_info(&r, 1);
    (void)r_info(&r, 5);
    t.max_rate = (uint8_t)r_info(&r, 4);
    (void)r_info(&r, 1);
    t.trellis = (uint8_t)r_info(&r, 2);
    t.nonlinear = (uint8_t)r_info(&r, 1);
    t.shaping = (uint8_t)r_info(&r, 1);
    t.ack = (uint8_t)r_info(&r, 1);
    r_start(&r);
    (void)r_info(&r, 1);
    t.rates = (uint16_t)r_info(&r, 13);
    (void)r_info(&r, 2);
    if (t.type) {
        for (i = 0; i < 3; i++) {
            t.h[i][0] = (int16_t)(uint16_t)r_word(&r);
            t.h[i][1] = (int16_t)(uint16_t)r_word(&r);
        }
    }
    (void)r_word(&r);
    len = r_tail(&r, 1);
    if (len > 0) *mp = t;
    return len;
}

/* ---- DIL descriptor ------------------------------------------------------ */

/* 16 bits of a packed pattern starting at bit `at`, zero beyond `len` bits */
static unsigned pat_word(const uint8_t *p, int at, int len)
{
    unsigned v = 0;
    int k;
    for (k = 0; k < 16; k++) {
        int m = at + k;
        if (m < len && ((p[m >> 3] >> (m & 7)) & 1)) v |= 1u << k;
    }
    return v;
}

int v90_dil_build(const v90_dil_t *d, uint8_t *bits)
{
    wr_t w;
    int i, len;
    if (d->lsp < 1 || d->lsp > 128 || d->ltp < 1 || d->ltp > 128) return V90_ERR_RANGE;
    for (i = 0; i < 8; i++)
        if (d->h[i] > 127 || d->ref[i] > 127) return V90_ERR_RANGE;
    for (i = 0; i < d->n; i++)
        if (d->train[i] > 127) return V90_ERR_RANGE;
    w_head(&w, bits);
    w_word(&w, d->n);                                    /* 18:25 N, 26:33 reserved */
    w_word(&w, (unsigned)(d->lsp - 1) | ((unsigned)(d->ltp - 1) << 8));   /* 35:50 */
    for (i = 0; i < d->lsp; i += 16) w_word(&w, pat_word(d->sp, i, d->lsp));
    for (i = 0; i < d->ltp; i += 16) w_word(&w, pat_word(d->tp, i, d->ltp));
    for (i = 0; i < 8; i += 2) w_word(&w, (unsigned)d->h[i] | ((unsigned)d->h[i + 1] << 8));
    for (i = 0; i < 8; i += 2) w_word(&w, (unsigned)d->ref[i] | ((unsigned)d->ref[i + 1] << 8));
    for (i = 0; i < d->n; i += 2)
        w_word(&w, (unsigned)d->train[i] | (i + 1 < d->n ? (unsigned)d->train[i + 1] << 8 : 0));
    len = w_tail(&w, 1);
    if (len & 1) { w_raw(&w, 0, 1); len++; }             /* keep the length even */
    return len;
}

static void pat_store(uint8_t *p, int at, unsigned v, int len)
{
    int k;
    for (k = 0; k < 16; k++) {
        int m = at + k;
        if (m < len && ((v >> k) & 1)) p[m >> 3] |= (uint8_t)(1u << (m & 7));
    }
}

int v90_dil_parse(const uint8_t *bits, int n, v90_dil_t *d)
{
    rd_t r;
    unsigned v;
    int i, len, nseg, lsp, ltp;
    /* The lengths come first and decide the layout, so parse into the
     * caller's struct only after the CRC has passed: first pass validates. */
    r_head(&r, bits, n);
    nseg = (int)(r_word(&r) & 0xFF);
    v = r_word(&r);
    lsp = (int)(v & 0x7F) + 1;
    ltp = (int)((v >> 8) & 0x7F) + 1;
    if (r.err) return r.err;
    for (i = 0; i < lsp; i += 16) (void)r_word(&r);
    for (i = 0; i < ltp; i += 16) (void)r_word(&r);
    for (i = 0; i < 8; i++) (void)r_word(&r);
    for (i = 0; i < nseg; i += 2) (void)r_word(&r);
    len = r_tail(&r, 1);
    if (len < 0) return len;
    if (len & 1) { r_fixed(&r, 0, 1); if (r.err) return r.err; len++; }

    memset(d, 0, sizeof *d);
    d->n = (uint8_t)nseg; d->lsp = (uint8_t)lsp; d->ltp = (uint8_t)ltp;
    r_head(&r, bits, n);
    (void)r_word(&r); (void)r_word(&r);
    for (i = 0; i < lsp; i += 16) pat_store(d->sp, i, r_word(&r), lsp);
    for (i = 0; i < ltp; i += 16) pat_store(d->tp, i, r_word(&r), ltp);
    for (i = 0; i < 8; i += 2) {
        v = r_word(&r);
        d->h[i] = (uint8_t)(v & 0x7F); d->h[i + 1] = (uint8_t)((v >> 8) & 0x7F);
    }
    for (i = 0; i < 8; i += 2) {
        v = r_word(&r);
        d->ref[i] = (uint8_t)(v & 0x7F); d->ref[i + 1] = (uint8_t)((v >> 8) & 0x7F);
    }
    for (i = 0; i < nseg; i += 2) {
        v = r_word(&r);
        d->train[i] = (uint8_t)(v & 0x7F);
        if (i + 1 < nseg) d->train[i + 1] = (uint8_t)((v >> 8) & 0x7F);
    }
    return len;
}

/* ---- CP ------------------------------------------------------------------ */

int v90_cp_nconst(const v90_cp_t *cp)
{
    int i, m = 0;
    for (i = 0; i < 6; i++) if (cp->idx[i] > m) m = cp->idx[i];
    return m + 1;
}

static void w_const(wr_t *w, const uint8_t *mask)
{
    int k;
    for (k = 0; k < 8; k++) w_word(w, (unsigned)mask[2 * k] | ((unsigned)mask[2 * k + 1] << 8));
}

static void r_const(rd_t *r, uint8_t *mask)
{
    int k;
    for (k = 0; k < 8; k++) {
        unsigned v = r_word(r);
        mask[2 * k] = (uint8_t)(v & 0xFF);
        mask[2 * k + 1] = (uint8_t)(v >> 8);
    }
}

int v90_cp_build(const v90_cp_t *cp, uint8_t *bits)
{
    wr_t w;
    int i, nc;
    if (cp->drn > 31 || cp->sr > 3 || cp->ld > 3 || cp->rates > 0x1FFF) return V90_ERR_RANGE;
    for (i = 0; i < 6; i++) if (cp->idx[i] > 5) return V90_ERR_RANGE;
    nc = v90_cp_nconst(cp);
    w_head(&w, bits);
    w_start(&w);                                         /* 17 */
    w_info(&w, 0, 1);                                    /* 18 reserved */
    w_info(&w, cp->is_cp ? 1 : 0, 1);                    /* 19 */
    w_info(&w, cp->drn, 5);                              /* 20:24 */
    w_info(&w, 0, 5);                                    /* 25:29 reserved */
    w_info(&w, cp->silence ? 1 : 0, 1);                  /* 30 */
    w_info(&w, cp->sr, 2);                               /* 31:32 */
    w_info(&w, cp->ack ? 1 : 0, 1);                      /* 33 */
    w_start(&w);                                         /* 34 */
    w_info(&w, cp->alaw ? 1 : 0, 1);                     /* 35 */
    w_info(&w, cp->rates, 13);                           /* 36:48 */
    w_info(&w, cp->ld, 2);                               /* 49:50 */
    w_word(&w, cp->rms_ratio);                           /* 51, 52:67 */
    w_word(&w, (unsigned)(uint8_t)cp->a1 | ((unsigned)(uint8_t)cp->a2 << 8));   /* 68, 69:84 */
    w_word(&w, (unsigned)(uint8_t)cp->b1 | ((unsigned)(uint8_t)cp->b2 << 8));   /* 85, 86:101 */
    w_word(&w, (unsigned)cp->idx[0] | ((unsigned)cp->idx[1] << 4) |
               ((unsigned)cp->idx[2] << 8) | ((unsigned)cp->idx[3] << 12));     /* 102, 103:118 */
    w_word(&w, (unsigned)cp->idx[4] | ((unsigned)cp->idx[5] << 4) |
               (cp->codec_differs ? 0x100u : 0));        /* 119, 120:135 */
    for (i = 0; i < nc; i++) w_const(&w, cp->mask[i]);
    if (cp->codec_differs)
        for (i = 0; i < nc; i++) w_const(&w, cp->codec_mask[i]);
    return w_tail(&w, 3);
}

int v90_cp_parse(const uint8_t *bits, int n, v90_cp_t *cp)
{
    rd_t r;
    v90_cp_t *t = cp;
    unsigned v;
    int i, nc, len;

    /* cp is filled in place (it is too large to copy through the stack) and
     * cleared again on any error. */
    memset(t, 0, sizeof *t);
    r_head(&r, bits, n);
    r_start(&r);
    (void)r_info(&r, 1);
    t->is_cp = (uint8_t)r_info(&r, 1);
    t->drn = (uint8_t)r_info(&r, 5);
    (void)r_info(&r, 5);
    t->silence = (uint8_t)r_info(&r, 1);
    t->sr = (uint8_t)r_info(&r, 2);
    t->ack = (uint8_t)r_info(&r, 1);
    r_start(&r);
    t->alaw = (uint8_t)r_info(&r, 1);
    t->rates = (uint16_t)r_info(&r, 13);
    t->ld = (uint8_t)r_info(&r, 2);
    t->rms_ratio = (uint16_t)r_word(&r);
    v = r_word(&r);
    t->a1 = (int8_t)(uint8_t)(v & 0xFF); t->a2 = (int8_t)(uint8_t)(v >> 8);
    v = r_word(&r);
    t->b1 = (int8_t)(uint8_t)(v & 0xFF); t->b2 = (int8_t)(uint8_t)(v >> 8);
    v = r_word(&r);
    for (i = 0; i < 4; i++) t->idx[i] = (uint8_t)((v >> (4 * i)) & 0xF);
    v = r_word(&r);
    t->idx[4] = (uint8_t)(v & 0xF); t->idx[5] = (uint8_t)((v >> 4) & 0xF);
    t->codec_differs = (uint8_t)((v >> 8) & 1);
    if (r.err) { len = r.err; goto fail; }
    /* the indices decide the length, so they are checked before the CRC */
    for (i = 0; i < 6; i++) if (t->idx[i] > 5) { len = V90_ERR_RANGE; goto fail; }
    nc = v90_cp_nconst(t);
    for (i = 0; i < nc; i++) r_const(&r, t->mask[i]);
    if (t->codec_differs)
        for (i = 0; i < nc; i++) r_const(&r, t->codec_mask[i]);
    /* Table 14 has three fill bits; a Conexant CX93001 sends one (290 bits
     * in all). The CRC decides; of the fill, what is there is taken. */
    len = r_tail(&r, 0);
    if (len > 0) {
        for (i = 0; i < 3 && r.pos < r.n && !(r.b[r.pos] & 1); i++) r.pos++;
        return r.pos;
    }
fail:
    memset(t, 0, sizeof *t);
    return len;
}

/* ---- E ------------------------------------------------------------------- */

int v90_e_build(uint8_t *bits)
{
    int i;
    for (i = 0; i < V90_E_BITS; i++) bits[i] = 1;
    return V90_E_BITS;
}

int v90_e_match(const uint8_t *bits, int n)
{
    int i;
    if (n < V90_E_BITS) return 0;
    for (i = 0; i < V90_E_BITS; i++) if (!bits[i]) return 0;
    return 1;
}
