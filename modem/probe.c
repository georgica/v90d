/* L1 / L2 line probing: generator and block-DFT analyser. See probe.h. */
#include "probe.h"
#include "sintab.h"

int probe_has_tone(int m)
{
    return m >= 1 && m <= PROBE_NBIN && m != 6 && m != 8 && m != 12 && m != 16;
}

/* Table 17/V.34: tones whose initial phase is 180 degrees */
static int phase180(int m)
{
    return m == 2 || m == 11 || m == 15 || m == 18 || m == 20 || m == 21 || m == 22 || m == 23;
}

/* cos(2 pi k / 160), Q15, by linear interpolation in the sine table */
static void cos160(int16_t *cs)
{
    int k;
    for (k = 0; k < PROBE_BLK; k++) {
        uint32_t ph = (uint32_t)(((uint64_t)k << 32) / PROBE_BLK) + 0x40000000u;
        int i = (int)(ph >> 22);
        int32_t f = (int32_t)((ph >> 7) & 0x7fff);
        int32_t a = sintab[i & SIN_MASK], b = sintab[(i + 1) & SIN_MASK];
        cs[k] = (int16_t)(a + (((b - a) * f) >> 15));
    }
}

void probetx_init(probetx_t *p, int level)
{
    int16_t cs[PROBE_BLK];
    int k, m;
    cos160(cs);
    for (k = 0; k < PROBE_BLK; k++) {
        int32_t acc = 0;
        for (m = 1; m <= PROBE_NBIN; m++) {
            if (!probe_has_tone(m)) continue;
            acc += phase180(m) ? -cs[(3 * m * k) % PROBE_BLK] : cs[(3 * m * k) % PROBE_BLK];
        }
        /* each tone at amplitude level * sqrt(2/21) (10113 in Q15) */
        acc = (int32_t)(((int64_t)acc * 10113) >> 15);
        p->tab[k] = (int16_t)(((int64_t)acc * level) >> 15);
    }
    p->pos = 0;
}

