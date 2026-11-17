/* Viterbi decoder for the 8-state trellis code of Figure 1/V.32 bis, and the
 * differential decoder (Tables 1 and 2). */
#include "v32_priv.h"

void v32_vit_init(v32_vit_t *v, int rate)
{
    int s, y, k;
    v->rate = rate;
    /* 6.1, 6.2: the encoder's delay elements start at zero */
    for (s = 0; s < 8; s++) v->pm[s] = s ? (1 << 28) : 0;
    for (k = 0; k < 32; k++) for (s = 0; s < 8; s++) v->hist[k][s] = 0;
    for (s = 0; s < 8; s++) for (y = 0; y < 4; y++) v->next[s][y] = (uint8_t)v32_trellis_next(s, y);
    v->pos = 0; v->n = 0;
}

int v32_vit_step(v32_vit_t *v, int32_t zr, int32_t zi, int *hard)
{
    const int8_t (*c)[2] = v32_constel(v->rate);
    int npts = 1 << (3 + v->rate), sh = v->rate, i, s, y, best;
    int32_t bm[8], nm[8], lim = 16 * V32_UNIT, min;
    int bl[8];
    uint16_t *h = v->hist[v->pos];

    if (zr > lim) zr = lim; else if (zr < -lim) zr = -lim;
    if (zi > lim) zi = lim; else if (zi < -lim) zi = -lim;
    for (i = 0; i < 8; i++) { bm[i] = 0x7fffffff; bl[i] = 0; }
    /* nearest point of each subset Y0 Y1 Y2 */
    for (i = 0; i < npts; i++) {
        int32_t dx = zr - c[i][0] * V32_UNIT, dy = zi - c[i][1] * V32_UNIT;
        int32_t d = (dx >> 2) * (dx >> 2) + (dy >> 2) * (dy >> 2);
        int k = i >> sh;
        if (d < bm[k]) { bm[k] = d; bl[k] = i; }
    }
    best = 0;
    for (i = 1; i < 8; i++) if (bm[i] < bm[best]) best = i;
    if (hard) *hard = bl[best];

    for (s = 0; s < 8; s++) nm[s] = 0x7fffffff;
    for (s = 0; s < 8; s++) {
        int32_t pm = v->pm[s];
        int c0 = (s & 1) << 2;
        for (y = 0; y < 4; y++) {
            int ns = v->next[s][y];
            int32_t m = pm + bm[c0 | y];
            if (m < nm[ns]) { nm[ns] = m; h[ns] = (uint16_t)(bl[c0 | y] << 3 | s); }
        }
    }
    min = nm[0]; best = 0;
    for (s = 1; s < 8; s++) if (nm[s] < min) { min = nm[s]; best = s; }
    for (s = 0; s < 8; s++) {
        int32_t m = nm[s] - min;
        v->pm[s] = m > (1 << 28) ? (1 << 28) : m;
    }
    v->pos = (v->pos + 1) & 31;
    v->n++;
    if (v->n < V32_VDEPTH) return -1;
    s = best;
    for (i = 1; i < V32_VDEPTH; i++) s = v->hist[(v->pos - i) & 31][s] & 7;
    return v->hist[(v->pos - V32_VDEPTH) & 31][s] >> 3;
}

int v32_decode_bits(int rate, int label, int *yprev)
{
    int n = 2 + rate;
    if (rate == 0) {
        static const int q_of[4] = { 1, 0, 2, 3 };      /* quadrant change 0, +90, +180, +270 */
        int d = (v32_label_abcd[label & 3] - v32_label_abcd[*yprev & 3]) & 3;
        *yprev = label & 3;
        return q_of[d];
    } else {
        int y = (label >> (n - 2)) & 3, p = *yprev;
        int d = (((y >> 1) | ((y & 1) << 1)) - ((p >> 1) | ((p & 1) << 1))) & 3;
        *yprev = y;
        return (((d & 1) << 1) | (d >> 1)) << (n - 2) | (label & ((1 << (n - 2)) - 1));
    }
}
