/* V.92 INFO, MH and QC sequence codecs. See v92_info.h. */
#include <string.h>
#include "v92_info.h"
#include "v90_seq.h"        /* v90_crc_step */

/* ---- INFO / MH frame ----------------------------------------------------- */

#define SYNC_LSB_FIRST 0x4E     /* 01110010, left-most bit first in time */

typedef struct { uint8_t *b; int n; uint16_t crc; } wr_t;
typedef struct { const uint8_t *b; int n, pos; uint16_t crc; int err; } rd_t;

static void put(wr_t *w, unsigned v, int nbits)
{
    int k;
    for (k = 0; k < nbits; k++) {
        int bit = (int)((v >> k) & 1);
        w->b[w->n++] = (uint8_t)bit;
        w->crc = v90_crc_step(w->crc, bit);
    }
}

static void head(wr_t *w, uint8_t *bits)
{
    w->b = bits; w->n = 0; w->crc = 0;
    put(w, 0xF, 4);
    put(w, SYNC_LSB_FIRST, 8);
    w->crc = 0xFFFF;            /* the CRC starts after the sync */
}

static int tail(wr_t *w)
{
    put(w, w->crc, 16);
    put(w, 0xF, 4);
    return w->n;
}

static unsigned get(rd_t *r, int nbits)
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

/* a two's complement field of nbits */
static int get_signed(rd_t *r, int nbits)
{
    int v = (int)get(r, nbits);
    return v >= 1 << (nbits - 1) ? v - (1 << nbits) : v;
}

static void rhead(rd_t *r, const uint8_t *bits, int n)
{
    r->b = bits; r->n = n; r->pos = 4; r->err = 0; r->crc = 0;
    if (get(r, 8) != SYNC_LSB_FIRST && !r->err) r->err = V92_ERR_SYNC;
    r->crc = 0xFFFF;
}

/* CRC; returns the frame length (with the trailing fill, whether or not it
 * is in the buffer, capped at n) or the error */
static int rtail(rd_t *r)
{
    uint16_t want = r->crc;
    unsigned got = get(r, 16);
    if (!r->err && got != want) r->err = V92_ERR_CRC;
    if (r->err) return r->err;
    return r->pos + 4 <= r->n ? r->pos + 4 : r->n;
}

int v92_info_find(const uint8_t *bits, int n, int from)
{
    int i, k;
    if (from < 0) from = 0;
    for (i = from; i + V92_INFO_HDR_BITS <= n; i++) {
        for (k = 0; k < 8; k++)
            if ((bits[i + 4 + k] & 1) != ((SYNC_LSB_FIRST >> k) & 1)) break;
        if (k == 8) return i;
    }
    return -1;
}

/* ---- INFO0 ---------------------------------------------------------------- */

static int info0_range(const v92_info0_t *i)
{
    return i->max_sr_diff > 7 || i->tx_power > 15 || i->max_tx_power > 31;
}

/* bits 12:25, common to INFO0d and INFO0a */
static void put_info0(wr_t *w, const v92_info0_t *i)
{
    put(w, i->sr2743 ? 1 : 0, 1); put(w, i->sr2800 ? 1 : 0, 1); put(w, i->sr3429 ? 1 : 0, 1);
    put(w, i->lo3000 ? 1 : 0, 1); put(w, i->hi3000 ? 1 : 0, 1);
    put(w, i->lo3200 ? 1 : 0, 1); put(w, i->hi3200 ? 1 : 0, 1);
    put(w, i->allow3429 ? 1 : 0, 1); put(w, i->pwr_reduce ? 1 : 0, 1);
    put(w, i->max_sr_diff, 3);
    put(w, i->cme ? 1 : 0, 1); put(w, i->c1664 ? 1 : 0, 1);
}

static void get_info0(rd_t *r, v92_info0_t *i)
{
    i->sr2743 = (uint8_t)get(r, 1); i->sr2800 = (uint8_t)get(r, 1); i->sr3429 = (uint8_t)get(r, 1);
    i->lo3000 = (uint8_t)get(r, 1); i->hi3000 = (uint8_t)get(r, 1);
    i->lo3200 = (uint8_t)get(r, 1); i->hi3200 = (uint8_t)get(r, 1);
    i->allow3429 = (uint8_t)get(r, 1); i->pwr_reduce = (uint8_t)get(r, 1);
    i->max_sr_diff = (uint8_t)get(r, 3);
    i->cme = (uint8_t)get(r, 1); i->c1664 = (uint8_t)get(r, 1);
}

