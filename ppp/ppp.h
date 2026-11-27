/* PPP server (the answering / NAS side of a dial-up link).
 *
 * Implements, from the RFCs: HDLC-like async framing (RFC 1662), LCP
 * (RFC 1661), PAP (RFC 1334) and CHAP-MD5 (RFC 1994) as authenticator,
 * IPCP (RFC 1332) with the DNS options of RFC 1877. We assign the peer its
 * IP address. Van Jacobson TCP/IP header compression (RFC 1144) each way.
 * Packet compression: CCP (RFC 1962) with PPP Deflate (RFC 1979) and MPPC
 * (RFC 2118, without encryption), each direction negotiated on its own;
 * see README.md.
 *
 * Portable C99, no floating point, no allocation, no OS calls.
 * All state lives in a caller-provided ppp_t. The library is not
 * reentrant: call it from one thread (or under one lock).
 *
 * Typical use:
 *
 *     static ppp_t ppp;
 *     ppp_config_t cfg;
 *     ppp_config_default(&cfg);
 *     cfg.auth = PPP_AUTH_CHAP | PPP_AUTH_PAP;
 *     cfg.users = table; cfg.n_users = 2;
 *     cfg.our_ip = PPP_IP4(10,0,0,1); cfg.peer_ip = PPP_IP4(10,0,0,2);
 *     cfg.ip_rx = my_ip_input;
 *     ppp_init(&ppp, &cfg);
 *
 *     on carrier:        ppp_lower_up(&ppp);
 *     modem rx data:     ppp_input_bytes(&ppp, buf, n);
 *     modem tx ready:    n = ppp_output_bytes(&ppp, buf, room);
 *     every few ms:      ppp_tick(&ppp, elapsed_ms);
 *     IP packet to peer: ppp_send_ip(&ppp, pkt, len);
 *     phase goes DEAD:   hang up, then ppp_lower_down(&ppp);
 *     carrier lost:      ppp_lower_down(&ppp);
 *
 * Callbacks (ip_rx, phase_cb, secret_cb, pap_cb) run inside
 * ppp_input_bytes / ppp_tick / ppp_lower_* / ppp_close. From a callback
 * only ppp_send_ip, ppp_output_bytes and ppp_get_status may be called. */
#ifndef V90D_PPP_H
#define V90D_PPP_H
#include <stddef.h>
#include <stdint.h>
#include "ppp_fsm.h"
#include "ppp_hdlc.h"
#include "ppp_deflate.h"
#include "ppp_mppc.h"
#include "ppp_crypto.h"
#include "ppp_mppe.h"
#include "ppp_vj.h"

/* Transmit buffer size in octets (stuffed wire form). One worst-case IP
 * frame is 2 * (1500 + 6) + 2 = 3014 octets. */
#ifndef PPP_TX_BUF
#define PPP_TX_BUF 8192
#endif
/* Part of the transmit buffer that ppp_send_ip leaves free so control
 * packets (echo replies, terminate) always get through. */
#ifndef PPP_TX_RESERVE
#define PPP_TX_RESERVE 512
#endif
#define PPP_NAME_MAX    63   /* longest peer name / user name kept */
#define PPP_SECRET_MAX  64   /* longest CHAP secret */
#define PPP_MIN_MRU     68   /* smallest peer MRU we accept */

/* IPv4 address as a host-order 32-bit value: 10.0.0.1 is 0x0a000001 */
#define PPP_IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

/* protocol numbers */
#define PPP_PROTO_IP    0x0021
#define PPP_PROTO_COMP  0x00fd   /* a compressed datagram (RFC 1962 2.1) */
#define PPP_PROTO_CCP   0x80fd
#define PPP_PROTO_IPCP  0x8021
#define PPP_PROTO_LCP   0xc021
#define PPP_PROTO_PAP   0xc023
#define PPP_PROTO_CHAP  0xc223

/* link phases, RFC 1661 section 3.2 */
typedef enum {
    PPP_PHASE_DEAD = 0,
    PPP_PHASE_ESTABLISH,
    PPP_PHASE_AUTHENTICATE,
    PPP_PHASE_NETWORK,
    PPP_PHASE_TERMINATE
} ppp_phase_t;

