/* Is one tone most of what is on the line? The requests for a retrain are
 * such tones: Tone A, 2400 Hz, from the calling modem and Tone B, 1200 Hz,
 * from the answering one, for more than 50 ms (V.90 9.5.1.2, V.92 9.7.1.2,
 * and V.34 the same way).
 *
 * Each 10 ms block: the share of the block's power that sits at the tone's
 * frequency. A data signal leaves a few per cent there; Tone A with its
 * guard tone about 80 %. A tone of power P gives |sum|^2 = N^2 P / 2, and
 * the tone is "on" in a block that is not silent and has more than half of
 * its power there. Fixed-point, 8000 samples/s. */
#ifndef V90D_MODEM_TONEDET_H
#define V90D_MODEM_TONEDET_H
#include <stdint.h>

#define TONEDET_BLOCK   80
#define TONEDET_BLOCKS  6           /* blocks in a row: more than 50 ms */
#define TONEDET_1200    0x26666666u /* 1200 / 8000 of a turn */
#define TONEDET_2400    0x4CCCCCCDu

typedef struct {
    uint32_t ph, dph;
    int64_t  re, im, pw;
    int      n, blocks;
} tonedet_t;

void tonedet_init(tonedet_t *t, uint32_t dph);
/* one sample in; 1 while the tone has been on for TONEDET_BLOCKS blocks */
int  tonedet_tick(tonedet_t *t, int16_t x);

#endif
