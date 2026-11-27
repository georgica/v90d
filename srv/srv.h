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
#include "saturn.h"
#include "bbs.h"

typedef void (*srv_ip_fn)(void *ctx, const uint8_t *pkt, size_t len);
typedef void (*srv_raw_fn)(void *ctx, const uint8_t *oct, size_t n);
/* a PPP caller is hung up when its network is not up this long after the
 * answer (the modem's training, LCP, the login and IPCP all fit well) */
#define SRV_LOGIN_MAX_S 90
/* srv_link(): a call to the bulletin board number (bbs.h), beside the SATURN_* kinds */
#define SRV_LINK_BBS 16
/* srv_link(): a call to a number of a service ([services], srv/svc.h) */
#define SRV_LINK_SVC 17
/* srv_link(): a call on the Internet number with [services] menu on, still
 * in its terminal menu (term/tmenu.h, srv/termsvc.c); once it goes over to
 * PPP (srv_start_ppp, or Dial-Up Networking found by its first frame) it
 * is an ordinary call (SATURN_NONE) */
#define SRV_LINK_MENU 18
/* PROTOWEB on the keypad with zeros behind (the rule of the board numbers):
 * the old web of Protoweb, as Netstalgia also has it */
#define SRV_PROTOWEB "7768693200"
/* a WebTV / MSN TV box's own numbers (a WebTV port, srv_webtv): 1-800-WEBTV-00;
 * the toll-free number a box with no TellyScript dials (V.34 at most: such
 * a box hangs up above 33.6K); and the TellyScript's number. All three as
 * Netstalgia has them, the WebTV service going to the WebTV server */
#define SRV_WEBTV    "8009328800"
#define SRV_WEBTV_TF "8006138199"
#define SRV_WEBTV_56 "4848201337"
/* srv_link(): a call to the time service (srv/acts.h, srv/actsvc.c) */
#define SRV_LINK_ACTS 19
/* the time service's number: NIST's ACTS number, which period time-setting
 * programs and the NTP ACTS driver come set to. Fixed; on every port, with
 * or without a 1 in front, while the box's clock is set (srv_set_clock). */
#define SRV_ACTS "3034944774"
/* dialed against Netstalgia's list of services (the daemon's): 2 = it is
 * one of its numbers, 1 = it begins one, 0 = neither (cfg_svc_number_begins) */
typedef int (*srv_svc_fn)(const char *dialed);

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
    int        originate;               /* bench setting: srv_set_originate */
    void     (*ppp_trace)(void *ctx, int out, uint32_t ms, uint16_t proto,
                          const uint8_t *pkt, size_t pn, size_t len);   /* srv_set_ppp_trace */
    int       *heavy_slot;      /* shared across ports: calls now offering a heavy modulation, x2 or K56flex (NULL = no guard) */
    int        heavy_held;      /* 1 = this call reserved the heavy slot */
    int        in_call;         /* the call object is running */
    int        episode;         /* the line is off hook and not yet in the history */
    int        hangup_sent;
    tel_state  last_tel;
    int        sub;             /* samples into the current second */
    int64_t    rx_pow, rx_pow_last;    /* received power: sum of squares this second, mean of the last */
    uint32_t   clock;           /* UTC seconds, 0 = not set */
    uint32_t   call_start;      /* clock when the line was answered */
    uint32_t   call_s;          /* seconds since the answer */
    int        ppp_up_once;     /* this call's PPP has had its network up (the login deadline is over) */
    uint32_t   ppp_from_s;      /* call_s when its PPP began (after the terminal menu: later than 0) */
    dce_status_t ds;            /* scratch */
    uint32_t   neg_offered;     /* what the call's modem offered (st_neg_t); 0 = no modem ran */
    srv_raw_fn raw_out;         /* service = saturn: octets from the caller's modem */
    int        link; uint32_t link_addr;   /* the call in progress there: SATURN_*, whom to call */
    char       link_game[3];                /* matchmaking: the two-character game id */
    console_t  console;         /* service = dreamcast: the packets on their way out */
    srv_svc_fn svc_known;       /* Netstalgia's numbers of services, NULL = none */
    int        protoweb_ok;     /* the box has the archive gateway: SRV_PROTOWEB answers */
    uint8_t    entropy[32]; size_t entropy_n;   /* srv_set_entropy: for the next call's PPP */
    int        protoweb;        /* the call in progress came in on SRV_PROTOWEB */
    int        webtv;           /* the call in progress is a WebTV box's (srv_webtv) */
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
/* Bench: the box calls. A modem on the port that goes off hook (it was told
 * ATA) is called at once by the library's calling modem (V.34, V.32bis,
 * V.22bis, V.22, V.21 so far), with V.42 as the originator. Takes effect with the next
 * call; the port takes no calls in this mode. */