/* authentication methods; ppp_config_t.auth is a mask of the allowed ones.
 * 0 means no authentication. With both set CHAP is requested first and PAP
 * is used if the peer Naks CHAP. */
#define PPP_AUTH_NONE  0u
#define PPP_AUTH_PAP   1u
#define PPP_AUTH_CHAP  2u          /* CHAP-MD5 (RFC 1994) */
#define PPP_AUTH_MSCHAPV2 4u       /* MS-CHAPv2 (RFC 2759), for MPPE keys */
#define PPP_AUTH_MSCHAPV1 8u       /* MS-CHAP v1 (RFC 2433), for MPPE keys; offered
                                      first when allowed (Windows 98 needs it) */

/* why the link ended (or is ending) */
typedef enum {
    PPP_END_NONE = 0,
    PPP_END_LOCAL_CLOSE,       /* ppp_close() */
    PPP_END_PEER_TERMINATED,   /* peer sent Terminate-Request */
    PPP_END_LOWER_DOWN,        /* ppp_lower_down() while the link was alive */
    PPP_END_NEGOTIATION,       /* LCP did not converge / no answer */
    PPP_END_AUTH_REFUSED,      /* peer will not do any method we allow */
    PPP_END_AUTH_FAILED,       /* bad credentials */
    PPP_END_AUTH_TIMEOUT,      /* peer never authenticated */
    PPP_END_KEEPALIVE,         /* echo requests went unanswered */
    PPP_END_LOOPBACK,          /* we are hearing ourselves */
    PPP_END_IPCP_FAILED,       /* IPCP did not converge or was closed */
    PPP_END_ENCRYPT_REQUIRED,  /* encryption is required but MPPE was not agreed */
    PPP_END_MPPE_LOST          /* MPPE fell out of step past recovery (a long
                                  outage: ppp_mppe_rx_late) */
} ppp_end_t;

typedef struct {
    const char *name;
    const char *secret;        /* plaintext; needed for CHAP */
} ppp_user_t;

/* packet compression algorithms; ppp_config_t.comp is a mask of the ones
 * allowed. Where the peer offers both, Deflate is used. */
#define PPP_COMP_NONE     0u
#define PPP_COMP_DEFLATE  1u   /* RFC 1979 */
#define PPP_COMP_MPPC     2u   /* RFC 2118 */
#define PPP_COMP_MPPE     4u   /* RFC 3078 encryption; not a cfg.comp bit, a
                                  marker for the data path and status */
/* Octets per millisecond the compressor is given at the most when
 * ppp_config_t.comp_rate is 0. It bounds the processor time compression
 * can take: ppp_send_ip answers PPP_ERR_FULL while the allowance is used
 * up, and never compresses more than one full-size packet between two
 * calls of ppp_tick. */
#ifndef PPP_COMP_RATE
#define PPP_COMP_RATE     40
#endif