int v92_info0d_build(const v92_info0_t *i, uint8_t *bits)
{
    wr_t w;
    if (info0_range(i)) return V92_ERR_RANGE;
    head(&w, bits);
    put_info0(&w, i);                                    /* 12:25 */
    put(&w, i->short_p2 ? 1 : 0, 1);                     /* 26 */
    put(&w, i->v92 ? 1 : 0, 1);                          /* 27 */
    put(&w, i->ack ? 1 : 0, 1);                          /* 28 */
    put(&w, i->tx_power, 4);                             /* 29:32 */
    put(&w, i->max_tx_power, 5);                         /* 33:37 */
    put(&w, i->pwr_at_codec ? 1 : 0, 1);                 /* 38 */
    put(&w, i->alaw ? 1 : 0, 1);                         /* 39 */
    put(&w, i->up3429 ? 1 : 0, 1);                       /* 40 */
    put(&w, 0, 1);                                       /* 41 reserved */
    return tail(&w);
}

int v92_info0d_parse(const uint8_t *bits, int n, v92_info0_t *i)
{
    rd_t r;
    v92_info0_t t;
    int len;
    memset(&t, 0, sizeof t);
    rhead(&r, bits, n);
    get_info0(&r, &t);
    t.short_p2 = (uint8_t)get(&r, 1);
    t.v92 = (uint8_t)get(&r, 1);
    t.ack = (uint8_t)get(&r, 1);
    t.tx_power = (uint8_t)get(&r, 4);
    t.max_tx_power = (uint8_t)get(&r, 5);
    t.pwr_at_codec = (uint8_t)get(&r, 1);
    t.alaw = (uint8_t)get(&r, 1);
    t.up3429 = (uint8_t)get(&r, 1);
    (void)get(&r, 1);
    len = rtail(&r);
    if (len > 0) *i = t;
    return len;
}

int v92_info0a_build(const v92_info0_t *i, uint8_t *bits)
{
    wr_t w;
    if (info0_range(i)) return V92_ERR_RANGE;
    head(&w, bits);
    put_info0(&w, i);                                    /* 12:25 */
    put(&w, i->v92 ? 1 : 0, 1);                          /* 26 */
    put(&w, i->short_p2 ? 1 : 0, 1);                     /* 27 */
    put(&w, i->ack ? 1 : 0, 1);                          /* 28 */
    return tail(&w);
}

int v92_info0a_parse(const uint8_t *bits, int n, v92_info0_t *i)
{
    rd_t r;
    v92_info0_t t;
    int len;
    memset(&t, 0, sizeof t);
    rhead(&r, bits, n);
    get_info0(&r, &t);
    t.v92 = (uint8_t)get(&r, 1);
    t.short_p2 = (uint8_t)get(&r, 1);
    t.ack = (uint8_t)get(&r, 1);
    len = rtail(&r);
    if (len > 0) *i = t;
    return len;
}

/* ---- INFO1d --------------------------------------------------------------- */

int v92_info1d_build(const v92_info1d_t *i, uint8_t *bits)
{
    wr_t w;
    int k;
    if (i->min_pwr_red > 7 || i->add_pwr_red > 7 || i->md_len > 127 ||
        i->freq_off < -512 || i->freq_off > 511)
        return V92_ERR_RANGE;
    for (k = 0; k < 6; k++)
        if (i->sr[k].preemph > 15 || i->sr[k].max_rate > 15) return V92_ERR_RANGE;
    head(&w, bits);
    put(&w, i->min_pwr_red, 3);                          /* 12:14 */
    put(&w, i->add_pwr_red, 3);                          /* 15:17 */
    put(&w, i->md_len, 7);                               /* 18:24 */
    for (k = 0; k < 5; k++) {                            /* 25:69, 9 bits each */
        put(&w, i->sr[k].hi_carrier ? 1 : 0, 1);
        put(&w, i->sr[k].preemph, 4);
        put(&w, i->sr[k].max_rate, 4);
    }
    put(&w, i->pcm_up ? 1 : 0, 1);                       /* 70 */
    put(&w, i->sr[5].preemph, 4);                        /* 71:74 */
    put(&w, i->sr[5].max_rate, 4);                       /* 75:78 */
    put(&w, (unsigned)i->freq_off & 0x3FF, 10);          /* 79:88 */
    return tail(&w);
}