int16_t probetx_tick(probetx_t *p, int l1)
{
    int32_t v = p->tab[p->pos];
    if (++p->pos == PROBE_BLK) p->pos = 0;
    if (l1) v *= 2;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

void proberx_init(proberx_t *r)
{
    int m;
    cos160(r->cs);
    r->n = 0; r->nblk = 0;
    for (m = 0; m <= PROBE_NBIN; m++) {
        r->lr[m] = r->li[m] = 0;
        r->pw[m] = r->p0[m] = r->cr[m] = r->ci[m] = 0;
    }
}

void proberx_tick(proberx_t *r, int16_t x)
{
    int m, k;
    r->blk[r->n++] = x;
    if (r->n < PROBE_BLK) return;
    r->n = 0;
    for (m = 1; m <= PROBE_NBIN; m++) {
        int64_t ar = 0, ai = 0;
        int32_t xr, xi;
        int idx = 0, step = (3 * m) % PROBE_BLK;
        for (k = 0; k < PROBE_BLK; k++) {
            ar += (int32_t)r->blk[k] * r->cs[idx];
            ai -= (int32_t)r->blk[k] * r->cs[(idx + PROBE_BLK - 40) % PROBE_BLK];
            idx += step; if (idx >= PROBE_BLK) idx -= PROBE_BLK;
        }
        xr = (int32_t)(ar >> 15); xi = (int32_t)(ai >> 15);     /* amplitude * 80 */
        r->pw[m] += (int64_t)xr * xr + (int64_t)xi * xi;
        if (r->nblk == 0) r->p0[m] = (int64_t)xr * xr + (int64_t)xi * xi;
        if (r->nblk > 0) {
            r->cr[m] += (int64_t)xr * r->lr[m] + (int64_t)xi * r->li[m];
            r->ci[m] += (int64_t)xi * r->lr[m] - (int64_t)xr * r->li[m];
        }
        r->lr[m] = xr; r->li[m] = xi;
    }
    r->nblk++;
}

static int64_t isqrt64(int64_t v)
{
    int64_t r = 0, b = (int64_t)1 << 62;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* repeating (signal) power and noise power per block at bin m, in |X|^2 units.
 * With c = sum X[k] conj(X[k-1]) over K-1 block pairs, the signal is |c|/(K-1)
 * and sum |X[k] - e^(j theta) X[k-1]|^2 = 2 sum|X|^2 - |X first|^2 - |X last|^2
 * - 2|c| is 2(K-1) times the noise, whatever the frequency offset theta. */
static void powers(const proberx_t *r, int m, int64_t *sig, int64_t *noise)
{
    int64_t a = r->cr[m] < 0 ? -r->cr[m] : r->cr[m], b = r->ci[m] < 0 ? -r->ci[m] : r->ci[m];
    /* |c| without overflow: scale down to 31 bits first */
    int sh = 0;
    while ((a >> sh) > 0x3fffffff || (b >> sh) > 0x3fffffff) sh++;
    int64_t c = isqrt64((a >> sh) * (a >> sh) + (b >> sh) * (b >> sh)) << sh;
    int64_t last = (int64_t)r->lr[m] * r->lr[m] + (int64_t)r->li[m] * r->li[m];
    *sig = c / (r->nblk - 1);
    *noise = (2 * r->pw[m] - r->p0[m] - last - 2 * c) / (2 * (r->nblk - 1));
}

int proberx_level(const proberx_t *r, int m)
{
    int64_t s, t;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    powers(r, m, &s, &t);
    return (int)isqrt64(s / 12800);
}

int proberx_last(const proberx_t *r, int m)
{
    if (r->nblk < 1 || m < 1 || m > PROBE_NBIN) return 0;
    return (int)isqrt64(((int64_t)r->lr[m] * r->lr[m] + (int64_t)r->li[m] * r->li[m]) / 12800);
}

int proberx_noise(const proberx_t *r, int m)
{
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    return (int)isqrt64(r->pw[m] / r->nblk / 12800);
}

int proberx_snr(const proberx_t *r, int m)
{
    int64_t s, n;
    int db;
    if (r->nblk < 3 || m < 1 || m > PROBE_NBIN) return 0;
    powers(r, m, &s, &n);
    if (s <= 0) return 0;
    if (n < 1) n = 1;
    db = probe_db10(s, n);
    return db < 0 ? 0 : db > 600 ? 600 : db;
}

int proberx_freq_off(const proberx_t *r)
{
    int64_t x = r->cr[7], y = r->ci[7], ax, ay, mn, mx, z, t;
    if (r->nblk < 3 || proberx_snr(r, 7) < 100) return -512;
    ax = x < 0 ? -x : x; ay = y < 0 ? -y : y;
    while (ax > 0x3fffffff || ay > 0x3fffffff) { ax >>= 1; ay >>= 1; }
    mn = ax < ay ? ax : ay; mx = ax < ay ? ay : ax;
    if (mx == 0) return -512;
    z = (mn << 15) / mx;                                    /* Q15, 0..1 */
    /* atan(z) in turns, Q20: z * (1/8 + 0.04345 * (1 - z)) */
    t = (z * (131072 + ((45560 * (32768 - z)) >> 15))) >> 15;
    if (ay > ax) t = 262144 - t;                            /* 1/4 turn - t */
    if (x < 0) t = 524288 - t;                              /* 1/2 turn - t */
    if (y < 0) t = -t;
    /* one turn per 20 ms block = 50 Hz = 2500 units of 0.02 Hz */
    t = (t * 2500) / 1048576;
    return (int)(t > 511 ? 511 : t < -511 ? -511 : t);
}

/* log2(v) in Q8 */
static int32_t log2q8(int64_t v)
{
    static const uint16_t tab[17] = { 0, 22, 44, 63, 82, 100, 118, 134, 150, 165, 179, 193,
                                      207, 220, 232, 244, 256 };
    int n = 0, i, f;
    int64_t t = v;
    if (v <= 0) return -(64 << 8);
    while (t > 1) { t >>= 1; n++; }
    /* 8 mantissa bits below the leading one */
    t = n >= 8 ? (v >> (n - 8)) & 0xff : (v << (8 - n)) & 0xff;
    i = (int)(t >> 4); f = (int)(t & 15);
    return (n << 8) + tab[i] + (((tab[i + 1] - tab[i]) * f) >> 4);
}

int probe_db10(int64_t num, int64_t den)
{
    /* 10 log10(2) = 3.0103 dB per octave -> 30.103 tenths; / 256 for Q8 */
    return (int)(((int64_t)(log2q8(num) - log2q8(den)) * 30103) / 256000);
}
