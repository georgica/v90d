/* Checks of the data mode parameters and the index -> Ki relation of
 * V.92 6.4.2. See v92.h. */
#include "v92.h"
#include "v92_mod.h"

int v92_map_check(const v92_map_t *m)
{
    v92_mod_t mod;
    int i, e;

    if (m->trellis > V92_TRELLIS_64) return V92_ERR_RANGE;
    if (v92_mod_init(&mod, m->M, m->K) != 0) return V92_ERR_RANGE;
    for (i = 0; i < V92_CFRAME; i++) {
        unsigned c = m->cidx[i];
        const uint16_t *p;
        if (c >= V92_NCONST) return V92_ERR_RANGE;
        if (m->lc[c] < 1 || m->lc[c] > V92_MAX_LC) return V92_ERR_RANGE;
        p = m->pt[c];
        if (p[0] == 0 || p[m->lc[c] - 1] > 32767) return V92_ERR_RANGE;
        for (e = 1; e < m->lc[c]; e++)
            if (p[e] <= p[e - 1]) return V92_ERR_RANGE;
    }
    for (i = 0; i < V92_DFRAME; i++) {
        int n = 2 * v92_lc(m, i);
        int need = (i % V92_TFRAME == 3) ? 2 * m->M[i] : m->M[i];
        if (need > n) return V92_ERR_RANGE;
    }
    return 0;
}

int v92_eta_to_k(const v92_map_t *m, int i, int eta)
{
    int mi = m->M[i], r;
    if (i % V92_TFRAME == 3)
        eta = (eta - (eta % 2 != 0)) / 2;   /* floor(eta / 2): drop the parity */
    r = eta % mi;
    return r < 0 ? r + mi : r;
}

int v92_filt_check(const v92_filt_t *f)
{
    if (f->lz1 > V92_MAX_LSEC || f->lp1 > V92_MAX_LSEC ||
        f->lz2 > V92_MAX_LSEC || f->lp2 > V92_MAX_LSEC) return V92_ERR_RANGE;
    if ((unsigned)f->lz1 + f->lp1 + f->lz2 + f->lp2 > V92_MAX_LTOT) return V92_ERR_RANGE;
    if (f->lz2 == 0 || f->gain4 == 0) return V92_ERR_RANGE;
    return 0;
}