int v92_info1d_parse(const uint8_t *bits, int n, v92_info1d_t *i)
{
    rd_t r;
    v92_info1d_t t;
    int len, k;
    memset(&t, 0, sizeof t);
    rhead(&r, bits, n);
    t.min_pwr_red = (uint8_t)get(&r, 3);
    t.add_pwr_red = (uint8_t)get(&r, 3);
    t.md_len = (uint8_t)get(&r, 7);
    for (k = 0; k < 5; k++) {
        t.sr[k].hi_carrier = (uint8_t)get(&r, 1);
        t.sr[k].preemph = (uint8_t)get(&r, 4);
        t.sr[k].max_rate = (uint8_t)get(&r, 4);
    }
    t.pcm_up = (uint8_t)get(&r, 1);
    t.sr[5].preemph = (uint8_t)get(&r, 4);
    t.sr[5].max_rate = (uint8_t)get(&r, 4);
    t.freq_off = (int16_t)get_signed(&r, 10);
    len = rtail(&r);
    if (len > 0) *i = t;
    return len;
}

/* ---- INFO1a --------------------------------------------------------------- */

int v92_info1a_build(const v92_info1a_t *i, uint8_t *bits)
{
    wr_t w;
    if (i->md_len > 127) return V92_ERR_RANGE;
    switch (i->kind) {
    case V92_INFO1A_PCM:                                 /* Table 18/V.92 */
        if (i->sections > 3 || i->ltot_code > 3 || i->lmax_code > 3 || i->uinfo > 127)
            return V92_ERR_RANGE;
        head(&w, bits);
        put(&w, i->sections, 2);                         /* 12:13 */
        put(&w, i->ltot_code, 2);                        /* 14:15 */
        put(&w, i->lmax_code, 2);                        /* 16:17 */
        put(&w, i->md_len, 7);                           /* 18:24 */
        put(&w, i->uinfo, 7);                            /* 25:31 */
        put(&w, 0, 2);                                   /* 32:33 reserved */
        put(&w, 6, 3);                                   /* 34:36 */
        put(&w, 6, 3);                                   /* 37:39 */
        put(&w, 0x3FF, 10);                              /* 40:49 set to 1 */
        break;
    case V92_INFO1A_V90:                                 /* Table 19/V.92, Table 10/V.90 */
        if (i->uinfo > 127 || i->up_sr > 5 || i->freq_off < -512 || i->freq_off > 511)
            return V92_ERR_RANGE;
        head(&w, bits);
        put(&w, 0, 6);                                   /* 12:17 reserved */
        put(&w, i->md_len, 7);                           /* 18:24 */
        put(&w, i->uinfo, 7);                            /* 25:31 */
        put(&w, 0, 1);                                   /* 32 reserved */
        put(&w, i->hi_carrier ? 1 : 0, 1);               /* 33 */
        put(&w, i->up_sr, 3);                            /* 34:36 */
        put(&w, 6, 3);                                   /* 37:39 */
        put(&w, (unsigned)i->freq_off & 0x3FF, 10);      /* 40:49 */
        break;
    case V92_INFO1A_V34:                                 /* Table 11/V.90 */
        if (i->min_pwr_red > 7 || i->add_pwr_red > 7 || i->preemph > 15 ||
            i->max_rate > 15 || i->up_sr > 5 || i->down_sr > 5 ||
            i->freq_off < -512 || i->freq_off > 511)
            return V92_ERR_RANGE;
        head(&w, bits);
        put(&w, i->min_pwr_red, 3);                      /* 12:14 */
        put(&w, i->add_pwr_red, 3);                      /* 15:17 */
        put(&w, i->md_len, 7);                           /* 18:24 */
        put(&w, i->hi_carrier ? 1 : 0, 1);               /* 25 */
        put(&w, i->preemph, 4);                          /* 26:29 */
        put(&w, i->max_rate, 4);                         /* 30:33 */
        put(&w, i->up_sr, 3);                            /* 34:36 */
        put(&w, i->down_sr, 3);                          /* 37:39 */
        put(&w, (unsigned)i->freq_off & 0x3FF, 10);      /* 40:49 */
        break;
    default:
        return V92_ERR_RANGE;
    }
    return tail(&w);
}

