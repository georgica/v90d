/* ANSam: modified answer tone (V.8 7.2): 2100 Hz, amplitude-modulated by a
 * 15 Hz sinewave (envelope 0.8..1.2 x average), with optional 180-degree phase
 * reversals every 450 ms (for network echo-canceller disabling).
 * The answer DCE transmits this first. Fixed-point. */
#ifndef V90D_MODEM_ANSAM_H
#define V90D_MODEM_ANSAM_H
#include <stdint.h>

#define ANSAM_REV_MS 450

typedef struct {
    uint32_t cph, dcar;    /* 2100 Hz carrier */
    uint32_t aph, dam;     /* 15 Hz AM */
    uint32_t rev;          /* current 180-deg reversal offset (0 or 2^31) */
    int32_t  rev_acc, rev_period;
    int      reversals;
    int16_t  amp;          /* average peak amplitude */
} ansam_t;

void ansam_init(ansam_t *a, int fs, int16_t amp, int reversals);
int  ansam_run(ansam_t *a, int16_t *out, int nsamp);

#endif
