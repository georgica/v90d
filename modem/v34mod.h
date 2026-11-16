/* V.34 QAM modulator: complex symbols at 2400 / 3000 / 3200 per second in,
 * real pass-band samples at 8 kHz out.
 *
 *   symbols -> root-raised-cosine pulse (10 % excess band, polyphase, so the
 *   spectrum is flat over carrier +- 0.45 S as V.34 5.4 wants) -> carrier
 *   (V.34 Table 2) -> pre-emphasis filter (5.4, Figures 1 and 2, index 0..10
 *   as the far receiver asked in INFO1) -> transmit level less the power
 *   reduction (INFO1).
 *
 * Symbols are in the units of the receiver core: a 4-point training symbol
 * is (+-V34MOD_REF, +-V34MOD_REF); a signal of that mean power (2 REF^2 per
 * symbol) leaves at the configured level whatever the pre-emphasis.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_V34MOD_H
#define V90D_MODEM_V34MOD_H
#include <stdint.h>
#include "v34mod_tabs.h"

#define V34MOD_REF 4096

typedef struct {
    const int16_t *rrc;             /* [b][V34MOD_NT] */
    const int16_t *pre;             /* half of the symmetric pre-emphasis FIR, or 0 */
    int      a, b, acc;             /* S / 8000 = a / b; position in the symbol, 1 / b */
    int16_t  hr[2 * V34MOD_NT], hi[2 * V34MOD_NT]; int hpos;    /* symbols, doubled */
    int16_t  pb[2 * V34MOD_NPRE]; int ppos;                     /* pass-band samples, doubled */
    uint32_t ph, dph;               /* carrier */
    int32_t  gain;                  /* Q12 */
    int      sym_rate;
    int32_t  nsym;                  /* symbols taken */
} v34mod_t;

/* sym_rate 2400, 3000 or 3200; carrier car_num / car_den of it; preemph
 * 0..10; level: RMS in sample units; pwr_red_db 0..7 (more is clamped).
 * Returns 0, or -1 for a symbol rate or filter that does not exist. */
int     v34mod_init(v34mod_t *m, int sym_rate, int car_num, int car_den, int preemph,
                    int level, int pwr_red_db);
/* 1 when the next v34mod_tick() begins a new symbol: give it with
 * v34mod_put() first (if it is not given, silence is sent) */
int     v34mod_due(const v34mod_t *m);
void    v34mod_put(v34mod_t *m, int32_t re, int32_t im);
int16_t v34mod_tick(v34mod_t *m);
/* The centre of a symbol is on the line this many samples (x 256) after the
 * tick that took it. */
int32_t v34mod_delay_q8(const v34mod_t *m);

#endif