int v92_info1a_parse(const uint8_t *bits, int n, v92_info1a_t *i)
{
    rd_t r;
    v92_info1a_t t;
    unsigned f[8];
    int len;
    /* the same field boundaries serve all three forms:
     * 12:14 15:17 18:24 25 26:29 30:33 34:36 37:39, then 40:49 */
    rhead(&r, bits, n);
    f[0] = get(&r, 3); f[1] = get(&r, 3); f[2] = get(&r, 7); f[3] = get(&r, 1);
    f[4] = get(&r, 4); f[5] = get(&r, 4); f[6] = get(&r, 3); f[7] = get(&r, 3);
    memset(&t, 0, sizeof t);
    t.freq_off = (int16_t)get_signed(&r, 10);
    len = rtail(&r);
    if (len < 0) return len;
    if (f[6] == 7 || f[7] == 7 || (f[6] == 6 && f[7] != 6)) return V92_ERR_RANGE;
    t.md_len = (uint8_t)f[2];
    if (f[6] == 6) {
        t.kind = V92_INFO1A_PCM;
        t.sections = (uint8_t)(f[0] & 3);                /* 12:13 */
        t.ltot_code = (uint8_t)((f[0] >> 2) | ((f[1] & 1) << 1));   /* 14:15 */
        t.lmax_code = (uint8_t)(f[1] >> 1);              /* 16:17 */
        t.uinfo = (uint8_t)(f[3] | (f[4] << 1) | ((f[5] & 3) << 5));   /* 25:31 */
        t.freq_off = 0;                                  /* 40:49 are reserved */
    } else if (f[7] == 6) {
        t.kind = V92_INFO1A_V90;
        t.uinfo = (uint8_t)(f[3] | (f[4] << 1) | ((f[5] & 3) << 5));
        t.hi_carrier = (uint8_t)(f[5] >> 3);             /* 33 */
        t.up_sr = (uint8_t)f[6];
    } else {
        t.kind = V92_INFO1A_V34;
        t.min_pwr_red = (uint8_t)f[0];
        t.add_pwr_red = (uint8_t)f[1];
        t.hi_carrier = (uint8_t)f[3];
        t.preemph = (uint8_t)f[4];
        t.max_rate = (uint8_t)f[5];
        t.up_sr = (uint8_t)f[6];
        t.down_sr = (uint8_t)f[7];
    }
    *i = t;
    return len;
}

int v92_filt_allowed(const v92_filt_t *f, const v92_info1a_t *a)
{
    int lmax, ltot;
    if (a->kind != V92_INFO1A_PCM) return 0;
    lmax = V92_INFO1A_LMAX(a->lmax_code);
    ltot = V92_INFO1A_LTOT(a->ltot_code);
    if (f->lz1 && !(a->sections & 1)) return 0;
    if (f->lp2 && !(a->sections & 2)) return 0;
    if (f->lz2 < 1) return 0;
    if (f->lz1 > lmax || f->lp1 > lmax || f->lz2 > lmax || f->lp2 > lmax) return 0;
    return f->lz1 + f->lp1 + f->lz2 + f->lp2 <= ltot;
}

/* ---- MH ------------------------------------------------------------------- */

/* a 4-bit pattern as printed (left-most bit first in time) to the LSB-first
 * value put() takes, and back */
static unsigned rev4(unsigned v)
{
    return ((v & 1) << 3) | ((v & 2) << 1) | ((v & 4) >> 1) | ((v & 8) >> 3);
}

static int mh_sig_ok(unsigned s)
{
    return s == V92_MH_REQ || s == V92_MH_ACK || s == V92_MH_NACK ||
           s == V92_MH_CLRD || s == V92_MH_CDA || s == V92_MH_FRR;
}

static int mh_has_info(unsigned s) { return s == V92_MH_ACK || s == V92_MH_CLRD; }

int v92_mh_build(const v92_mh_t *m, uint8_t *bits)
{
    wr_t w;
    unsigned info = mh_has_info(m->sig) ? m->info : m->sig;
    if (!mh_sig_ok(m->sig) || info > 15) return V92_ERR_RANGE;
    head(&w, bits);
    put(&w, rev4(m->sig), 4);                            /* 12:15 */
    put(&w, rev4(info), 4);                              /* 16:19 */
    return tail(&w);
}

int v92_mh_parse(const uint8_t *bits, int n, v92_mh_t *m)
{
    rd_t r;
    unsigned sig, info;
    int len;
    rhead(&r, bits, n);
    sig = rev4(get(&r, 4));
    info = rev4(get(&r, 4));
    len = rtail(&r);
    if (len < 0) return len;
    if (!mh_sig_ok(sig)) return V92_ERR_RANGE;
    if (!mh_has_info(sig) && info != sig) return V92_ERR_RANGE;
    m->sig = (uint8_t)sig;
    m->info = (uint8_t)info;
    return len;
}

