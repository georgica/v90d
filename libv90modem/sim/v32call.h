/* Test peer: a V.32bis CALLING modem (V.32bis 6.1, 7.1), in double, x86 test
 * side. Written from the Recommendation independently of v32/: its transmit
 * signals come straight from the hand-transcribed tables of sim/v32spec.h
 * (signal space diagrams, Tables 1 and 2), its own gate-level reading of the
 * trellis encoder (Figure 1), its own scramblers and its own receiver, so
 * that it checks the answering modem against the Recommendation and not
 * against itself. */
#ifndef V90D_SIM_V32CALL_H
#define V90D_SIM_V32CALL_H
#include <stdint.h>

/* ---------------- transmitter: any line signal of 5.2, 5.3, 2.3 ---------------- */
typedef enum {
    VC_SILENCE, VC_AA, VC_CC, VC_AC, VC_CA, VC_S, VC_SBAR, VC_TRN, VC_RATE, VC_ONES, VC_DATA
} vc_txmode;

#define VC_TXL 14          /* pulse length, symbols */
#define VC_TXD 6           /* the line signal trails the echo canceller's reference by this many symbols */
#define VC_NPTS 128

typedef struct {
    int      answer;                 /* 1: use the answer mode scrambler (to play an answerer) */
    double   level;                  /* line RMS */
    vc_txmode mode;
    double   ar[VC_TXL + VC_TXD], ai[VC_TXL + VC_TXD];   /* symbols, newest first */
    double   g[10][VC_TXL], w[10][VC_TXL];               /* pulse, and the flat reference pulse */
    int      acc; long nsamp, nsym_total;
    long     nsym;                   /* symbols sent in this mode */
    int      scr[24];                /* scrambler: the last 23 output bits, [1] newest */
    int      y1, y2;                 /* differential encoder memory */
    int      ta, tb, tc;             /* trellis encoder delay elements */
    int      rate;                   /* 0..4 */
    unsigned rword; int rbit;
    int      (*getbit)(void *); void *user;
    double   ref;                    /* echo canceller reference of the last sample */
    int      newsym;
    double   pts[5][VC_NPTS][2];     /* signal space, from v32spec.h, common scale */
} vc_tx;

void   vc_tx_init(vc_tx *t, int answer, double level, int (*getbit)(void *), void *user);
int    vc_tx_symbol_due(const vc_tx *t);
void   vc_tx_set(vc_tx *t, vc_txmode mode);
double vc_tx_tick(vc_tx *t);
/* samples from the tick that generates a symbol to its pulse centre on the line */
#define VC_TX_DELAY ((VC_TXD + VC_TXL / 2) * 10.0 / 3)

/* ---------------- the calling modem ---------------- */
typedef enum {
    VCS_AA,          /* sending AA, waiting for AC and its first phase reversal */
    VCS_AA64,        /* the 64 T before answering the reversal */
    VCS_CC,          /* sending CC, waiting for the second reversal */
    VCS_LISTEN,      /* silent: S, S-bar, TRN, R1 from the answerer */
    VCS_S_NT,        /* S for NT */
    VCS_S, VCS_SBAR, VCS_TRN,
    VCS_R2,          /* until R3 */
    VCS_E, VCS_B1,   /* B1: scrambled ones until the answerer's E + 128 T */
    VCS_DATA,
    VCS_FAILED
} vc_state;

typedef struct {
    double   level;
    unsigned rates;                  /* V32_R* style mask: bit 0 = 4800 .. bit 4 = 14400 */
    int      trn_len;                /* TRN symbols (>= 1280) */
    int      (*getbit)(void *);
    void     (*putbit)(void *, int);
    void    *user;
} vc_cfg;

#define VC_EC_NEAR 96
#define VC_EC_FAR  96
#define VC_EC_HIST 16384
#define VC_EQN 32
#define VC_RXP 32
#define VC_RXN 41
#define VC_VDEPTH 28

typedef struct {
    vc_cfg   cfg;
    vc_state state;
    vc_tx    tx;
    long     nsamp, seg, t_state;
    /* echo canceller */
    double   xh[VC_EC_HIST]; int xpos;
    double   hn[VC_EC_NEAR], hf[VC_EC_FAR];
    int      bulk; double mu;
    double   ec_in, ec_out;          /* smoothed powers */
    /* receiver front end */
    double   ring[64]; int rpos;
    double   bank[VC_RXP][VC_RXN][2];
    double   tnext, t_integ;
    double   lr, li, ur, ui;         /* band-edge resonators */
    int      half;
    double   yr[8], yi[8]; int ypos;
    double   pw;                     /* smoothed |y|^2 */
    double   ac_m, ac_p; int ac_run; /* the AC tones: metric, power; half symbols present */
    int      rev_count; long rev_t; int rev_hold;
    double   c4, tqs; int s_run;
    int      rxmode;                 /* 0 detectors, 1 S, 2 training, 3 4-point, 4 data */
    double   gain; double agc_acc; int agc_n;
    double   xr[VC_EQN], xi[VC_EQN], wr[VC_EQN], wi[VC_EQN];
    double   theta, freq;
    double   o1r, o1i, o2r, o2i; int sbar_neg;
    long     rsym; int ref_idx; int ref_scr[24];
    int      dscr[24];
    int      py1, py2;               /* differential decoder memory */
    uint64_t bits; int nbits, frame_bit;
    unsigned rate_word; int rate_seen; unsigned e_word;
    double   mse;
    int      rx_rate; long clamp;
    /* Viterbi */
    double   pm[8]; int vh[32][8]; int vpos; long vn;
    int      vnext[8][4], vy0[8];
    /* start-up record */
    long     t_rev1, t_rev2;         /* arrival of the answerer's reversals (line time, samples) */
    long     t_cc;                   /* our AA-CC transition leaving (pulse centre) */
    long     nt;                     /* round trip measured, samples */
    unsigned r1, r3;
    int      n_train;                /* trainings completed (R1, then R3) */
    long     t_s[4], t_sbar[4], t_rate[4];   /* per training: S detected, S-bar detected, rate signal detected */
    double   mse_at_rate[4];
    int      rate;                   /* agreed rate index */
    int      tx_ready, rx_ready, connected, bitrate;
    int      retrains;
    long trn_run;                 /* length of our TRN: the configured one plus the round trip */
    double   erle_db;
    double   zr, zi;                 /* last equalised point */
} v32call_t;

void   v32call_init(v32call_t *c, const vc_cfg *cfg);
double v32call_tick(v32call_t *c, double rx);
/* data mode: start a retrain (7.1) */
int    v32call_retrain(v32call_t *c);

#endif
