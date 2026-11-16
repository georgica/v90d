/* Test side only: the CALLING end of a dial-up call, the mirror image of
 * dce.h: a calling modem + a V.42 originator (or a plain start-stop
 * terminal, to play a modem without error control) + the PPP test client.
 * Never shipped.
 *
 * The calling modem is sim/callpeer (V.8, V.22bis / V.21) or, with cfg.v90,
 * a V.90 analogue modem chained from sim/v8call (CM with the PCM category),
 * sim/p2a (Phase 2) and sim/p34a (Phases 3 and 4, data). The latter reads
 * the answerer's transmitted PCM octets directly (see p34a.h), so the
 * harness has to hand them over: caller_tick_pcm(). If the answerer's JM
 * does not agree to PCM operation the V.90 caller goes on as a V.22bis
 * caller.
 *
 * With cfg.v92 the calling modem is sim/v92call: a V.92 analogue modem
 * that brings its own line (V.8, Phase 2, then PCM upstream or V.90 as its
 * INFO1a says, retrains). The harness then exchanges octets with it
 * directly: caller_tick_v92(). Such a caller skips the ODP of V.42 when
 * both modems said V.92 and LAPM (V.92 9.3.1). */
#ifndef V90D_DCE_TEST_CALLER_H
#define V90D_DCE_TEST_CALLER_H
#include <stddef.h>
#include <stdint.h>
#include "callpeer.h"
#include "p2a.h"
#include "p34a.h"
#include "v42.h"
#include "pppc.h"
#include "v34peer.h"
#include "../../v32/v32.h"
#include "v32call.h"
#include "v92call.h"

typedef struct {
    int      use_v8;           /* speak V.8 (else a pre-V.8 V.22bis caller) */
    int      v90;              /* a V.90 analogue modem (V.8 with PCM in CM; modes ignored) */
    int      v34;              /* a V.34 modem without PCM (sim/v34peer; modes ignored) */
    int      v32;              /* a V.32bis modem without V.8: AA after a second of answer tone (sim/v32call) */
    int      v92;              /* a V.92 modem with its own line (sim/v92call; use caller_tick_v92):
                                  1 = PCM upstream enabled, 2 = V.92 with PCM upstream switched off */
    int      v92_rx_linear;    /* its line ends in a codec with a linear A/D at the answerer
                                  (the harness reads vc.rx_lin instead of the octet) */
    int      v92_mute;         /* fault: its first PCM upstream start-ups send nothing (v92call mute_pcm) */
    int      alaw;             /* V.90: the law of the answerer's codec */
    unsigned modes;            /* V8_MOD_* offered in CM */
    int      allow2400;
    int      level;            /* transmit RMS */
    int      use_v42;          /* 1 = V.42 originator, 0 = no error control */
    v42ec_cfg_t   v42;
    int      fixed_timers;     /* 1 = do not scale T401 and PPP Restart (see dce.h) */
    pppc_config_t ppp;
    int      tx_busy_mark;     /* PPP transmit backlog in octets above which caller_send_ip()
                                  says "busy"; 0 = 4096 */
    void *ctx;
    void (*ip_rx)(void *ctx, const uint8_t *pkt, size_t len);
    /* Both set: the built-in PPP client is left out and the octets of the
     * data connection are the owner's (srv/test/ns_pppd.c puts the real
     * pppd there). raw_tx returns how many octets it put into b. */
    void   (*raw_rx)(void *ctx, const uint8_t *b, size_t n);
    size_t (*raw_tx)(void *ctx, uint8_t *b, size_t max);
} caller_cfg_t;

enum { CALLER_ST_PEER = 0, CALLER_ST_V8, CALLER_ST_P2, CALLER_ST_P34, CALLER_ST_FAILED,
       CALLER_ST_RTSIL /* the 70 ms of silence that begin a retrain */,
       CALLER_ST_V34 /* sim/v34peer from its CM on */, CALLER_ST_V32,
       CALLER_ST_V92 /* sim/v92call for the whole call */ };

typedef struct {
    caller_cfg_t cfg;
    callpeer_t   modem;        /* V.22bis / V.21 (and the start-stop terminal, modem.dte) */
    int          stage;        /* CALLER_ST_* */
    v8call_t     v8;           /* V.90 caller: its own V.8, then Phase 2, then the rest */
    p2a_t        p2;
    p34a_t       p34;
    v34peer_t    v34;
    v32call_t    v32; int v32_wait;
    v92call_t    vc;
    int          odp_bypass;   /* V.92 9.3.1 applied: no ODP was sent */
    int      connected;        /* the modem is in data mode */
    int      tx_rate, rx_rate; /* bit/s once connected (upstream, downstream) */
    v42ec_t      v42;
    pppc_t       ppp;
    int      v42_on, ppp_on, on_hook, sub;
    int      ip_seen;          /* pppc ip_count already reported */
    uint8_t  stash[256];       /* PPP octets the lower layer had no room for */
    size_t   stash_n;
    uint32_t ms;
    /* V.90 retrain (9.5.2) */
    dpskrx_t toneb;            /* listens for Tone B while in data mode */
    int      rt_n;             /* samples of silence so far */
    int      retrains;         /* retrains begun */
    int      retraining;       /* from the silence until data mode is back */
} caller_t;

/* V.8, V.22bis + V.21 offered, 2400 allowed, V.42 with V.42 bis, PPP client
 * "alice" / "wonderland" doing CHAP or PAP */
void    caller_cfg_default(caller_cfg_t *cfg);
void    caller_init(caller_t *c, const caller_cfg_t *cfg);
/* V.90, in data mode: start a retrain (9.5.2.1) */
void    caller_retrain(caller_t *c);
/* One 8 kHz sample: rx is the line signal received, ds_octet the PCM octet
 * the answerer transmitted in this sample (only a V.90 caller looks at it).
 * Returns the sample to transmit. */
double  caller_tick_pcm(caller_t *c, int16_t rx, uint8_t ds_octet);
/* A V.92 caller (cfg.v92): the octet the answerer transmitted in, the octet
 * the answerer receives out. */
uint8_t caller_tick_v92(caller_t *c, uint8_t ds_octet);
/* the same for callers that never need the octet */
int16_t caller_tick(caller_t *c, int16_t rx);
/* 0 = queued, -1 = IP not up, -2 = busy, try again later */
int     caller_send_ip(caller_t *c, const uint8_t *pkt, size_t len);
/* go on-hook at once: silence from now on, nothing is told to the far end */
void    caller_hangup(caller_t *c);
/* 1 once the layer under PPP is up (V.42 connected or start-stop) */
int     caller_link_up(const caller_t *c);

#endif
