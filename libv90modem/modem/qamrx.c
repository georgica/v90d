/* QAM receiver core. See qamrx.h. */
#include "qamrx.h"
#include "sintab.h"
#include "v34_tabs.h"

#define ONE24 (1 << 24)

void qamrx_init(qamrx_t *r, int sym_rate, int car_num, int car_den)
{
    int i;
    r->step = (int32_t)(((int64_t)8000 << 24) / (3 * sym_rate));
    r->dcar = (uint32_t)((((uint64_t)car_num) << 32) / (uint32_t)car_den);
    for (i = 0; i < 64; i++) r->ring[i] = 0;
    r->rpos = 0; r->pw = 0; r->gain = 256; r->running = 0;
    r->tnext = 0; r->t_integ = 0; r->m = 0;
    for (i = 0; i < 2 * QRX_NTAP; i++) r->x[i] = 0;
    r->xpos = 0;
    for (i = 0; i < QRX_NTAP; i++) r->cr[i] = r->ci[i] = 0;
    r->yr = r->yi = r->ymr = r->ymi = r->er = r->ei = 0;
    r->dnorm = 1; r->timing_on = 0;
    r->theta = 0; r->freq = 0;
    r->zr = r->zi = 0; r->nsym = 0; r->mse = 65536; r->mse_acc = (int64_t)65536 << 8;
}

static int32_t isqrt64(int64_t v)
{
    int64_t r = 0, b = (int64_t)1 << 62;
    if (v <= 0) return 0;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int32_t)r;
}

void qamrx_sync(qamrx_t *r, int32_t delay_q8)
{
    int i;
    int32_t rms = isqrt64(r->pw);
    int64_t t;
    /* AGC: bring the input to an RMS of 2048 */
    if (rms < 8) rms = 8;
    r->gain = (2048 * 256) / rms;
    if (r->gain > 16384) r->gain = 16384;
    if (r->gain < 16) r->gain = 16;
    for (i = 0; i < QRX_NTAP; i++) r->cr[i] = r->ci[i] = 0;
    for (i = 0; i < 2 * QRX_NTAP; i++) r->x[i] = 0;
    r->t_integ = 0; r->theta = 0; r->freq = 0; r->nsym = 0; r->mse = 65536;
    r->mse_acc = (int64_t)65536 << 8;
    r->timing_on = 0; r->er = r->ei = 0;
    /* output grid: tau = t_sym0 + m * step. After the next push the evaluable
     * instant is (that sample - V34_ID); pick the first grid point not before it.
     * tnext counts from the evaluable instant, and is decremented on the push. */
    t = ((int64_t)delay_q8 << 16) + ((int64_t)V34_ID << 24) + ONE24;
    r->m = 0;
    while (t < ONE24) { t += r->step; r->m++; }
    while (t - r->step >= ONE24) { t -= r->step; r->m--; }
    r->tnext = (int32_t)t;
    r->running = 1;
}

int qamrx_centroid(const qamrx_t *r)
{
    int64_t num = 0, den = 0;
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        int64_t e = ((int64_t)r->cr[k] * r->cr[k] + (int64_t)r->ci[k] * r->ci[k]) >> 16;
        num += e * k; den += e;
    }
    return den ? (int)((num * 256) / den) : QRX_CENTER * 256;
}

static void eq_out(const qamrx_t *r, int32_t *yr, int32_t *yi)
{
    const int16_t *x = &r->x[r->xpos];
    int64_t ar = 0, ai = 0;
    int k;
    for (k = 0; k < QRX_NTAP; k++) {
        ar += (int64_t)r->cr[k] * x[k];
        ai += (int64_t)r->ci[k] * x[k];
    }
    *yr = (int32_t)(ar >> 20); *yi = (int32_t)(ai >> 20);
}

/* one T/3 sample out of the interpolator and through the equaliser; returns 1
 * when it was the on-time sample of a symbol */
