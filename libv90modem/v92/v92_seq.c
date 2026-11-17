/* V.92 Phase 3 / Phase 4 sequence codecs. See v92_seq.h.
 *
 * Every framed sequence has the layout of V.90's: 17 ones of frame sync,
 * then 16-bit words each preceded by a start bit 0, the last word being the
 * CRC, then fill. The writer and reader below walk that layout the way
 * ../v90/v90_seq.c does (its helpers are private to that file); the CRC
 * register itself is v90_crc_step(). */
#include <string.h>
#include "v92_seq.h"

int v92_find_sync(const uint8_t *bits, int n, int from)
{
    int i, run = 0;
    if (from < 0) from = 0;
    /* ones just before `from` do not count: the sync must start at or
     * after it */
    for (i = from; i < n; i++) {
        if (bits[i]) { run++; continue; }
        if (run >= 17) return i - 17;
        run = 0;
    }
    return -1;
}

int v92_e_match(const uint8_t *bits, int n, int len)
{
    int i;
    if (len > n) return 0;
    for (i = 0; i < len; i++) if (bits[i] & 1) return 0;
    return 1;
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

/* start bit, CRC, the mandatory fill bit, then zeros up to a multiple of
 * `unit` bits; returns the total length */
static int w_tail(wr_t *w, int unit)
{
    uint16_t c = w->crc;
    w_start(w);
    w_raw(w, c, 16);
    w_raw(w, 0, 1);
    while (w->n % unit) w_raw(w, 0, 1);
    return w->n;
}

/* ---- bit reader --------------------------------------------------------- */

typedef struct { const uint8_t *b; int n, pos; uint16_t crc; int err; } rd_t;

static unsigned r_info(rd_t *r, int nbits)
{
    unsigned v = 0;
    int k;
    if (r->err) return 0;
    if (r->pos + nbits > r->n) { r->err = V92_ERR_SHORT; return 0; }
    for (k = 0; k < nbits; k++) {
        int bit = r->b[r->pos++] & 1;
        v |= (unsigned)bit << k;
        r->crc = v90_crc_step(r->crc, bit);
    }
    return v;
}

/* expect `nbits` bits all equal to `val` (start bits, frame sync) */
static void r_fixed(rd_t *r, int val, int nbits)
{
    int k;
    if (r->err) return;
    if (r->pos + nbits > r->n) { r->err = V92_ERR_SHORT; return; }
    for (k = 0; k < nbits; k++)
        if ((r->b[r->pos++] & 1) != val) { r->err = V92_ERR_SYNC; return; }
}

static void r_head(rd_t *r, const uint8_t *bits, int n)
{
    r->b = bits; r->n = n; r->pos = 0; r->crc = 0xFFFF; r->err = 0;
    r_fixed(r, 1, 17);
}

static void r_start(rd_t *r) { r_fixed(r, 0, 1); }

static unsigned r_word(rd_t *r) { r_start(r); return r_info(r, 16); }

/* start bit and CRC. Returns 0 or the error. */
static int r_crc(rd_t *r)
{
    uint16_t want = r->crc;
    unsigned got;
    r_start(r);
    got = r_info(r, 16);
    if (!r->err && got != want) r->err = V92_ERR_CRC;
    return r->err;
}

/* After a good CRC: take the zero fill that is there, up to the fill bit and
 * the next multiple of `unit`. Returns the length. */
static int r_fill(rd_t *r, int unit)
{
    int end = r->pos + 1;
    if (unit > 1) end += (unit - end % unit) % unit;
    while (r->pos < end && r->pos < r->n && !(r->b[r->pos] & 1)) r->pos++;
    return r->pos;
}

/* ---- Ja ------------------------------------------------------------------ */

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

static void pat_store(uint8_t *p, int at, unsigned v, int len)
{
    int k;
    for (k = 0; k < 16; k++) {
        int m = at + k;
        if (m < len && ((v >> k) & 1)) p[m >> 3] |= (uint8_t)(1u << (m & 7));
    }
}

int v92_ja_build(const v92_ja_t *ja, uint8_t *bits)
{
    const v90_dil_t *d = &ja->dil;
    wr_t w;
    int i;
    if (ja->rates >> V92_UP_NUM_RATES) return V92_ERR_RANGE;
    if (d->lsp < 1 || d->lsp > 128 || d->ltp < 1 || d->ltp > 128) return V92_ERR_RANGE;
    for (i = 0; i < 8; i++)
        if (d->h[i] > 127 || d->ref[i] > 127) return V92_ERR_RANGE;
    for (i = 0; i < d->n; i++)
        if (d->train[i] > 127) return V92_ERR_RANGE;
    /* 0 : 186 + beta + ceil(N/2) * 17 as in Table 12/V.90 */
    w_head(&w, bits);
    w_word(&w, d->n);                                    /* 18:25 N, 26:33 reserved */
    w_word(&w, (unsigned)(d->lsp - 1) | ((unsigned)(d->ltp - 1) << 8));   /* 35:50 */
    for (i = 0; i < d->lsp; i += 16) w_word(&w, pat_word(d->sp, i, d->lsp));
    for (i = 0; i < d->ltp; i += 16) w_word(&w, pat_word(d->tp, i, d->ltp));
    for (i = 0; i < 8; i += 2) w_word(&w, (unsigned)d->h[i] | ((unsigned)d->h[i + 1] << 8));
    for (i = 0; i < 8; i += 2) w_word(&w, (unsigned)d->ref[i] | ((unsigned)d->ref[i + 1] << 8));
    for (i = 0; i < d->n; i += 2)
        w_word(&w, (unsigned)d->train[i] | (i + 1 < d->n ? (unsigned)d->train[i + 1] << 8 : 0));
    /* Table 20/V.92: the rate mask takes the place of V.90's CRC */
    w_word(&w, ja->rates & 0xFFFF);                      /* 24000 .. 44000 */
    w_word(&w, (ja->rates >> 16) & 7);                   /* 45333 .. 48000, reserved */
    return w_tail(&w, 12);
}

int v92_ja_parse(const uint8_t *bits, int n, v92_ja_t *ja)
{
    v90_dil_t *d = &ja->dil;
    rd_t r;
    unsigned v;
    int i, nseg, lsp, ltp;

    /* ja is filled in place and cleared again on any error */
    memset(ja, 0, sizeof *ja);
    r_head(&r, bits, n);
    nseg = (int)(r_word(&r) & 0xFF);
    v = r_word(&r);
    lsp = (int)(v & 0x7F) + 1;
    ltp = (int)((v >> 8) & 0x7F) + 1;
    if (r.err) return r.err;
    d->n = (uint8_t)nseg; d->lsp = (uint8_t)lsp; d->ltp = (uint8_t)ltp;
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
    ja->rates = r_word(&r);
    ja->rates |= (uint32_t)(r_word(&r) & 7) << 16;
    if (r_crc(&r)) {
        memset(ja, 0, sizeof *ja);
        return r.err;
    }
    return r_fill(&r, 12);
}

/* ---- Jd, Jp -------------------------------------------------------------- */

int v92_j_kind(const uint8_t *bits, int n)
{
    if (n < 48) return V92_ERR_SHORT;
    return bits[47] & 1;
}

/* the common frame of Tables 21 and 22: word 18:33, then 35:50 as `lo` */
static int j_build(unsigned w1, unsigned w2, uint8_t *bits)
{
    wr_t w;
    w_head(&w, bits);
    w_word(&w, w1);                                      /* 17, 18:33 */
    w_word(&w, w2);                                      /* 34, 35:50 */
    w_start(&w);                                         /* 51 */
    w_raw(&w, w.crc, 16);                                /* 52:67 */
    w_raw(&w, 0, 4);                                     /* 68:71 */
    return w.n;
}

static int j_parse(const uint8_t *bits, int n, unsigned kind, unsigned *w1, unsigned *w2)
{
    rd_t r;
    r_head(&r, bits, n);
    *w1 = r_word(&r);
    *w2 = r_word(&r);
    if (r.err) return r.err;
    if (((*w2 >> 12) & 1) != kind) return V92_ERR_KIND;  /* bit 47 */
    if (r_crc(&r)) return r.err;
    r_fixed(&r, 0, 4);
    return r.err ? r.err : r.pos;
}

int v92_jd_build(const v92_jd_t *jd, uint8_t *bits)
{
    if (jd->rates >> V90_NUM_RATES) return V92_ERR_RANGE;
    if (jd->max_ld > 3) return V92_ERR_RANGE;
    /* 35:40 rates, 41:46 reserved, 47 = 0, 48 reserved, 49:50 look-ahead */
    return j_build(jd->rates & 0xFFFF,
                   ((jd->rates >> 16) & 0x3F) | ((unsigned)jd->max_ld << 14), bits);
}

int v92_jd_parse(const uint8_t *bits, int n, v92_jd_t *jd)
{
    unsigned w1, w2;
    int len = j_parse(bits, n, V92_J_JD, &w1, &w2);
    if (len < 0) return len;
    jd->rates = w1 | ((uint32_t)(w2 & 0x3F) << 16);
    jd->max_ld = (uint8_t)(w2 >> 14);
    return len;
}

int v92_jp_build(const v92_jp_t *jp, uint8_t *bits)
{
    /* 35:46 reserved, 47 = 1, 48 training, 49 renegotiation, 50 reserved */
    return j_build(jp->frac, 0x1000u | (jp->pt8_train ? 0x2000u : 0) |
                             (jp->pt8_reneg ? 0x4000u : 0), bits);
}

int v92_jp_parse(const uint8_t *bits, int n, v92_jp_t *jp)
{
    unsigned w1, w2;
    int len = j_parse(bits, n, V92_J_JP, &w1, &w2);
    if (len < 0) return len;
    jp->frac = (uint16_t)w1;
    jp->pt8_train = (uint8_t)((w2 >> 13) & 1);
    jp->pt8_reneg = (uint8_t)((w2 >> 14) & 1);
    return len;
}

/* ---- CPt, CPu ------------------------------------------------------------ */

int v92_up_kind(const uint8_t *bits, int n)
{
    int t;
    if (n < 21) return V92_ERR_SHORT;
    if (bits[18] & 1) return V92_UP_SUVU;
    t = (bits[19] & 1) | ((bits[20] & 1) << 1);
    return t == 3 ? V92_ERR_RANGE : t;
}

int v92_dn_kind(const uint8_t *bits, int n)
{
    if (n < 19) return V92_ERR_SHORT;
    return bits[18] & 1;
}

int v92_cpu_nconst(const v92_cpu_t *cp)
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

int v92_cpu_build(const v92_cpu_t *cp, int unit, uint8_t *bits)
{
    wr_t w;
    int i, nc;
    if (unit < 1) return V92_ERR_RANGE;
    if (cp->type > V92_UP_CPU || cp->drn > 31 || cp->sr > 3 || cp->ld > 3) return V92_ERR_RANGE;
    for (i = 0; i < 6; i++) if (cp->idx[i] > 5) return V92_ERR_RANGE;
    nc = v92_cpu_nconst(cp);
    w_head(&w, bits);
    w_start(&w);                                         /* 17 */
    w_info(&w, 0, 1);                                    /* 18 CP */
    w_info(&w, cp->type, 2);                             /* 19:20 */
    w_info(&w, cp->drn, 5);                              /* 21:25 */
    w_info(&w, 0, 5);                                    /* 26:30 reserved */
    w_info(&w, cp->sr, 2);                               /* 31:32 */
    w_info(&w, cp->ack ? 1 : 0, 1);                      /* 33 */
    w_start(&w);                                         /* 34 */
    w_info(&w, cp->alaw ? 1 : 0, 1);                     /* 35 */
    w_info(&w, 0, 13);                                   /* 36:48 reserved */
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
    return w_tail(&w, unit);
}

int v92_cpu_parse(const uint8_t *bits, int n, int unit, v92_cpu_t *cp)
{
    rd_t r;
    unsigned v;
    int i, nc, err;

    /* cp is filled in place and cleared again on any error */
    memset(cp, 0, sizeof *cp);
    r_head(&r, bits, n);
    r_start(&r);
    v = r_info(&r, 3);                                   /* 18, 19:20 */
    if (r.err) return r.err;
    if ((v & 1) || (v >> 1) > V92_UP_CPU) return V92_ERR_KIND;
    cp->type = (uint8_t)(v >> 1);
    cp->drn = (uint8_t)r_info(&r, 5);
    (void)r_info(&r, 5);
    cp->sr = (uint8_t)r_info(&r, 2);
    cp->ack = (uint8_t)r_info(&r, 1);
    r_start(&r);
    cp->alaw = (uint8_t)r_info(&r, 1);
    (void)r_info(&r, 13);
    cp->ld = (uint8_t)r_info(&r, 2);
    cp->rms_ratio = (uint16_t)r_word(&r);
    v = r_word(&r);
    cp->a1 = (int8_t)(uint8_t)(v & 0xFF); cp->a2 = (int8_t)(uint8_t)(v >> 8);
    v = r_word(&r);
    cp->b1 = (int8_t)(uint8_t)(v & 0xFF); cp->b2 = (int8_t)(uint8_t)(v >> 8);
    v = r_word(&r);
    for (i = 0; i < 4; i++) cp->idx[i] = (uint8_t)((v >> (4 * i)) & 0xF);
    v = r_word(&r);
    cp->idx[4] = (uint8_t)(v & 0xF); cp->idx[5] = (uint8_t)((v >> 4) & 0xF);
    cp->codec_differs = (uint8_t)((v >> 8) & 1);
    err = r.err;
    /* the indices decide the length, so they are checked before the CRC */
    for (i = 0; i < 6 && !err; i++) if (cp->idx[i] > 5) err = V92_ERR_RANGE;
    if (!err) {
        nc = v92_cpu_nconst(cp);
        for (i = 0; i < nc; i++) r_const(&r, cp->mask[i]);
        if (cp->codec_differs)
            for (i = 0; i < nc; i++) r_const(&r, cp->codec_mask[i]);
        err = r_crc(&r);
    }
    if (err) {
        memset(cp, 0, sizeof *cp);
        return err;
    }
    return r_fill(&r, unit);
}

void v92_cpu_to_v90(const v92_cpu_t *cp, v90_cp_t *out)
{
    memset(out, 0, sizeof *out);
    out->is_cp = (uint8_t)(cp->type == V92_UP_CPU);
    out->drn = cp->drn;
    out->sr = cp->sr;
    out->ack = cp->ack;
    out->alaw = cp->alaw;
    out->ld = cp->ld;
    out->rms_ratio = cp->rms_ratio;
    out->a1 = cp->a1; out->a2 = cp->a2; out->b1 = cp->b1; out->b2 = cp->b2;
    memcpy(out->idx, cp->idx, sizeof out->idx);
    out->codec_differs = cp->codec_differs;
    memcpy(out->mask, cp->mask, sizeof out->mask);
    memcpy(out->codec_mask, cp->codec_mask, sizeof out->codec_mask);
}

/* ---- the 52-bit sequences: CPus, SUVu, SUVd ------------------------------ */

/* 17 ones, start bit, word 18:33, start bit, CRC, fill */
static int short_build(unsigned word, int unit, uint8_t *bits)
{
    wr_t w;
    if (unit < 1) return V92_ERR_RANGE;
    w_head(&w, bits);
    w_word(&w, word);
    return w_tail(&w, unit);
}

/* the bits of `mask` in the word must equal `kind` */
static int short_parse(const uint8_t *bits, int n, int unit, unsigned mask,
                       unsigned kind, unsigned *word)
{
    rd_t r;
    r_head(&r, bits, n);
    *word = r_word(&r);
    if (r.err) return r.err;
    if ((*word & mask) != kind) return V92_ERR_KIND;
    if (r_crc(&r)) return r.err;
    return r_fill(&r, unit);
}

/* in the word 18:33, bit b of the table is bit b - 18 */
#define WB(b) (1u << ((b) - 18))

int v92_cpus_build(const v92_cpus_t *c, int unit, uint8_t *bits)
{
    if (c->drn > 31) return V92_ERR_RANGE;
    /* 18 = 0, 19:20 = 2, 21:25 drn, 26:32 reserved, 33 acknowledge */
    return short_build(((unsigned)V92_UP_CPUS << 1) | ((unsigned)c->drn << 3) |
                       (c->ack ? WB(33) : 0), unit, bits);
}

int v92_cpus_parse(const uint8_t *bits, int n, int unit, v92_cpus_t *c)
{
    unsigned v;
    int len = short_parse(bits, n, unit, 7, (unsigned)V92_UP_CPUS << 1, &v);
    if (len < 0) return len;
    c->drn = (uint8_t)((v >> 3) & 0x1F);
    c->ack = (uint8_t)((v >> 15) & 1);
    return len;
}

int v92_suvu_build(const v92_suvu_t *s, int unit, uint8_t *bits)
{
    if (s->level < -16 || s->level > 15) return V92_ERR_RANGE;
    /* 18 = 1, 19:25 reserved, 26 wait, 27:31 level, 32 silence, 33 acknowledge */
    return short_build(WB(18) | (s->wait_cpu ? WB(26) : 0) |
                       (((unsigned)s->level & 0x1F) << (27 - 18)) |
                       (s->silence ? WB(32) : 0) | (s->ack ? WB(33) : 0), unit, bits);
}

int v92_suvu_parse(const uint8_t *bits, int n, int unit, v92_suvu_t *s)
{
    unsigned v;
    int lev, len = short_parse(bits, n, unit, WB(18), WB(18), &v);
    if (len < 0) return len;
    s->wait_cpu = (uint8_t)((v >> (26 - 18)) & 1);
    lev = (int)((v >> (27 - 18)) & 0x1F);
    s->level = (int8_t)(lev >= 16 ? lev - 32 : lev);
    s->silence = (uint8_t)((v >> (32 - 18)) & 1);
    s->ack = (uint8_t)((v >> (33 - 18)) & 1);
    return len;
}

int v92_suvd_build(const v92_suvd_t *s, int unit, uint8_t *bits)
{
    /* 18 = 1, 19:31 reserved, 32 silence, 33 acknowledge */
    return short_build(WB(18) | (s->silence ? WB(32) : 0) | (s->ack ? WB(33) : 0),
                       unit, bits);
}

int v92_suvd_parse(const uint8_t *bits, int n, int unit, v92_suvd_t *s)
{
    unsigned v;
    int len = short_parse(bits, n, unit, WB(18), WB(18), &v);
    if (len < 0) return len;
    s->silence = (uint8_t)((v >> (32 - 18)) & 1);
    s->ack = (uint8_t)((v >> (33 - 18)) & 1);
    return len;
}

/* ---- CPd ----------------------------------------------------------------- */

/* 0 if the four section lengths fit the largest Lmax and Ltot */
static int filt_len_ok(unsigned lz1, unsigned lp1, unsigned lz2, unsigned lp2)
{
    return lz1 <= V92_MAX_LSEC && lp1 <= V92_MAX_LSEC && lz2 <= V92_MAX_LSEC &&
           lp2 <= V92_MAX_LSEC && lz1 + lp1 + lz2 + lp2 <= V92_MAX_LTOT;
}

int v92_cpd_build(const v92_cpd_t *c, int unit, uint8_t *bits)
{
    const v92_map_t *m = &c->map;
    const v92_filt_t *f = &c->filt;
    wr_t w;
    int i, s, ncoef = 0;

    if (unit < 1) return V92_ERR_RANGE;
    if (c->drn > V92_UP_NUM_RATES || m->trellis > V92_TRELLIS_64 || f->gain4 == 0)
        return V92_ERR_RANGE;
    if (c->has_filt) {
        if (!filt_len_ok(f->lz1, f->lp1, f->lz2, f->lp2)) return V92_ERR_RANGE;
        ncoef = f->lz1 + f->lp1 + f->lz2 + f->lp2;
    }
    if (c->has_const) {
        for (i = 0; i < V92_CFRAME; i++) if (m->cidx[i] >= V92_NCONST) return V92_ERR_RANGE;
        if (m->lc[0] == 0) return V92_ERR_RANGE;
        for (s = 0; s < V92_NCONST; s++) {
            if (m->lc[s] > V92_MAX_LC) return V92_ERR_RANGE;
            if (s > 0 && m->lc[s] && !m->lc[s - 1]) return V92_ERR_RANGE;
            for (i = 0; i < m->lc[s]; i++) if (m->pt[s][i] == 0) return V92_ERR_RANGE;
        }
    }

    w_head(&w, bits);
    w_start(&w);                                         /* 17 */
    w_info(&w, 0, 1);                                    /* 18 CPd */
    w_info(&w, c->has_mod ? 1 : 0, 1);                   /* 19 */
    w_info(&w, c->has_filt ? 1 : 0, 1);                  /* 20 */
    w_info(&w, c->has_const ? 1 : 0, 1);                 /* 21 */
    w_info(&w, c->drn, 5);                               /* 22:26 */
    w_info(&w, m->trellis, 2);                           /* 27:28 */
    w_info(&w, c->e2u_extend ? 1 : 0, 1);                /* 29 */
    w_info(&w, 0, 3);                                    /* 30:32 reserved */
    w_info(&w, c->ack ? 1 : 0, 1);                       /* 33 */
    w_word(&w, f->gain4);                                /* 34, 35:50 */
    if (c->has_mod)                                      /* 51:152 */
        for (i = 0; i < V92_DFRAME; i += 2)
            w_word(&w, (unsigned)m->M[i] | ((unsigned)m->M[i + 1] << 8));
    if (c->has_filt) {
        w_word(&w, f->lz1);                              /* 153, 154:162, reserved */
        w_word(&w, f->lp1);                              /* 170 */
        w_word(&w, f->lz2);                              /* 187 */
        w_word(&w, f->lp2);                              /* 204 */
        for (i = 0; i < ncoef; i++) w_word(&w, (uint16_t)f->coef[i]);
    }
    if (c->has_const) {
        w_word(&w, (unsigned)m->cidx[0] | ((unsigned)m->cidx[1] << 4) |
                   ((unsigned)m->cidx[2] << 8) | ((unsigned)m->cidx[3] << 12));
        w_word(&w, (unsigned)m->cidx[4] | ((unsigned)m->cidx[5] << 4));
        for (s = 0; s < V92_NCONST; s += 2)
            w_word(&w, (unsigned)m->lc[s] | ((unsigned)m->lc[s + 1] << 8));
        for (s = 0; s < V92_NCONST; s++)
            for (i = 0; i < m->lc[s]; i++) w_word(&w, m->pt[s][i]);
    }
    return w_tail(&w, unit);
}

int v92_cpd_parse(const uint8_t *bits, int n, int unit, v92_cpd_t *c)
{
    v92_map_t *m = &c->map;
    v92_filt_t *f = &c->filt;
    rd_t r;
    unsigned v;
    int i, s, err;

    /* c is filled in place (it is too large to copy through the stack) and
     * cleared again on any error */
    memset(c, 0, sizeof *c);
    r_head(&r, bits, n);
    r_start(&r);
    v = r_info(&r, 1);
    if (r.err) return r.err;
    if (v) return V92_ERR_KIND;
    c->has_mod = (uint8_t)r_info(&r, 1);
    c->has_filt = (uint8_t)r_info(&r, 1);
    c->has_const = (uint8_t)r_info(&r, 1);
    c->drn = (uint8_t)r_info(&r, 5);
    m->trellis = (uint8_t)r_info(&r, 2);
    c->e2u_extend = (uint8_t)r_info(&r, 1);
    (void)r_info(&r, 3);
    c->ack = (uint8_t)r_info(&r, 1);
    f->gain4 = (uint16_t)r_word(&r);
    if (c->has_mod)
        for (i = 0; i < V92_DFRAME; i += 2) {
            v = r_word(&r);
            m->M[i] = (uint8_t)(v & 0xFF); m->M[i + 1] = (uint8_t)(v >> 8);
        }
    err = r.err;
    if (!err && c->has_filt) {
        unsigned l[4];
        for (i = 0; i < 4; i++) l[i] = r_word(&r) & 0x1FF;
        err = r.err;
        /* the lengths decide the layout, so they are checked before the CRC */
        if (!err && !filt_len_ok(l[0], l[1], l[2], l[3])) err = V92_ERR_RANGE;
        if (!err) {
            int ncoef = (int)(l[0] + l[1] + l[2] + l[3]);
            f->lz1 = (uint16_t)l[0]; f->lp1 = (uint16_t)l[1];
            f->lz2 = (uint16_t)l[2]; f->lp2 = (uint16_t)l[3];
            for (i = 0; i < ncoef; i++) f->coef[i] = (int16_t)(uint16_t)r_word(&r);
            err = r.err;
        }
    }
    if (!err && c->has_const) {
        v = r_word(&r);
        for (i = 0; i < 4; i++) m->cidx[i] = (uint8_t)((v >> (4 * i)) & 0xF);
        v = r_word(&r);
        m->cidx[4] = (uint8_t)(v & 0xF); m->cidx[5] = (uint8_t)((v >> 4) & 0xF);
        for (s = 0; s < V92_NCONST; s += 2) {
            v = r_word(&r);
            m->lc[s] = (uint8_t)(v & 0xFF); m->lc[s + 1] = (uint8_t)(v >> 8);
        }
        err = r.err;
        for (s = 0; s < V92_NCONST && !err; s++)
            if (m->lc[s] > V92_MAX_LC) err = V92_ERR_RANGE;
        for (s = 0; s < V92_NCONST && !err; s++)
            for (i = 0; i < m->lc[s]; i++) m->pt[s][i] = (uint16_t)r_word(&r);
    }
    if (!err) err = r_crc(&r);
    if (err) {
        memset(c, 0, sizeof *c);
        return err;
    }
    m->K = (uint8_t)(c->drn >= 1 && c->drn <= V92_UP_NUM_RATES ? V92_UP_K(c->drn) : 0);
    return r_fill(&r, unit);
}
