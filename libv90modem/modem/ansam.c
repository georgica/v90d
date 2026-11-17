#include "ansam.h"
#include "sintab.h"

static uint32_t f2inc(int f, int fs)
{
    return (uint32_t)(((uint64_t)(uint32_t)f << 32) / (uint32_t)fs);
}

void ansam_init(ansam_t *a, int fs, int16_t amp, int reversals)
{
    a->cph = 0; a->dcar = f2inc(2100, fs);
    a->aph = 0; a->dam = f2inc(15, fs);
    a->rev = 0;
    a->rev_acc = 0;
    a->rev_period = ANSAM_REV_MS * fs / 1000;   /* 450 ms -> 3600 samples @ 8 kHz */
    a->reversals = reversals;
    a->amp = amp;
}

int ansam_run(ansam_t *a, int16_t *out, int nsamp)
{
    int i;
    for (i = 0; i < nsamp; i++) {
        int32_t sc = sintab[((a->cph + a->rev) >> 22) & SIN_MASK];
        int32_t s15 = sintab[(a->aph >> 22) & SIN_MASK];
        int32_t env = 32768 + ((6554 * s15) >> 15);   /* 1.0 + 0.2*sin, Q15 (0.8..1.2) */
        int32_t v = ((int32_t)a->amp * sc) >> 15;      /* carrier at avg amplitude */
        v = (v * env) >> 15;                           /* apply 15 Hz envelope */
        out[i] = (int16_t)v;
        a->cph += a->dcar;
        a->aph += a->dam;
        if (a->reversals && ++a->rev_acc >= a->rev_period) {
            a->rev_acc = 0;
            a->rev ^= 0x80000000u;                      /* 180-degree phase reversal */
        }
    }
    return nsamp;
}
