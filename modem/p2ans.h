/* V.34 Phase 2 (probing / ranging), ANSWER modem: V.34 11.2.1.2, recovery
 * 11.2.2.2, and the Phase 2 part of a retrain (11.5.2).
 *
 *   INFO0a, Tone A ... (INFO0c and Tone B from the caller) ... Tone A phase
 *   reversal ... the caller's reversal gives the round-trip delay ... our
 *   reversal 40 ms after it, 10 ms more of Tone A, L1, L2 until Tone B is
 *   heard under it ... Tone A for 50 ms, reversal, 10 ms, silence ... the
 *   caller's reversal, its L1 and L2 (analysed) ... Tone A, INFO1c from the
 *   caller, INFO1a.
 *
 * INFO1a is where the answer modem decides: the symbol rate (one for both
 * directions here), the carrier the caller shall use, and the projected
 * call -> answer rate. What the caller's receiver asks of our transmitter
 * (carrier, pre-emphasis filter, power reduction for the chosen symbol rate)
 * is taken from INFO1c and reported in tx_*.
 *
 * Sample-driven: one received sample in, one transmit sample out, 8 kHz.
 * Fixed-point, no allocation. */
#ifndef V90D_MODEM_P2ANS_H
#define V90D_MODEM_P2ANS_H
#include <stdint.h>
#include "dpsk.h"
#include "probe.h"
#include "info.h"

#define P2N_SR_2400 (1 << 0)    /* symbol rate indices of INFO1: 0 = 2400 ... 5 = 3429 */
#define P2N_SR_3000 (1 << 3)
#define P2N_SR_3200 (1 << 4)

typedef struct {
    int level;          /* nominal transmit power: RMS in 16-bit sample units */
    int io_delay;       /* round-trip delay of our own sample path to the line terminals
                           and back, samples (a driver's buffering) */
    int sr_mask;        /* symbol rates INFO1a may select; 0 = 2400, 3000 and 3200 */
    int max_rate;       /* ceiling for the projected call -> answer rate, bit/s; 0 = none */
} p2ans_cfg;

typedef enum {
    P2N_INFO0,          /* INFO0a, then Tone A; waiting for INFO0c and Tone B */
    P2N_WAIT_BREV1,     /* our reversal sent; waiting for the Tone B reversal */
    P2N_REPLY,          /* timed Tone A reversal, 10 ms more */
    P2N_L1, P2N_L2,     /* probing signals; L2 until Tone B is heard */
    P2N_TONE50,         /* Tone A for 50 ms */
    P2N_REV_TAIL,       /* reversal, 10 ms more, then silence */
    P2N_WAIT_BREV2,     /* silent; waiting for the Tone B reversal */
    P2N_PROBE_RX,       /* receiving L1 / L2 */
    P2N_WAIT_INFO1C,    /* Tone A; waiting for INFO1c */
    P2N_INFO1A,         /* sending INFO1a */
    P2N_RT_SILENCE,     /* 11.5.2.1: 70 ms of silence before Tone A (a retrain we begin) */
    P2N_DONE,
    P2N_FAILED
} p2ans_state;

enum { P2N_OK = 0,
       P2N_ERR_INFO0,       /* no INFO0c / Tone B */
       P2N_ERR_REV,         /* the Tone B phase reversals never came */
       P2N_ERR_INFO1C,      /* no INFO1c, retrains used up */
       P2N_ERR_NO_RATE };   /* INFO1c leaves no symbol rate we implement */

#define P2N_MAX_RETRAINS 2      /* retrains p2ans begins by itself before giving up */

typedef struct {
    p2ans_cfg   cfg;
    p2ans_state state;
    int         error;
    int32_t     now, t;             /* sample clock; samples in the current state */
    dpsktx_t    tx; dpskrx_t rx;
    probetx_t   ptx; proberx_t prx;
    int         probing;            /* 0 none, 1 L1, 2 L2 */
    info0_t     info0c; int got0c, resend, n_info0a;
    info1d_t    info1c;             /* INFO1c has the layout of V.90's INFO1d */
    info1a_t    info1a;             /* as sent */
    int32_t     act_at, a_rev_line, probe_from, tone_since, tone_b_at;
    int32_t     rtd;                /* round-trip delay estimate RTDEa, samples */
    int         tone_blocks;
    int         retrains;           /* begun by p2ans itself (11.2.2.2.4) */
    int         restarts;           /* returns to 11.2.1.2.3 (11.2.2.2.2, 11.2.2.2.3) */
    int         snr[PROBE_NBIN + 1];        /* per 150 Hz bin, 0.1 dB */
    int         rx_level[PROBE_NBIN + 1];
    /* ---- the outcome, valid in P2N_DONE ---- */
    int         sym_idx;            /* symbol rate index for both directions: 0, 3 or 4 */
    int         sym_rate;           /* 2400, 3000 or 3200 */
    int         rx_hi_carrier;      /* call -> answer carrier (our choice, INFO1a bit 25) */
    int         rx_snr_db10;        /* projected receiver SNR at that symbol rate, 0.1 dB */
    int         tx_hi_carrier;      /* answer -> call: what INFO1c asked for */
    int         tx_preemph;         /* pre-emphasis filter index 0..10 */
    int         tx_pwr_red;         /* power reduction, dB */
    int         md_len;             /* the caller's MD, units of 35 ms */
} p2ans_t;

void    p2ans_init(p2ans_t *p, const p2ans_cfg *cfg);
/* Phase 2 of a retrain, after the 70 ms of silence (11.5.2.1, 11.5.2.2):
 * Tone A at once, no INFO0 exchange unless the caller sends INFO0c. info0c
 * is the caller's INFO0c of the first Phase 2 (it is not sent again). */
void    p2ans_init_retrain(p2ans_t *p, const p2ans_cfg *cfg, const info0_t *info0c);
int16_t p2ans_tick(p2ans_t *p, int16_t rx);

/* V.34 Tables 1 and 2 for a symbol rate index: the rate, and the carrier as
 * the ratio num/den of it. Returns 0 for an index outside 0..5. */
int     v34_symbol_rate(int idx);
void    v34_carrier(int idx, int hi, int *num, int *den);

#endif