void    srv_set_originate(srv_t *s, int on);
/* Share a heavy-call arbiter across the ports: *slot counts the calls that
 * currently offer a heavy modulation (x2 or K56flex). A line offers x2 or
 * K56flex only while the count is 0, so at most one heavy call runs at a time
 * (each is about two thirds of the CPU in data mode, so two do not fit). NULL
 * (the default) means no guard: a single-port build or a test offers them
 * whenever the configuration allows. Set once, before the first call. */
void    srv_set_heavy_slot(srv_t *s, int *slot);
/* service = saturn (config.h, saturn.h). raw_out receives, with the ctx of
 * srv_init, what the caller's modem sends. srv_link() tells about the call
 * in progress: SATURN_WAIT, SATURN_CALL with the address dialled in *addr,
 * or SATURN_NONE when there is none (SATURN_MATCH_WAIT / SATURN_MATCH_CALL use srv_link_game); *online is 1 once the modem carries
 * octets, and from then on srv_send_raw() takes octets for it (returns how
 * many it took). srv_hangup() ends the call from our side: the other
 * player never came, or has gone. */
void    srv_set_raw(srv_t *s, srv_raw_fn raw_out);
int     srv_link(const srv_t *s, uint32_t *addr, int *online);
/* matchmaking kinds: the two-character game id (NUL-terminated), or "". */
const char *srv_link_game(const srv_t *s);
size_t  srv_send_raw(srv_t *s, const uint8_t *oct, size_t n);
/* A call in its terminal menu (SRV_LINK_MENU) goes over to PPP on the same
 * connection (dce_start_ppp): from now on it is a PPP call, and its login
 * deadline counts from here. -1 when it is not in the menu. */
int     srv_start_ppp(srv_t *s);
/* A call in its terminal menu: whether a PPP frame from the caller still
 * takes it to PPP (dce_ppp_watch). Off while the caller is on a board. */
void    srv_ppp_watch(srv_t *s, int on);
void    srv_hangup(srv_t *s);
/* The bulletin board number (bbs.h) answers on a port of any service. That
 * call carries octets like a saturn call (srv_set_raw, srv_send_raw,
 * srv_hangup), with error control as [modem] says, and srv_link() reports
 * SRV_LINK_BBS for it; srv_dialed() is what was dialled in the call in
 * progress (which of the numbers). */
const char *srv_dialed(const srv_t *s);
/* The numbers of services ([services] in config.h) answer like the bulletin
 * board numbers, on a port of any service, unless the number is the port's
 * own: the call carries octets (srv_set_raw, srv_send_raw, srv_hangup) and
 * srv_link() reports SRV_LINK_SVC. The owner's numbers are in the
 * configuration; Netstalgia's come from the daemon through known (NULL:
 * none), asked only while [services] netstalgia = on. srv_svc_number()
 * says whose a number is: SRV_SVC_OWN, SRV_SVC_LIST, or 0 for none. */
