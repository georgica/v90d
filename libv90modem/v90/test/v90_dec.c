/* Reference decoder, test code only. See v90_dec.h. */
#include <string.h>
#include "v90_dec.h"
#include "v90_pcm.h"

void v90dec_init(v90dec_t *d, const v90_map_t *m, int alaw)
{
    memset(d, 0, sizeof *d);
    d->m = m;
    d->alaw = alaw;
}

int v90dec_symbols(v90dec_t *d, const uint8_t u[6], const uint8_t sign[6], uint8_t *bits)
{
    const v90_map_t *m = d->m;
    int S = 6 - m->Sr, i, j, k, n = 0;
    int label[6];
    uint64_t r;

    /* signs -> s bits */
    if (m->Sr == 0) {
        for (i = 0; i < 6; i++) {
            bits[n++] = (uint8_t)(sign[i] ^ d->prev[0]);
            d->prev[0] = sign[i];
        }
    } else {
        int F = 6 / m->Sr;
        for (j = 0; j < m->Sr; j++) {
            uint8_t q[6], x;
            for (k = 0; k < F; k++) {
                q[k] = (uint8_t)(sign[j * F + k] ^ d->prev[k]);
                d->prev[k] = sign[j * F + k];
            }
            x = q[0];
            for (k = 1; k < F; k++) {
                if (k & 1) {
                    bits[n++] = (uint8_t)(q[k] ^ d->qodd ^ (k == 1 ? x : 0));
                    d->qodd = q[k];
                } else {
                    bits[n++] = (uint8_t)(q[k] ^ x);
                }
            }
        }
    }
    if (n != S) return -3;

    /* Ucodes -> labels -> R0 -> b bits */
    for (i = 0; i < 6; i++) {
        label[i] = -1;
        for (k = 0; k < m->M[i]; k++) if (m->C[i][k] == u[i]) label[i] = k;
        if (label[i] < 0) return -1;
    }
    r = 0;
    for (i = 5; i >= 0; i--) r = r * m->M[i] + (uint64_t)label[i];
    if (m->K < 64 && (r >> m->K) != 0) return -2;
    for (i = 0; i < m->K; i++) bits[n++] = (uint8_t)((r >> i) & 1);
    return n;
}

int v90dec_frame(v90dec_t *d, const uint8_t oct[6], uint8_t *bits)
{
    uint8_t u[6], s[6];
    int i;
    for (i = 0; i < 6; i++) {
        u[i] = (uint8_t)v90_octet_ucode(d->alaw, oct[i]);
        s[i] = (uint8_t)v90_octet_sign(oct[i]);
    }
    return v90dec_symbols(d, u, s, bits);
}

void v90dec_sign_init(v90dec_sign_t *d)
{
    v90_scr_init(&d->scr);
    d->prev = 0;
}

int v90dec_trn1d(v90dec_sign_t *d, int sign)
{
    d->prev = (uint8_t)(sign & 1);
    return v90_descramble_gpc(&d->scr, sign & 1);
}

int v90dec_jd(v90dec_sign_t *d, int sign)
{
    int c = (sign ^ d->prev) & 1;
    d->prev = (uint8_t)(sign & 1);
    return v90_descramble_gpc(&d->scr, c);
}