int v92_mh_t1_seconds(int code)
{
    static const int16_t t1[16] = {
        0, 10, 20, 30, 40, 60, 120, 180, 240, 360, 480, 720, 960, -1, 0, 0
    };
    return code >= 0 && code < 16 ? t1[code] : 0;
}

/* ---- QC1 / QCA1 ----------------------------------------------------------- */

/* bits 20:29 as an LSB-first value (bit 20 = bit 0 of the value) */
static int qc_frame(const v92_qc_t *q)
{
    unsigned c = q->code, v;
    v = (q->digital ? 1u << 1 : 0) | (q->qca ? 1u << 2 : 0) | (q->lapm ? 1u << 3 : 0) | (1u << 9);
    if (q->digital) {
        if (c > 3) return -1;
        v |= ((c >> 1) & 1) << 7 | (c & 1) << 8;         /* 000LM1 */
    } else {
        if (c > 15) return -1;
        v |= ((c >> 3) & 1) << 4 | ((c >> 2) & 1) << 6 | /* W0XYZ1 */
             ((c >> 1) & 1) << 7 | (c & 1) << 8;
    }
    return (int)v;
}

static void qc_put(uint8_t *b, unsigned v, int nbits)
{
    int k;
    for (k = 0; k < nbits; k++) b[k] = (uint8_t)((v >> k) & 1);
}

static unsigned qc_get(const uint8_t *b, int nbits)
{
    unsigned v = 0;
    int k;
    for (k = 0; k < nbits; k++) v |= (unsigned)(b[k] & 1) << k;
    return v;
}

#define QC_SYNC 0x2AA           /* 0101010101, left-most bit first */

int v92_qc_build(const v92_qc_t *q, uint8_t *bits)
{
    int f = qc_frame(q), k;
    if (f < 0) return V92_ERR_RANGE;
    for (k = 0; k < 2; k++) {
        qc_put(bits + 30 * k, 0x3FF, 10);
        qc_put(bits + 30 * k + 10, QC_SYNC, 10);
        qc_put(bits + 30 * k + 20, (unsigned)f, 10);
    }
    if (!q->qca) return V92_QC_BITS;
    qc_put(bits + 60, 0x3FF, 10);
    return V92_QCA_BITS;
}

int v92_qc_parse(const uint8_t *bits, int n, v92_qc_t *q)
{
    unsigned f;
    v92_qc_t t;
    int k;
    if (n < V92_QC_BITS) return V92_ERR_SHORT;
    for (k = 0; k < 2; k++)
        if (qc_get(bits + 30 * k, 10) != 0x3FF || qc_get(bits + 30 * k + 10, 10) != QC_SYNC)
            return V92_ERR_SYNC;
    f = qc_get(bits + 20, 10);
    if (f != qc_get(bits + 50, 10)) return V92_ERR_SYNC;
    if ((f & 1) || !(f & (1u << 9))) return V92_ERR_SYNC;        /* start bit, final 1 */
    t.digital = (uint8_t)((f >> 1) & 1);
    t.qca = (uint8_t)((f >> 2) & 1);
    t.lapm = (uint8_t)((f >> 3) & 1);
    if (t.digital) {
        if (f & (7u << 4)) return V92_ERR_SYNC;                  /* 000 */
        t.code = (uint8_t)(((f >> 7) & 1) << 1 | ((f >> 8) & 1));
    } else {
        if (f & (1u << 5)) return V92_ERR_SYNC;                  /* W0 */
        t.code = (uint8_t)(((f >> 4) & 1) << 3 | ((f >> 6) & 1) << 2 |
                           ((f >> 7) & 1) << 1 | ((f >> 8) & 1));
    }
    if (t.qca) {
        if (n < V92_QCA_BITS) return V92_ERR_SHORT;
        if (qc_get(bits + 60, 10) != 0x3FF) return V92_ERR_SYNC;
    }
    *q = t;
    return t.qca ? V92_QCA_BITS : V92_QC_BITS;
}

int v92_qc_uqts(int code)
{
    static const uint8_t uqts[16] = {
        61, 62, 63, 66, 67, 70, 71, 74, 75, 78, 79, 82, 83, 86, 87, 0
    };
    return code >= 0 && code < 16 ? uqts[code] : 0;
}

int v92_qc_anspcm_level(int code)
{
    static const int8_t lev[4] = { -19, -24, -30, -36 };
    return code >= 0 && code < 4 ? lev[code] : 0;
}