typedef struct {
    /* --- authentication --- */
    unsigned auth;             /* PPP_AUTH_* mask, default none */
    const char *hostname;      /* our name in CHAP challenges, default "v90d" */
    /* Static user table, searched when the callbacks below are NULL.
     * The array and its strings must outlive the session. */
    const ppp_user_t *users;
    size_t n_users;
    /* Optional: look up a user's plaintext secret. Copy at most max octets
     * to secret and return the length, or -1 if the user is unknown. */
    int (*secret_cb)(void *ctx, const uint8_t *user, size_t ulen,
                     uint8_t *secret, size_t max);
    /* Optional: check a PAP name/password pair yourself (for example
     * against hashed storage). Return nonzero to accept. When NULL, PAP is
     * checked against secret_cb / the user table. */
    int (*pap_cb)(void *ctx, const uint8_t *user, size_t ulen,
                  const uint8_t *pass, size_t plen);

    /* --- addresses (see PPP_IP4) --- */
    uint32_t our_ip;           /* announced in our IPCP request */
    uint32_t peer_ip;          /* assigned to the peer; 0 accepts what it asks */
    uint32_t dns1, dns2;       /* RFC 1877 answers; 0 rejects the option */

    /* --- link --- */
    uint16_t mru;              /* our MRU, 0 or 1500 = default (not sent) */
    uint32_t accm;             /* control characters the peer must escape
                                  towards us; default 0 (none) */
    uint8_t  no_pfc;           /* nonzero: no Protocol-Field-Compression */
    uint8_t  no_acfc;          /* nonzero: no Address/Control compression */
    uint8_t  no_magic;         /* nonzero: do not request a Magic-Number */
    uint32_t restart_ms;       /* Restart timer, 0 = 3000 */
    uint8_t  max_configure;    /* 0 = 10 */
    uint8_t  max_terminate;    /* 0 = 2 */
    uint8_t  max_failure;      /* 0 = 5 */
    /* --- packet compression (CCP) --- */
    unsigned comp;             /* PPP_COMP_* mask; 0 = CCP is not spoken
                                  (Protocol-Reject). Default: both */
    uint8_t  encrypt;          /* 0 (default): MPPE on request -- a caller
                                  that asks for it in CCP, after an MS-CHAPv2
                                  login, gets it, and then nothing goes in
                                  the clear; any other caller gets the
                                  compression it asks for. Nonzero: MPPE
                                  required (RFC 3078) in CCP, keyed from
                                  MS-CHAPv2 (needs cfg.auth &
                                  PPP_AUTH_MSCHAPV2). When
                                  on, option 18 carries the MPPE bits in
                                  place of MPPC: 128-bit keys, or the
                                  strongest of 56 and 40 a peer without them
                                  has (Windows 9x/ME); stateless asked for
                                  first, stateful taken when the peer wants it
                                  (a Windows client on a modem). Default off. */
    uint8_t  comp_no_rx;       /* nonzero: compress what we send only; ask
                                  the peer for nothing */
    uint8_t  deflate_wbits;    /* Deflate window we ask the peer to use
                                  towards us, 8..15 as in RFC 1979;
                                  0 = PPP_DEFLATE_MAX_WBITS */
    uint16_t comp_rate;        /* octets per ms for the compressor,
                                  0 = PPP_COMP_RATE */
    uint32_t comp_reset_ms;    /* wait for the answer to a CCP Reset-Request
                                  before it is repeated, 0 = restart_ms.
                                  The answer queues behind a frame in
                                  transmission at either end: on a slow
                                  line that is longer than a Restart
                                  period, and a repeat that comes after
                                  the peer has reset makes it reset again */
    uint32_t echo_interval_ms; /* LCP echo keepalive period, 0 = off */
    uint8_t  echo_max_fail;    /* unanswered echoes before the peer is
                                  declared dead, 0 = 3 */
    /* Seed for magic numbers and CHAP challenges. The library has no
     * entropy source of its own: give it something unpredictable (for
     * example from /dev/urandom) and/or call ppp_add_entropy(). */
    uint32_t seed;
    /* and more of it, mixed in with the seed at ppp_init (NULL = none):
     * the 32 bits of seed alone can be searched through */
    const uint8_t *entropy; size_t entropy_n;

    /* --- callbacks --- */
    void *ctx;
    /* an IP packet arrived from the peer */
    void (*ip_rx)(void *ctx, const uint8_t *pkt, size_t len);
    /* the phase changed; on PPP_PHASE_DEAD the caller should hang up and
     * then call ppp_lower_down() (not from inside this callback) */
    void (*phase_cb)(void *ctx, ppp_phase_t phase);
    /* Optional, for a per-call log: every control packet (LCP, PAP, CHAP,
     * IPCP, CCP, any protocol from 0x8000 up) received (out 0) or queued to
     * send (out 1), never IP. ms is the time since ppp_init (ppp_tick's
     * sum); pkt the packet from its Code octet, n octets of it (at most
     * PPP_TRACE_MAX; the 4-octet header alone for PAP and CHAP, so no
     * password, challenge answer or name is given); len its whole length.
     * Received packets are traced as they arrive, before they are judged
     * (also those discarded in the phase they came in). */
    void (*trace)(void *ctx, int out, uint32_t ms, uint16_t proto,
                  const uint8_t *pkt, size_t n, size_t len);
    /* nonzero: no Van Jacobson TCP/IP header compression (IPCP option 2,
     * RFC 1144), neither asked for nor taken. Default (0): asked for (16
     * slots) and given to a peer that asks for it, each way on its own */
    uint8_t  no_vj;
} ppp_config_t;
#define PPP_TRACE_MAX 64

