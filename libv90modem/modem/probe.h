/* V.34 10.1.2.4 line probing signals L1 / L2: 21 cosines 150 Hz apart from
 * 150 to 3750 Hz, with 900, 1200, 1800 and 2400 Hz left out (the Phase 2
 * tones live there, and the gaps let the receiver see noise and distortion).
 * The signal repeats every 1/150 s; three periods are exactly 160 samples.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_PROBE_H
#define V90D_MODEM_PROBE_H
#include <stdint.h>

#define PROBE_NBIN 25           /* bin m (1..25) is 150*m Hz */
#define PROBE_BLK 160           /* analysis block: 20 ms, three periods */

/* 1 when 150*m Hz carries a probing tone */
int probe_has_tone(int m);

typedef struct {
    int16_t tab[PROBE_BLK];
    int     pos;
} probetx_t;

/* level = RMS of the nominal transmit power (the level of L2) */
void    probetx_init(probetx_t *p, int level);
/* l1 != 0: 6 dB above nominal (L1). The phase runs on across L1 -> L2. */
int16_t probetx_tick(probetx_t *p, int l1);

typedef struct {
    int16_t  cs[PROBE_BLK];             /* cos(2 pi k / 160), Q15 */
    int16_t  blk[PROBE_BLK];
    int      n, nblk;
    int32_t  lr[PROBE_NBIN + 1], li[PROBE_NBIN + 1];    /* previous block's bins */
    int64_t  pw[PROBE_NBIN + 1];        /* sum |X|^2 */
    int64_t  p0[PROBE_NBIN + 1];        /* |X|^2 of the first block */
    int64_t  cr[PROBE_NBIN + 1], ci[PROBE_NBIN + 1];    /* sum X[k] conj(X[k-1]) */
} proberx_t;

void proberx_init(proberx_t *r);
void proberx_tick(proberx_t *r, int16_t x);
/* results over the blocks seen so far (at least 3 needed, else 0 / -1):
 * level of the tone or residue at 150*m Hz, RMS in sample units */
int  proberx_level(const proberx_t *r, int m);
/* signal-to-noise ratio at tone m in units of 0.1 dB (noise = the part of the
 * bin that does not repeat from block to block), clipped to 0..600 */
int  proberx_snr(const proberx_t *r, int m);
/* level at 150*m Hz in the most recent complete block alone (tone detection
 * under the echo of our own probing signal, which never falls in a gap) */
int  proberx_last(const proberx_t *r, int m);
/* noise + distortion RMS seen in the bin of a missing tone (m = 6, 8, 12, 16) */
int  proberx_noise(const proberx_t *r, int m);
/* frequency offset of the 1050 Hz tone in 0.02 Hz units (INFO1 coding);
 * -512 when it cannot be measured */
int  proberx_freq_off(const proberx_t *r);
/* fixed-point helper: 10*log10(num/den) in 0.1 dB units */
int  probe_db10(int64_t num, int64_t den);

#endif
