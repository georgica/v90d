/* The dial-up server proper, without its operating system: one telephone
 * line, one call at a time.
 *
 *     hook + G.711 from the SLIC  ->  telephony layer (tel/): dial tone,
 *         digits, ringing, busy, drops
 *     once answered               ->  the call (dce/): modem, V.42, PPP
 *     IP packets                  <-> the owner (a TUN device on the box)
 *     configuration (cfg/)        ->  both, taking effect at the next call
 *     status (cfg/status)         <-  both, with a history of calls
 *
 * srv_tick() once per 8 kHz sample is the only clock. Portable C99, integer
 * only, no allocation, no globals. A srv_t is large (the call object alone
 * is about 64 kB): make it static. */
#ifndef V90D_SRV_H
#define V90D_SRV_H
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "status.h"
#include "tel.h"
#include "dce.h"
#include "console.h"

typedef void (*srv_ip_fn)(void *ctx, const uint8_t *pkt, size_t len);

typedef struct {
    cfg_t      cfg;             /* in force for the current / next call */
    cfg_t      next; int have_next;
    tel_t      tel;
    dce_t      dce;
    status_t   status;
    srv_ip_fn  ip_out; void *ctx;
    ppp_user_t users[CFG_MAX_USERS];
    uint32_t   seed;
    int        echo_delay;
    int        rx_linear, max_tx_code;  /* bench settings: srv_set_rx_linear, srv_set_max_tx_code */
    int        in_call;         /* the call object is running */
    int        episode;         /* the line is off hook and not yet in the history */
    int        hangup_sent;
    tel_state  last_tel;
    int        sub;             /* samples into the current second */
    int64_t    rx_pow, rx_pow_last;    /* received power: sum of squares this second, mean of the last */
    uint32_t   clock;           /* UTC seconds, 0 = not set */
    uint32_t   call_start;      /* clock when the line was answered */
    uint32_t   call_s;          /* seconds since the answer */
    dce_status_t ds;            /* scratch */
    console_t  console;         /* service = dreamcast: the packets on their way out */
    uint8_t    pkt[1500];
} srv_t;

/* ip_out receives the IP packets that arrive from the caller. */
void    srv_init(srv_t *s, const cfg_t *cfg, uint32_t seed, srv_ip_fn ip_out, void *ctx);
/* One sample: the loop state (1 = off hook) and the G.711 codeword from the
 * line in, the codeword for the line out (law as in cfg.law). */
uint8_t srv_tick(srv_t *s, int offhook, uint8_t rx);
/* The same for a line whose codec receives in 16-bit linear (line.h): rx is
 * the sample on the 16-bit scale of the G.711 tables (mu-law +/-32124, A-law
 * +/-32256 at full scale), the result is still the codeword to send. Tell
 * the server with srv_set_rx_linear() before the call begins. */
uint8_t srv_tick_lin(srv_t *s, int offhook, int16_t rx);
/* An IP packet for the caller. 0 = queued; PPP_ERR_DOWN (nobody there),
 * PPP_ERR_SIZE, PPP_ERR_FULL (try again later). */
int     srv_send_ip(srv_t *s, const uint8_t *pkt, size_t len);
/* New settings: used from the next call on (at once if the line is idle). */
void    srv_reconfigure(srv_t *s, const cfg_t *cfg);
/* Wall clock for the call history; it then runs on the sample clock. */
void    srv_set_clock(srv_t *s, uint32_t utc_seconds);
/* What the line driver knows about itself: samples from handing over a
 * codeword until its echo starts to come back. The default is -1: not
 * known, the V.90 modem measures it in every call (up to 160 ms). */
void    srv_set_echo_delay(srv_t *s, int samples);
/* Bench settings, not in the configuration; both apply from the next call.
 * rx_linear: the line hands over linear samples (srv_tick_lin), so the V.92
 * upstream design need not allow for a G.711 quantiser. max_tx_code: the
 * maximum downstream power INFO0d announces, -0.5 dBm0 less 0.5 dB per step,
 * 1..31; 0 = the default 23 (-12 dBm0). */
void    srv_set_rx_linear(srv_t *s, int on);
void    srv_set_max_tx_code(srv_t *s, int code);
/* 1 while a caller has IP up (packets can be sent to it) */
int     srv_ip_up(const srv_t *s);

/* transmit level: dBm0 to the RMS the modem wants (16-bit sample units) */
int     srv_level_rms(int dbm0);

#endif