typedef struct {
    uint32_t rx_frames;        /* good frames */
    uint32_t rx_bad_fcs;
    uint32_t rx_runts;
    uint32_t rx_overlong;
    uint32_t rx_aborts;
    uint32_t rx_bad_header;    /* malformed or non-negotiated compression */
    uint32_t rx_discards;      /* valid frames not acceptable in this phase */
    uint32_t rx_unknown_proto; /* answered with Protocol-Reject */
    uint32_t rx_ip;
    uint32_t tx_frames;
    uint32_t tx_ip;
    uint32_t tx_dropped;       /* control frames lost to a full tx buffer */
    /* packet compression. The algorithms are the last ones agreed in this
     * session (PPP_COMP_*): they stay when CCP or the link goes down, so
     * that the figures can be read after the call. */
    uint8_t  comp_tx, comp_rx;          /* towards / from the peer */
    uint8_t  comp_tx_wbits, comp_rx_wbits;   /* Deflate window, 0 for MPPC */
    uint32_t comp_tx_in;       /* octets of the IP packets given to the compressor */
    uint32_t comp_tx_out;      /* octets sent for them (information fields) */
    uint32_t comp_tx_native;   /* of those packets, sent uncompressed: no gain */
    uint32_t comp_tx_resets;   /* Reset-Requests from the peer */
    uint32_t comp_tx_purged;   /* compressed packets taken back out of the
                                  transmit buffer when one came */
    uint32_t comp_rx_in;       /* octets received while a decompressor was on */
    uint32_t comp_rx_out;      /* octets of the packets they became */
    uint32_t comp_rx_errors;   /* out of step: Reset-Request sent */
    uint32_t comp_rx_dropped;  /* compressed packets discarded for it */
    uint32_t ccp_opens;        /* times CCP reached Opened */
    /* MPPE negotiation, for the log of a call that did not encrypt: the
     * peer's last option 18 request, and its last Nak of ours (each valid
     * when its PPP_MPPE_SEEN_* bit is set in mppe_seen) */
    uint32_t mppe_peer_req, mppe_peer_nak;
    uint8_t  mppe_seen;
    uint8_t  mppe_stateful;    /* MPPE agreed stateful: 1 towards, 2 from the peer */
    uint8_t  mppe_keybits;     /* MPPE key length agreed: 40, 56 or 128 (0: none) */
    uint8_t  win98;            /* the caller's first LCP request was Windows 98's
                                  (MS-CHAP v1 offered first) */
    /* Van Jacobson header compression: slots agreed towards / from the peer
     * (0: not in use that way), and the TCP packets each way */
    uint8_t  vj_tx_slots, vj_rx_slots;
    uint32_t vj_tx_comp, vj_tx_uncomp;  /* sent compressed / uncompressed (slot set) */
    uint32_t vj_rx_comp, vj_rx_uncomp;  /* received and rebuilt */
    uint32_t vj_errors;                 /* received and dropped: tossed after a lost
                                           frame, or malformed */
} ppp_counters_t;
#define PPP_MPPE_SEEN_REQ 1u
#define PPP_MPPE_SEEN_NAK 2u

typedef struct {
    ppp_phase_t phase;
    ppp_end_t   end_reason;
    uint8_t     lcp_state;     /* PPP_FSM_* */
    uint8_t     ipcp_state;
    uint8_t     ccp_state;
    uint8_t     comp_tx, comp_rx;   /* PPP_COMP_* in force right now */
    unsigned    auth;          /* method negotiated: PPP_AUTH_* (one bit) */
    int         authenticated;
    char        peer_name[PPP_NAME_MAX + 1];
    int         ip_up;         /* IPCP is Opened: IP may flow */
    uint32_t    our_ip, peer_ip, dns1, dns2;
    uint16_t    our_mru, peer_mru;
    uint32_t    tx_accm, rx_accm;
    uint32_t    our_magic, peer_magic;
    ppp_counters_t counters;
} ppp_status_t;

/* The session object. Treat the contents as private; it is defined here
 * only so the caller can allocate it statically. */