static int produce(qamrx_t *r)
{
    int got = 0;
    {
        int32_t f = -r->tnext;                      /* 0 .. ONE24-1 behind the evaluable instant */
        int p = (f >> 19) & (V34_IP - 1);
        int32_t a = (f >> 4) & 0x7fff;              /* position between branches p and p+1, Q15 */
        const int16_t *h0 = v34_interp[p], *h1 = v34_interp[p + 1];
        int32_t acc0 = 0, acc1 = 0, v, ph;
        int i, idx = r->rpos - 1;
        /* 32 branches alone leave a timing granularity of 1/32 sample, which
         * costs several dB at the band edge; interpolate between neighbours */
        for (i = 0; i < V34_IN; i++) {
            int32_t s = r->ring[(idx - i) & 63];
            acc0 += s * h0[i];
            acc1 += s * h1[i];
        }
        acc0 >>= 14; acc1 >>= 14;
        v = (acc0 + (((acc1 - acc0) * a) >> 15)) * r->gain >> 8;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        /* push into the equaliser line (newest first, doubled buffer) */
        if (--r->xpos < 0) r->xpos = QRX_NTAP - 1;
        r->x[r->xpos] = r->x[r->xpos + QRX_NTAP] = (int16_t)v;

        ph = (r->m - QRX_CENTER) % 3;
        if (ph < 0) ph += 3;
        if (r->m >= QRX_CENTER - 1) {
            if (ph == 2) {
                eq_out(r, &r->ymr, &r->ymi);        /* a third of a symbol early */
            } else if (ph == 0) {
                int32_t c, s;
                eq_out(r, &r->yr, &r->yi);
                s = sintab[(r->theta >> 22) & SIN_MASK];
                c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
                r->zr = (int32_t)(((int64_t)r->yr * c + (int64_t)r->yi * s) >> 15);
                r->zi = (int32_t)(((int64_t)r->yi * c - (int64_t)r->yr * s) >> 15);
                r->nsym++;
                got = 1;
            } else if (r->timing_on) {
                /* a third late: timing gradient Re{e conj(z+ - z-)} / |d|^2, with
                 * z+ and z- the outputs a third of a symbol late and early, each
                 * derotated by the carrier phase of its own instant (theta has
                 * already moved on one symbol since the on-time sample) */
                int32_t ypr, ypi, zpr, zpi, zmr, zmi, c, s;
                uint32_t third = r->dcar / 3, th;
                int64_t g;
                int32_t gn;
                eq_out(r, &ypr, &ypi);
                th = r->theta - 2 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zpr = (int32_t)(((int64_t)ypr * c + (int64_t)ypi * s) >> 15);
                zpi = (int32_t)(((int64_t)ypi * c - (int64_t)ypr * s) >> 15);
                th = r->theta - 4 * third;
                s = sintab[(th >> 22) & SIN_MASK]; c = sintab[((th >> 22) + SIN_N / 4) & SIN_MASK];
                zmr = (int32_t)(((int64_t)r->ymr * c + (int64_t)r->ymi * s) >> 15);
                zmi = (int32_t)(((int64_t)r->ymi * c - (int64_t)r->ymr * s) >> 15);
                g = (int64_t)r->er * (zpr - zmr) + (int64_t)r->ei * (zpi - zmi);
                gn = (int32_t)((g << 12) / r->dnorm);       /* Q12 */
                if (gn > 4096) gn = 4096;
                if (gn < -4096) gn = -4096;
                /* second-order loop, time constant about 130 symbols */
                if (r->timing_fast) {
                    r->tnext += gn * 256 + r->t_integ;
                } else {
                    r->t_integ += gn / 4;
                    if (r->t_integ > ONE24 / 256) r->t_integ = ONE24 / 256;
                    if (r->t_integ < -ONE24 / 256) r->t_integ = -ONE24 / 256;
                    r->tnext += gn * 32 + r->t_integ;
                }
                r->timing_on = 0;
            }
        }
        r->m++;
        r->tnext += r->step;
    }
    return got;
}

