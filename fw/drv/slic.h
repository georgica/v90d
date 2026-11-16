/* Bring-up and use of one Si3217x rev C FXS port of the HT802, without the
 * vendor's RAM patch.
 *
 * Sequence and constants: refs/si3217x-map.md (the public description of the
 * part's start-up) and the values read back from this board while its stock
 * firmware ran (recon/out/slic0_stock_state.txt). No vendor code.
 *
 * The caller has pulsed the part's reset line and has the PCM clocks running.
 * slic_init() takes about half a second on a healthy part and sleeps through
 * the bus's msleep. */
#ifndef V90D_FW_SLIC_H
#define V90D_FW_SLIC_H
#include "proslic.h"

enum { SLIC_PCM_ALAW = 0, SLIC_PCM_ULAW = 1, SLIC_PCM_LINEAR = 3 };

typedef struct {
    uint8_t  pcm_format;        /* SLIC_PCM_* */
    uint16_t slot;              /* start of our samples, in bit clocks from frame sync */
} slic_cfg;

enum {
    SLIC_OK = 0,
    SLIC_E_ID = -1,             /* not a Si3217x rev C */
    SLIC_E_BUS = -2,            /* a register did not read back / RAM never ready */
    SLIC_E_CAL = -3,            /* a calibration did not finish */
    SLIC_E_SHORT = -4,          /* battery under half its target 50 ms after start: converter stopped */
    SLIC_E_VBAT = -5            /* battery did not come up in 2 s: converter stopped */
};

typedef struct {
    uint8_t id;                 /* register 0 */
    int     stage;              /* how far slic_init() got (for the log) */
    long    vbat_mv;            /* battery voltage when it returned */
} slic_info;

int  slic_init(const ps_bus *b, const slic_cfg *cfg, slic_info *info);
int  slic_offhook(const ps_bus *b);             /* 1 off hook, 0 on hook */
long slic_vbat_mv(const ps_bus *b);
void slic_shutdown(const ps_bus *b);            /* line open, converter off */

/* The AC path of the part (HARDWARE.md, "Linear PCM and the gain knobs"):
 * RAM locations, and what slic_init() loads into them (1, or 0 for an
 * address that is not in its impedance set). The part's names: TX is line to
 * PCM (its A/D, what we receive), RX is PCM to line (its D/A, what we send). */
enum {
    SLIC_TXACEQ_C0 = 540,       /* ..C3 = 543: equaliser of the A/D path */
    SLIC_TXACGAIN = 544,        /* gain of the A/D path */
    SLIC_RXACGAIN = 545,        /* gain of the D/A path */
    SLIC_RXACEQ_C0 = 546,       /* ..C3 = 549: equaliser of the D/A path */
    SLIC_ECFIR_C2 = 563,        /* ..C9 = 570: hybrid balance, FIR part */
    SLIC_ECIIR_B0 = 571,        /* B1 572, A1 573, A2 574: its IIR part */
    SLIC_RXACGAIN_SAVE = 906,   /* written with the same value as RXACGAIN */
    SLIC_LINEAR_OS = 1002       /* rev C; name only in our notes, see HARDWARE.md */
};
int  slic_default_ram(unsigned addr, uint32_t *val);
/* base scaled by 10^(db10 / 200), db10 in tenths of a dB, on 29-bit two's
 * complement RAM words (sign kept; saturates at the largest magnitude) */
uint32_t slic_gain_scale(uint32_t base, int db10);

#endif
