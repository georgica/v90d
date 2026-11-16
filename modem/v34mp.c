/* V.34 J patterns and MP sequences. See v34mp.h.
 *
 * Layout of an MP sequence: 17 ones of frame sync, then 16-bit words each
 * preceded by a start bit 0, the last word being the CRC, then fill. The CRC
 * (10.1.2.3.2) runs over the information bits only: not the frame sync, not
 * the start bits, not the fill. It is the register of modem/info.c. */
#include "v34mp.h"
#include "info.h"

const uint8_t v34_j4[16]     = { 0,0,0,0,1,0,0,1,1,0,0,1,0,0,0,1 };
const uint8_t v34_j16[16]    = { 0,0,0,0,1,1,0,1,1,0,0,1,0,0,0,1 };
const uint8_t v34_jprime[16] = { 1,1,1,1,1,0,0,1,1,0,0,1,0,0,0,1 };

#define MAX_WORDS 10

/* the information words of a sequence, CRC excluded: 3 for Type 0, 9 for Type 1 */
static int pack(const v34_mp_t *mp, uint16_t *w)
{
    int n = 0, i;
    w[n++] = (uint16_t)((mp->type & 1)                      /* 18 */
           /* 19 reserved */
           | (unsigned)(mp->max_ca & 15) << 2               /* 20:23 */
           | (unsigned)(mp->max_ac & 15) << 6               /* 24:27 */
           | (unsigned)(mp->aux ? 1 : 0) << 10              /* 28 */
           | (unsigned)(mp->trellis & 3) << 11              /* 29:30 */
           | (unsigned)(mp->nonlinear ? 1 : 0) << 13        /* 31 */
           | (unsigned)(mp->shaping ? 1 : 0) << 14          /* 32 */
           | (unsigned)(mp->ack ? 1 : 0) << 15);            /* 33 */
    w[n++] = (uint16_t)((mp->rates & 0x3FFF)                /* 35:48, 49 reserved */
           | (unsigned)(mp->asym ? 1 : 0) << 15);           /* 50 */
    if (mp->type) {
        for (i = 0; i < 3; i++) {
            w[n++] = (uint16_t)mp->h[i][0];
            w[n++] = (uint16_t)mp->h[i][1];
        }
    }
    w[n++] = 0;                                             /* reserved word */
    return n;
}

static uint16_t words_crc(const uint16_t *w, int n)
{
    uint8_t info[16 * (MAX_WORDS - 1)];
    int i, k;
    for (i = 0; i < n; i++)
        for (k = 0; k < 16; k++) info[16 * i + k] = (uint8_t)((w[i] >> k) & 1);
    return info_crc(info, 16 * n);
}

int v34_mp_build(const v34_mp_t *mp, uint8_t *bits)
{
    uint16_t w[MAX_WORDS];
    int n, i, k, pos = 0, total;
    if (mp->type > 1 || mp->max_ca > 14 || mp->max_ac > 14 || mp->trellis > 2 || mp->rates > 0x3FFF)
        return -1;
    n = pack(mp, w);
    w[n] = words_crc(w, n);
    n++;
    for (i = 0; i < 17; i++) bits[pos++] = 1;
    for (i = 0; i < n; i++) {
        bits[pos++] = 0;
        for (k = 0; k < 16; k++) bits[pos++] = (uint8_t)((w[i] >> k) & 1);
    }
    total = mp->type ? V34_MP1_BITS : V34_MP0_BITS;
    while (pos < total) bits[pos++] = 0;
    return total;
}

int v34_mp_parse(const uint8_t *bits, int n, v34_mp_t *mp)
{
    uint16_t w[MAX_WORDS];
    int i, k, nw, need;
    for (i = 0; i < 17 && i < n; i++) if (!bits[i]) return V34_MP_BAD;
    if (n < 19) return V34_MP_SHORT;
    if (bits[17]) return V34_MP_BAD;
    nw = bits[18] ? 10 : 4;                     /* words, CRC included */
    need = 17 + 17 * nw;                        /* through the CRC */
    if (n < need) return V34_MP_SHORT;
    for (i = 0; i < nw; i++) {
        const uint8_t *p = bits + 17 + 17 * i;
        if (p[0]) return V34_MP_BAD;
        w[i] = 0;
        for (k = 0; k < 16; k++) w[i] |= (uint16_t)((p[1 + k] & 1) << k);
    }
    if (words_crc(w, nw - 1) != w[nw - 1]) return V34_MP_BAD;
    mp->type      = (uint8_t)(w[0] & 1);
    mp->max_ca    = (uint8_t)((w[0] >> 2) & 15);
    mp->max_ac    = (uint8_t)((w[0] >> 6) & 15);
    mp->aux       = (uint8_t)((w[0] >> 10) & 1);
    mp->trellis   = (uint8_t)((w[0] >> 11) & 3);
    mp->nonlinear = (uint8_t)((w[0] >> 13) & 1);
    mp->shaping   = (uint8_t)((w[0] >> 14) & 1);
    mp->ack       = (uint8_t)((w[0] >> 15) & 1);
    mp->rates     = (uint16_t)(w[1] & 0x3FFF);
    mp->asym      = (uint8_t)((w[1] >> 15) & 1);
    for (i = 0; i < 3; i++) {
        mp->h[i][0] = mp->type ? (int16_t)w[2 + 2 * i] : 0;
        mp->h[i][1] = mp->type ? (int16_t)w[3 + 2 * i] : 0;
    }
    return mp->type ? V34_MP1_BITS : V34_MP0_BITS;
}

static int best_rate(unsigned common, int limit)
{
    int k;
    for (k = 13; k >= 0; k--)
        if ((common & (1u << k)) && k + 1 <= limit) return 2400 * (k + 1);
    return 0;
}

static int min2(int a, int b) { return a < b ? a : b; }

void v34_mp_rates(const v34_mp_t *ans, const v34_mp_t *call, int *ac, int *ca)
{
    unsigned common = (unsigned)ans->rates & (unsigned)call->rates;
    int lim_ac = min2(ans->max_ac, call->max_ac), lim_ca = min2(ans->max_ca, call->max_ca);
    if (!(ans->asym && call->asym)) lim_ac = lim_ca = min2(lim_ac, lim_ca);
    *ac = best_rate(common, lim_ac);
    *ca = best_rate(common, lim_ca);
}

int v34_mp_is_cleardown(const v34_mp_t *mp)
{
    return mp->max_ca == 0 && mp->max_ac == 0;
}