typedef struct ppp {
    ppp_config_t cfg;
    ppp_phase_t  phase;
    ppp_end_t    end_reason;
    ppp_end_t    pending_close;
    ppp_fsm_t    lcp, ipcp, ccp;
    uint8_t      scratch[256];

    /* LCP: what we request */
    uint8_t  w_mru, w_accm, w_magic, w_pfc, w_acfc;
    uint8_t  w_auth;           /* PPP_AUTH_* being requested */
    uint8_t  auth_tried;
    uint16_t mru;
    uint32_t accm, magic;
    /* LCP: what the peer's last acceptable request asked for */
    uint16_t pend_mru;
    uint32_t pend_accm, pend_magic;
    uint8_t  pend_pfc, pend_acfc;
    uint8_t  loop_count;
    /* LCP: in force while Opened */
    uint16_t peer_mru;
    uint32_t tx_accm, peer_magic;
    uint8_t  tx_pfc, tx_acfc, rx_pfc, rx_acfc;
    /* keepalive */
    uint32_t echo_timer;
    uint8_t  echo_pending;

    /* authentication */
    uint8_t  auth_proto;       /* PPP_AUTH_* in force */
    uint8_t  auth_done, auth_ok;
    uint8_t  auth_id, auth_tries;
    uint32_t auth_timer;
    uint8_t  challenge[16];
    char     peer_name[PPP_NAME_MAX + 1];
    /* MS-CHAPv2: the authenticator response to send on success, and the MPPE
     * master start keys derived from it (RFC 3079), for CCP to key MPPE */
    uint8_t  mschap2_resp[42];
    uint8_t  mppe_send[16], mppe_recv[16];
    uint8_t  mppe_short_send[8], mppe_short_recv[8];   /* the 40/56-bit start keys */
    uint8_t  mppe_have;

    /* IPCP */
    uint8_t  w_addr, ipcp_closing, addr_naks;
    uint8_t  w_vj;             /* VJ asked for in our next IPCP request */
    uint8_t  pend_vj_slots, pend_vj_cid;   /* the peer's VJ option we Acked (0 slots: none) */
    ppp_vj_t vj;               /* the header compression state, both ways */
    uint8_t  vjbuf[PPP_MAX_MRU];
    uint32_t vj_lost_seen;     /* frames lost on the way in, as last told to VJ */
    uint32_t our_ip, peer_ip, pend_peer_ip;

    /* CCP: what we ask for (our receive direction), one algorithm at a
     * time; what the peer's last acceptable request asked for (our
     * transmit direction); what is in force while Opened */
    uint8_t  cw_opt;           /* option type of our request, 0 = none */
    uint8_t  cw_wbits, cw_tried, cw_naks;
    uint8_t  cpend_opt, cpend_wbits, cchosen;
    uint8_t  tx_alg, rx_alg;   /* PPP_COMP_* */
    uint8_t  cw_mppe, cpend_mppe;  /* our request / the peer's offers MPPE on option 18 */
    uint8_t  cw_want_h;        /* ask for stateless MPPE (H bit) in our next request */
    uint8_t  cw_mppe_h, cpend_mppe_h;  /* the H bit of our last request / of the
                                  peer's request we accepted */
    uint32_t mppe_key;         /* the MPPE key length bit (S, M or L) we ask for
                                  and accept: 128-bit until the peer shows it
                                  has only shorter ones, then its strongest */
    uint32_t mppe_floor;       /* the strongest key bit of the peer's own
                                  request (0: none yet): no Nak goes below it */
    uint32_t cw_mppe_key, cpend_mppe_key;  /* that bit in our last request / in
                                  the peer's request we accepted */
    uint8_t  mppe_asked;       /* on request: the caller asked for MPPE (with
                                  our keys in hand): it gets MPPE, nothing in
                                  the clear */
    uint8_t  ccp_switch;       /* ... while our request was for compression:
                                  make the next one MPPE */
    uint8_t  mppe_redos;       /* CCP renegotiated for want of MPPE both ways */
    uint32_t ccp_wait_ms;      /* on request: CCP not opened yet, waiting this
                                  much longer for the caller's request */
    uint32_t lcp_wait_ms;      /* both MS-CHAP versions allowed: our first LCP
                                  request waits this much longer for the
                                  caller's, which says whether it is Windows 98 */
    uint8_t  ccp_redo;         /* the two directions came up at different key
                                  lengths: negotiate CCP again (not keyed yet) */
    uint8_t  mppe_started;     /* MPPE was keyed once this authentication; a CCP
                                  re-open must not re-key it (RC4 keystream reuse) */
    ppp_mppe_t mppe;           /* the encryption session, keyed at ccp_up */
    uint32_t enc_wait_ms;      /* with encryption required: time in the network
                                  phase without MPPE up; the link ends when it
                                  passes CCP's whole retry span */
    uint16_t tx_seq, rx_seq;   /* Deflate sequence number / MPPC coherency count */
    uint8_t  rx_wait;          /* out of step: waiting for the peer to start afresh */
    uint8_t  rr_id, rr_tries;
    uint32_t rr_timer;
    int32_t  comp_credit;
    size_t   comp_queue;       /* ppp_set_comp_queue() */

    /* nonce generator */
    uint8_t  pool[16];
    uint32_t pool_ctr;
    uint32_t uptime_ms, rx_octets;

    ppp_counters_t cnt;
    ppp_hdlc_rx_t  rx;
    size_t   tx_head, tx_tail;
    uint8_t  txbuf[PPP_TX_BUF];

    /* compressor (transmit) and decompressor (receive) of the algorithm
     * agreed for each direction, and one packet's worth of work area each */
    union { ppp_deflate_enc_t d; ppp_mppc_enc_t m; } cenc;
    union { ppp_deflate_dec_t d; ppp_mppc_dec_t m; } cdec;
    uint8_t  cbuf_tx[PPP_MAX_MRU + 8], cbuf_rx[PPP_MAX_MRU + 8];
} ppp_t;

