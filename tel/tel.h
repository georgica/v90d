/* Telephony layer: what a telephone exchange does for the line on our FXS
 * port, as far as a dialling modem (or a person with a telephone) notices.
 *
 *   on hook -> the caller lifts the receiver -> dial tone -> digits (DTMF or
 *   loop-disconnect pulses) -> the number is checked -> ringing tone and
 *   then the answer, or busy tone, or the "number not in service" tones
 *   -> the call (the owner runs the modem) -> the call ends: the caller
 *   hangs up, the far end hangs up, the line drops at random, a session
 *   limit or an idle limit runs out.
 *
 * One hook sample and one received line sample in, one sample to send out,
 * at 8 kHz. Integer only, no allocation, no globals. The owner (the daemon)
 * watches `state`: while it is TEL_ANSWERED the owner runs the modem and
 * sends the modem's samples instead of ours.
 *
 * The random effects (busy, drops) are there on request, to make the box
 * behave like the telephone network of the dial-up years; all are off by
 * default. */
#ifndef V90D_TEL_H
#define V90D_TEL_H
#include <stdint.h>

#define TEL_FS          8000
#define TEL_NUMBER_MAX  20

enum { TEL_TONES_US = 0, TEL_TONES_EU = 1 };

typedef struct {
    char     number[TEL_NUMBER_MAX + 1]; /* digits 0-9 * #; "" = any number connects */
    int      tones;             /* TEL_TONES_* */
    int      rings;             /* bursts of ringing tone before the answer, 0..10 */
    int      busy_chance;       /* percent of calls that get busy tone */
    int      drop_mean_min;     /* mean minutes of a call before the line drops, 0 = never */
    int      session_limit_min; /* longest call in minutes, 0 = no limit */
    int      idle_timeout_min;  /* minutes without tel_activity() before we end the call, 0 = never */
    int      tone_level;        /* peak of one tone component, 16-bit units; 0 = 2000 */
    uint32_t seed;              /* random generator seed (0 is replaced) */
} tel_cfg;

typedef enum {
    TEL_ONHOOK = 0,
    TEL_DIALTONE,       /* dial tone, no digit yet */
    TEL_DIALING,        /* collecting digits */
    TEL_CONNECTING,     /* number accepted: post-dial delay, then ringing tone */
    TEL_ANSWERED,       /* the owner runs the call */
    TEL_BUSY,           /* busy tone until the caller hangs up */
    TEL_INTERCEPT,      /* wrong number: information tones, then congestion tone */
    TEL_TIMEOUT,        /* no (complete) number in time: congestion tone */
    TEL_ENDED           /* the call is over: silence, then busy tone, until on hook */
} tel_state;

typedef enum {
    TEL_END_NONE = 0,
    TEL_END_CALLER,     /* the caller went on hook */
    TEL_END_FAR,        /* the owner ended the call (tel_call_ended) */
    TEL_END_DROP,       /* random line drop */
    TEL_END_LIMIT,      /* session limit */
    TEL_END_IDLE        /* idle timeout */
} tel_end;

/* DTMF receiver: two blocks of 102 samples that agree make a digit */
#define TEL_DTMF_N 102
typedef struct {
    int32_t q1[8], q2[8];       /* Goertzel state: 4 row, 4 column frequencies */
    int64_t energy;
    int     n;
    int     last, run;          /* last block verdict and how often in a row */
    int64_t q_pw;               /* tone energy of the last block */
    int     reported;           /* the digit in progress was delivered */
} tel_dtmf;

typedef struct {
    tel_cfg   cfg;
    tel_state state;
    tel_end   end;              /* why the last call ended (kept until the next call) */
    char      dialed[TEL_NUMBER_MAX + 1];
    int       ndialed;
    int       overflow;         /* more digits than fit */
    int       want_hangup;      /* TEL_ANSWERED: the owner should end the call in an
                                   orderly way and then call tel_call_ended(); `end`
                                   says why (limit or idle) */
    uint32_t  calls, answered;  /* off-hook events, calls that reached TEL_ANSWERED */
    /* about the current / last call, kept until the next one begins */
    int       answered_this, was_busy, was_wrong;
    /* private */
    tel_cfg   next; int have_next;   /* tel_reconfigure while a call is up */
    uint32_t  rng;
    int       hook;             /* debounced: 1 = off hook */
    int32_t   hook_n;           /* samples the raw hook has differed from `hook` */
    int       pulses;           /* loop breaks counted for the digit in progress */
    int32_t   since_make;       /* samples off hook since the last break ended */
    int32_t   t;                /* samples in this state */
    int32_t   idle;             /* samples since the last digit / activity */
    int32_t   delay;            /* post-dial delay, samples */
    int       will_busy;
    int32_t   sec;              /* samples into the current second of the call */
    uint32_t  call_s, idle_s;   /* seconds answered, seconds without activity */
    uint32_t  ph[2];            /* tone oscillators */
    tel_dtmf  dtmf;
} tel_t;

void    tel_init(tel_t *t, const tel_cfg *cfg);
/* offhook: raw loop state from the SLIC (1 = loop closed). rx: the line. */
int16_t tel_tick(tel_t *t, int offhook, int16_t rx);
/* The owner's call is over (carrier lost, PPP closed, modem never trained). */
void    tel_call_ended(tel_t *t);
/* Traffic was carried: restarts the idle timeout. */
void    tel_activity(tel_t *t);
/* Settings for the next call (takes effect when the line is next on hook;
 * at once if it is on hook now). */
void    tel_reconfigure(tel_t *t, const tel_cfg *cfg);

/* DTMF receiver on its own (used by tel_tick; exposed for tests).
 * Returns a character 0-9 * # A-D once per key press, else 0. */
void    tel_dtmf_init(tel_dtmf *d);
int     tel_dtmf_rx(tel_dtmf *d, int16_t x);

#endif
