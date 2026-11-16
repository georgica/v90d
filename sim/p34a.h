/* Test peer: V.90 Phases 3 and 4 and data mode, analogue modem side (V.90
 * 9.3.2, 9.4.2), as far as a test of the digital modem needs it.
 *
 * Upstream it is a real transmitter: S, S-bar, PP, TRN, Ja, S, S-bar, (SCR or
 * silence), S, S-bar, CPt, (SCR), CP, CP', E, B1 and data through the QAM
 * modulator. Downstream it cheats: it reads the digital modem's PCM octets
 * as sent (an ideal channel) and decodes them with the reference decoder, so
 * it checks WHAT the digital modem sends and when, not how a real analogue
 * modem's equaliser would cope with the line. Host only (double precision). */
#ifndef V90D_SIM_P34A_H
#define V90D_SIM_P34A_H
#include <stdint.h>
#include "qammod.h"
#include "../v34/v34.h"
#include "../v90/v90_tx.h"
#include "../v90/test/v90_dec.h"

typedef struct {
    int alaw, uinfo;
    int sym_rate; double fc, ppm, gain;
    int sil0;               /* symbols of silence before the first S (70 ms nominal) */
    int ntrn;               /* TRN length, >= 512 */
    int jd_wait;            /* symbols between receiving Jd and sending S */
    int dil_n;              /* DIL-segments asked for (0 = no DIL) */
    int dil_syms;           /* symbols of DIL listened to before the closing S */
    int scr_in_dil;         /* SCR instead of silence while the DIL runs */
    int scr_syms;           /* SCR between CPt and CP */
    double rot2;            /* carrier phase jump after each silence, rad */
    int Kt, Mt;             /* CPt: K and points per constellation */
    int Kd, Md, sr_d, ld_d; /* CP */
    uint16_t up_rates;      /* CP bits 36:48 */
    int  (*getbit)(void *user);
    void (*putbit)(void *user, int bit);
    void *user;
} p34a_cfg;

typedef enum {
    U_SIL0, U_S1, U_SBAR1, U_PP, U_TRN, U_JA, U_SIL1, U_S2, U_SBAR2, U_DILWAIT, U_S3, U_SBAR3,
    U_CPT, U_SCR, U_CP, U_E, U_DATA,
    U_RS, U_RSBAR           /* rate renegotiation: S, S-bar, then CP again */
} p34a_ustate;

typedef enum {
    D_WAIT_SD, D_SDBAR, D_SIGN, D_DIL, D_RI, D_RIBAR, D_TRAIN, D_B1D, D_DATA,
    D_RD, D_RDBAR,          /* rate renegotiation: Rd, Rd-bar, then MP in the training map */
    D_SIL, D_RT, D_RTBAR    /* ... with silence asked for: silence, Rt, Rt-bar, then MP */
} p34a_dstate;

typedef struct {
    p34a_cfg cfg;
    qammod_t mod;
    /* upstream */
    p34a_ustate us; int un;
    v34_tmod_t tmod;
    double rot;
    uint8_t fr[V90_DIL_MAX_BITS + 8]; int frn, frpos;   /* sequence being sent */
    int fr_ack;                 /* the CP being sent has the acknowledge bit */
    int cp_ack_sent, e_left;
    v34_params_t vpar; v34_tx_t vtx; double vscale;
    /* downstream */
    p34a_dstate ds; long dn;    /* octets in this state */
    uint8_t look[12]; long nlook;
    v90dec_sign_t dt, dj;
    uint8_t sb[400]; int sbn, sones;
    int seg, segm;
    v90dec_t dec; v90_scr_t dscr;
    uint8_t fo[6]; int fon;
    uint8_t mb[V90_MP_MAX_BITS + 64]; int mbn, mones;
    int zero_frames;
    /* parameters */
    v90_dil_t dil; v90_cp_t cpt, cp; v90_map_t tmap, dmap;
    v90_jd_t jd; v90_mp_t mp;
    /* events and results */
    int sdbar_seen, jd_ok, jdp_seen, ri_seen, ribar_seen, mp_seen, mpack_seen, ed_seen;
    int trn1d_n, trn1d_bad, sign_bad, dil_n_ok, dil_bad, ri_n, ri_bad, trn2d_frames, trn2d_bad;
    int mp_n, b1d_bad, b1d_frames, dec_bad;
    int tx_ready, rx_ready, up_rate, odd_frames;
    long sd_at, tick;
    /* rate renegotiation (9.6.2) and cleardown (9.7) */
    int reneg_scr;              /* symbols of SCR after S-bar */
    int reneg_silence;          /* ask for silence: CPs, CPs', SCR, then CP (9.6.2.1.5 to .8) */
    int sil_octets, sil_bad;    /* the silence as received */
    int rt_units, rtbar_units;  /* Rt as received */
    int rd_pol, rd_units, rdbar_units, rd_bad;  /* Rd as received */
    int renegs;                 /* renegotiations completed (data both ways again) */
    int clear_seen;             /* an MP with drn = 0 came */
    int data_starts;            /* times the upstream data transmitter was started */
} p34a_t;

void   p34a_init(p34a_t *p, const p34a_cfg *cfg);
/* one PCM octet from the digital modem in, one line sample out */
double p34a_tick(p34a_t *p, uint8_t octet);
/* From data mode: start a rate renegotiation (9.6.2.1). Kd > 0 asks for a new
 * downstream K; cleardown sends CP with drn = 0 (9.7). Returns 0, or -1 when
 * not in data mode. */
int    p34a_renegotiate(p34a_t *p, int Kd, int cleardown);

#endif