/* Fill cfg with the defaults described above. */
void ppp_config_default(ppp_config_t *cfg);

/* Initialise a session (phase Dead). cfg is copied; the strings, the user
 * table and the callbacks it points to must stay valid. */
void ppp_init(ppp_t *p, const ppp_config_t *cfg);

/* Carrier is up: start LCP. Carrier is gone: drop everything, phase Dead.
 * Once a session has ended (phase Dead) all input is ignored until the
 * carrier has cycled through ppp_lower_down() and ppp_lower_up(). */
void ppp_lower_up(ppp_t *p);
void ppp_lower_down(ppp_t *p);

/* Close the link gracefully (Terminate-Request ...). The phase reaches
 * Dead once the peer acknowledges or the retries run out. */
void ppp_close(ppp_t *p);

/* Feed octets received from the modem. */
void ppp_input_bytes(ppp_t *p, const uint8_t *buf, size_t n);

/* Pull up to max octets to transmit to the modem; returns the count.
 * ppp_output_pending() tells how many are waiting. */
size_t ppp_output_bytes(ppp_t *p, uint8_t *buf, size_t max);
size_t ppp_output_pending(const ppp_t *p);

/* On a link that loses frames (no error control underneath), every lost
 * compressed packet costs the ones queued behind it: they were compressed
 * against a history the peer no longer has. This bounds that queue: with
 * more than `octets` waiting in the transmit buffer, ppp_send_ip() answers
 * PPP_ERR_FULL while a compressor is on. 0 = no bound (the default after
 * ppp_init). The owner sets it when it knows what the link is. */
void ppp_set_comp_queue(ppp_t *p, size_t octets);

/* Queue an IP packet for the peer. Returns 0 on success, PPP_ERR_DOWN if
 * IPCP is not Opened, PPP_ERR_SIZE if it exceeds the peer's MRU,
 * PPP_ERR_FULL if the transmit buffer has no room right now, or (with
 * compression on) the compressor's allowance is used up: try again after
 * the next ppp_tick. With compression on the packet is compressed here. */
#define PPP_ERR_DOWN  (-1)
#define PPP_ERR_SIZE  (-2)
#define PPP_ERR_FULL  (-3)
int ppp_send_ip(ppp_t *p, const uint8_t *pkt, size_t len);

/* Advance time by ms milliseconds (restart timers, keepalive, auth
 * timeout). Call it regularly, e.g. every 10..100 ms. */
void ppp_tick(ppp_t *p, uint32_t ms);

/* Mix caller-provided randomness into the nonce generator. */
void ppp_add_entropy(ppp_t *p, const uint8_t *data, size_t n);

/* Snapshot of the session. Counters restart at each ppp_lower_up(). */
void ppp_get_status(const ppp_t *p, ppp_status_t *st);

#endif