int qamrx_tick(qamrx_t *r, int16_t xin)
{
    /* Two T/3 instants can fall within one input sample. When the first of
     * them completed a symbol, the second was left for now, so that the caller's
     * qamrx_update() saw the equaliser line as it was for that symbol. */
    if (r->running) while (r->tnext <= 0) (void)produce(r);

    r->ring[r->rpos] = xin;
    r->rpos = (r->rpos + 1) & 63;
    r->pw += (((int64_t)xin * xin) - r->pw) / 256;
    if (!r->running) return 0;

    r->tnext -= ONE24;
    while (r->tnext <= 0) {
        if (produce(r)) return 1;
    }
    return 0;
}

void qamrx_update(qamrx_t *r, int32_t dr, int32_t di, int flags, int mu_shift)
{
    int32_t er = dr - r->zr, ei = di - r->zi, c, s, pr, pi;
    int64_t dn = (int64_t)dr * dr + (int64_t)di * di, e2;
    int k;
    if (flags & QRX_DENSE) dn = (int64_t)2 * QRX_REF * QRX_REF;
    if (dn < 1) dn = 1;
    e2 = (((int64_t)er * er + (int64_t)ei * ei) << 16) / dn;
    if (e2 > (4 << 16)) e2 = 4 << 16;
    r->mse_acc += ((e2 << 8) - r->mse_acc) / 64;
    r->mse = (int32_t)(r->mse_acc >> 8);

    /* rotate the error back to the pass-band */
    s = sintab[(r->theta >> 22) & SIN_MASK];
    c = sintab[((r->theta >> 22) + SIN_N / 4) & SIN_MASK];
    pr = (int32_t)(((int64_t)er * c - (int64_t)ei * s) >> 15);
    pi = (int32_t)(((int64_t)ei * c + (int64_t)er * s) >> 15);
    r->er = er; r->ei = ei;             /* base-band error, for the timing loop */
    r->dnorm = (int32_t)(dn > 0x7fffffff ? 0x7fffffff : dn);
    r->timing_on = (flags & QRX_TIMING) != 0;
    r->timing_fast = (flags & QRX_TFAST) != 0;

    if (flags & QRX_EQ) {
        const int16_t *x = &r->x[r->xpos];
        /* rounded, not truncated: a shift alone rounds every update down, and
         * half an LSB per symbol on every tap is a drift of 0.14 in 90 s */
        const int32_t half = (int32_t)1 << (mu_shift - 1);
        for (k = 0; k < QRX_NTAP; k++) {
            r->cr[k] += (pr * x[k] + half) >> mu_shift;
            r->ci[k] += (pi * x[k] + half) >> mu_shift;
        }
    }
    if (flags & QRX_CARRIER) {
        /* phase error Im{z conj(d)} / |d|^2 in rad, Q16 */
        int64_t num = (int64_t)r->zi * dr - (int64_t)r->zr * di;
        int32_t phi = (int32_t)((num << 16) / dn);
        if (phi > 32768) phi = 32768;
        if (phi < -32768) phi = -32768;
        r->freq += phi * 16;
        r->theta += (uint32_t)(phi * 1043);
    }
    r->theta += r->dcar + (uint32_t)r->freq;

    /* The equaliser can absorb any timing phase, so the timing loop alone does
     * not pin the taps in place. Every 1024 symbols nudge the sampling instant
     * towards keeping the taps' centre of gravity in the middle; the equaliser
     * follows. One tap of offset moves the clock by 1/100 sample. */
    if ((flags & QRX_TIMING) && (r->nsym & 1023) == 0) {
        int32_t c = qamrx_centroid(r) - QRX_CENTER * 256;
        if (c > 512) c = 512;
        if (c < -512) c = -512;
        r->tnext -= c * 655;
    }
}
