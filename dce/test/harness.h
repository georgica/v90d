/* Test side only: one simulated call. The answering DCE (dce.h) and the
 * calling peer (caller.h) joined through the channel simulator and clocked
 * sample by sample, plus a traffic generator that verifies every IP packet
 * octet for octet on arrival. */
#ifndef V90D_DCE_TEST_HARNESS_H
#define V90D_DCE_TEST_HARNESS_H
#include "dce.h"
#include "caller.h"

#define FS       8000
#define HARNESS_OUR_IP   PPP_IP4(10, 64, 0, 1)
#define HARNESS_PEER_IP  PPP_IP4(10, 64, 0, 7)
#define HARNESS_DNS1     PPP_IP4(10, 64, 0, 53)

typedef struct {
    double noise_dbfs;         /* steady AWGN, both directions */
    int    burst_every_ms;     /* 0 = no bursts */
    int    burst_ms;
    int    burst_amp;          /* peak of the uniform noise added in a burst */
    double echo_gain;          /* hybrid echo of our own transmitter */
} line_t;

extern const line_t line_clean;      /* -55 dBFS noise, hybrid echo 0.2 */
extern const line_t line_no_echo;

enum { DN = 0, UP = 1 };       /* server -> caller, caller -> server */
enum { TEXT = 0, RANDOM = 1,
       HTML = 2 /* a download: full-size packets, a TCP/IP header that moves
                   and markup-like text behind it (ppp/test/gen.h) */ };

typedef struct {
    int  kind, base;           /* payload kind, index of the first packet */
    int  want, n_tx, n_rx, n_bad;
    int  n_lost;               /* missing from the sequence (only with traffic_lossy) */
    long bytes_rx;
    long t_last;               /* sample time of the last arrival */
} flow_t;

extern dce_t    D;             /* the answering side under test */
extern caller_t C;             /* the calling peer */
extern long     now;           /* samples since the call began */
extern int      harness_linear; /* 1 = drive the DCE through the linear dce_tick() */
extern int      harness_alaw;   /* 1 = the DCE's codec is A-law (V.90 calls) */
extern int      bursts_on;     /* noise bursts enabled (set once the call is up) */
extern long     n_bursts;
extern flow_t   F[2];          /* per direction, reset by each traffic() */
extern long     n_busy[2];     /* times a sender was told "full, try later" */

/* server: CHAP or PAP, users alice/wonderland and bob/hunter2, fixed
 * addresses; client: caller_cfg_default(). Both deliver IP to the checker. */
void server_cfg(dce_cfg_t *c);
void client_cfg(caller_cfg_t *c);
void call_begin(const dce_cfg_t *sc, const caller_cfg_t *cc, const line_t *line);

void step(void);                               /* one 8 kHz sample for everybody */
void run_ms(int ms);
/* run until pred() holds; returns the milliseconds it took, -1 on timeout */
int  run_until(int (*pred)(void), int limit_ms);
int  p_ip_up(void);                            /* IP up at both ends */
int  p_ended(void);                            /* the DCE reports DCE_ENDED */
dce_status_t *status(void);

/* Send n_dn packets down and n_up packets up as fast as the two ends take
 * them, until all have arrived or the limit passes. Fills rate[] with the
 * payload throughput of each direction in octets per second. Returns 1 when
 * everything arrived (see F[] for counts and corruption). With traffic_lossy
 * set, missing packets are counted in F[].n_lost instead of making every
 * later one "bad", and 1 is returned once all are accounted for. */
extern int traffic_lossy;       /* 1: up to 9 packets in a row may be missing; more than 1: that many */
extern int traffic_idle_ms;     /* lossy: nothing sent or arrived for this long ends the run [3000] */
int  traffic(int kind, int n_dn, int n_up, int limit_ms, long rate[2]);

#endif
