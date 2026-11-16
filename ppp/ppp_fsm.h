/* The PPP option negotiation automaton (RFC 1661 section 4), independent of
 * any particular control protocol. LCP and IPCP each own one instance and
 * supply the option handling through ppp_fsm_ops_t.
 *
 * No allocation, no OS dependencies. Time arrives through ppp_fsm_tick(). */
#ifndef V90D_PPP_FSM_H
#define V90D_PPP_FSM_H
#include <stddef.h>
#include <stdint.h>

/* states, numbered as in the RFC 1661 state transition table */
enum {
    PPP_FSM_INITIAL = 0, PPP_FSM_STARTING, PPP_FSM_CLOSED, PPP_FSM_STOPPED,
    PPP_FSM_CLOSING, PPP_FSM_STOPPING, PPP_FSM_REQ_SENT, PPP_FSM_ACK_RCVD,
    PPP_FSM_ACK_SENT, PPP_FSM_OPENED
};

/* packet codes common to all control protocols (RFC 1661 section 5) */
enum {
    PPP_CONF_REQ = 1, PPP_CONF_ACK, PPP_CONF_NAK, PPP_CONF_REJ,
    PPP_TERM_REQ, PPP_TERM_ACK, PPP_CODE_REJ
};

/* what ops->ext_code tells the automaton about a code above 7 */
enum {
    PPP_FSM_EXT_NONE = 0,   /* consumed, no automaton event */
    PPP_FSM_EXT_RUC,        /* unknown code: send Code-Reject */
    PPP_FSM_EXT_RXJ_GOOD,   /* a reject we can live with */
    PPP_FSM_EXT_RXJ_BAD,    /* a catastrophic reject */
    PPP_FSM_EXT_RXR         /* echo/discard class */
};

typedef struct ppp_fsm ppp_fsm_t;

typedef struct {
    /* Transmit one packet of this protocol: code, identifier, and a data
     * field made of a[0..alen) followed by b[0..blen). The callee adds the
     * length field and all framing. */
    void (*send)(ppp_fsm_t *f, uint8_t code, uint8_t id,
                 const uint8_t *a, size_t alen, const uint8_t *b, size_t blen);
    /* A fresh negotiation starts: restore the options we want to request. */
    void (*reset)(ppp_fsm_t *f);
    /* Write the options of our Configure-Request; return their length.
     * Must be deterministic for the current option state (it is called
     * again to validate a Configure-Ack). */
    size_t (*build_cr)(ppp_fsm_t *f, uint8_t *opts, size_t max);
    /* Judge the peer's Configure-Request options (already checked to be a
     * well-formed option list). Return PPP_CONF_ACK, or PPP_CONF_NAK /
     * PPP_CONF_REJ with the reply options written to out and *outlen. */
    int (*recv_cr)(ppp_fsm_t *f, const uint8_t *opts, size_t len,
                   uint8_t *out, size_t max, size_t *outlen);
    /* The peer answered our Configure-Request (identifier matched). */
    void (*recv_ack)(ppp_fsm_t *f, const uint8_t *opts, size_t len);
    void (*recv_nak)(ppp_fsm_t *f, const uint8_t *opts, size_t len);
    void (*recv_rej)(ppp_fsm_t *f, const uint8_t *opts, size_t len);
    /* This-Layer-Up / Down / Started / Finished. Called after the state
     * variable has been updated. Any of these may be NULL. */
    void (*up)(ppp_fsm_t *f);
    void (*down)(ppp_fsm_t *f);
    void (*started)(ppp_fsm_t *f);
    void (*finished)(ppp_fsm_t *f);
    /* A packet with a code above 7 arrived; data is the field after the
     * 4-octet header. Returns a PPP_FSM_EXT_* value. NULL: all unknown. */
    int (*ext_code)(ppp_fsm_t *f, uint8_t code, uint8_t id,
                    const uint8_t *data, size_t len);
} ppp_fsm_ops_t;

struct ppp_fsm {
    const ppp_fsm_ops_t *ops;
    void     *ctx;            /* owner's pointer, untouched by the automaton */
    uint8_t  *scratch;        /* work area for option lists */
    size_t    scratch_size;
    /* configuration (RFC 1661 4.6); ppp_fsm_init sets the RFC defaults */
    uint32_t  restart_ms;     /* Restart timer, default 3000 */
    uint8_t   max_configure;  /* default 10 */
    uint8_t   max_terminate;  /* default 2 */
    uint8_t   max_failure;    /* default 5 */
    uint16_t  peer_mru;       /* Code-Rejects are truncated to this; 0 = 1500 */
    /* state */
    uint8_t   state;
    uint8_t   next_id;        /* identifier for the next request we originate */
    uint8_t   req_id;         /* identifier of our last Configure-Request */
    uint8_t   restart_count;
    uint8_t   failures;       /* Configure-Naks sent since the last Ack */
    uint8_t   timer_on;
    uint32_t  timer_left;     /* ms until the Restart timer expires */
    uint16_t  rejected_codes; /* bit n set: peer Code-Rejected code n (8..15) */
};

void ppp_fsm_init(ppp_fsm_t *f, const ppp_fsm_ops_t *ops, void *ctx,
                  uint8_t *scratch, size_t scratch_size);

/* the four external events */
void ppp_fsm_up(ppp_fsm_t *f);
void ppp_fsm_down(ppp_fsm_t *f);
void ppp_fsm_open(ppp_fsm_t *f);
void ppp_fsm_close(ppp_fsm_t *f);

/* Advance time; fires TO+ / TO- when the Restart timer expires. */
void ppp_fsm_tick(ppp_fsm_t *f, uint32_t ms);

/* A packet for this protocol arrived (starting at the code octet). */
void ppp_fsm_input(ppp_fsm_t *f, const uint8_t *pkt, size_t len);

/* The peer Protocol-Rejected this whole protocol: the RXJ- event. */
void ppp_fsm_protocol_rejected(ppp_fsm_t *f);

/* Take the next identifier for a packet the owner originates itself
 * (Echo-Request, Protocol-Reject). */
uint8_t ppp_fsm_new_id(ppp_fsm_t *f);

/* Walk an option list: returns 1 and sets type, value pointer and value
 * length for the option at *pos, advancing *pos; 0 at the end. The list
 * must be well formed (ppp_fsm_options_ok). */
int ppp_fsm_next_option(const uint8_t *opts, size_t len, size_t *pos,
                        uint8_t *type, const uint8_t **val, size_t *vlen);
int ppp_fsm_options_ok(const uint8_t *opts, size_t len);

#endif