enum { SRV_SVC_OWN = 1, SRV_SVC_LIST = 2 };
void    srv_set_svc_lookup(srv_t *s, srv_svc_fn known);
int     srv_svc_number(const srv_t *s, const char *dialed);
/* SRV_PROTOWEB, with or without a 1 in front, on a port whose service is
 * internet or archive, unless it is the port's own number: the dial-up
 * connection as on the port's own number (its PPP login), with the web,
 * FTP and names of the archive (the daemon's archive mode, for that call
 * alone). Only when the daemon says it has the archive gateway (on = 1).
 * srv_protoweb() is 1 while such a call is in progress. */
void    srv_set_protoweb(srv_t *s, int on);
/* A WebTV port ([line] service = webtv): every call, whatever number it
 * dialled (the SRV_WEBTV* numbers, with or without a 1, end the dialling at
 * once; any other after the pause), is the box's: the dial-up connection
 * with PAP asked for and any name and password taken (its own built-in
 * login), no encryption and no terminal menu, whatever [ppp] and
 * [services] menu say; on SRV_WEBTV_TF no V.90, V.92, x2 or K56flex. 1
 * while such a call is in progress (the daemon then sends its WebTV
 * service to the WebTV server, [services] webtv_server). */
int     srv_webtv(const srv_t *s);
/* the K56flex line of a call's log, from libv90modem's v90m_k56: what our
 * V.8bis CL offered and what the client chose (a declined client is one
 * that would have run K56flex version 1). "" and 0 when no CL went out (or
 * k is NULL); otherwise the length, the text cut to fit n. */
size_t  srv_k56_text(const v90m_k56 *k, char *buf, size_t n);
/* The V.92 Quick Connect line of a call's log, from libv90modem's v90m_qc:
 * whether the caller asked (its Ucode, LAPM), whether we took it, the
 * ANSpcm level, its TONEq and short Phase 2, or the fall back to V.8.
 * "" and 0 when it did not ask (or q is NULL); otherwise the length, cut
 * to fit n. */
size_t  srv_qc_text(const v90m_qc *q, char *buf, size_t n);
/* The Van Jacobson header compression line of a call's log: each way, the
 * TCP packets sent compressed of all sent with a slot, and the slots; the
 * packets dropped. "" and 0 when it was agreed neither way. */
size_t  srv_vj_text(const ppp_counters_t *k, char *buf, size_t n);
/* For the log of a call: a caller that asked for MPPE encryption on CCP
 * option 18 (its last request) while none was agreed -- Windows ends such a
 * call with error 742. "" and 0 when it asked for none, or MPPE is in use. */
size_t  srv_mppe_note(const ppp_counters_t *k, char *buf, size_t n);
/* One line of a call's PPP trace (ppp_config_t.trace): the time since PPP
 * started, the direction, the protocol and code by name, the identifier,
 * the length and the octets after the header that were given (cut, with
 * "...", where more were sent). Cut to fit n. */
void    srv_ppp_trace_text(int out, uint32_t ms, uint16_t proto, const uint8_t *pkt,
                           size_t pn, size_t len, char *buf, size_t n);
/* where each call's PPP trace goes (NULL: nowhere); ctx is the srv_t */
typedef void (*srv_ppp_trace_fn)(void *ctx, int out, uint32_t ms, uint16_t proto,
                                 const uint8_t *pkt, size_t pn, size_t len);
void    srv_set_ppp_trace(srv_t *s, srv_ppp_trace_fn fn);
/* Unpredictable octets (the daemon reads them from /dev/urandom) for the
 * next call's PPP: its magic numbers and challenges. Give fresh ones after
 * each call; the server keeps at most 32. */
void    srv_set_entropy(srv_t *s, const uint8_t *b, size_t n);
int     srv_protoweb(const srv_t *s);
/* 1 while a caller has IP up (packets can be sent to it) */
int     srv_ip_up(const srv_t *s);

/* transmit level: dBm0 to the RMS the modem wants (16-bit sample units) */
int     srv_level_rms(int dbm0);

#endif
