/* RFC 1661 section 4 option negotiation automaton. See ppp_fsm.h.
 *
 * Each event handler below is one row of the state transition table in
 * RFC 1661 4.1; the comments quote the row. Transitions the table marks
 * illegal ("-") are ignored. */
#include <string.h>
#include "ppp_fsm.h"

#define ST(f) ((f)->state)

static int timer_state(int s)
{
    return s >= PPP_FSM_CLOSING && s <= PPP_FSM_ACK_SENT;
}

/* change state; the Restart timer only runs in states 4..8 */
static void set_state(ppp_fsm_t *f, int s)
{
    f->state = (uint8_t)s;
    if (!timer_state(s))
        f->timer_on = 0;
}

static void start_timer(ppp_fsm_t *f)
{
    f->timer_on = 1;
    f->timer_left = f->restart_ms;
}

static void tlu(ppp_fsm_t *f) { if (f->ops->up) f->ops->up(f); }
static void tld(ppp_fsm_t *f) { if (f->ops->down) f->ops->down(f); }
static void tls(ppp_fsm_t *f) { if (f->ops->started) f->ops->started(f); }
static void tlf(ppp_fsm_t *f) { if (f->ops->finished) f->ops->finished(f); }

static void irc_configure(ppp_fsm_t *f) { f->restart_count = f->max_configure; }
static void irc_terminate(ppp_fsm_t *f) { f->restart_count = f->max_terminate; }

static void zrc(ppp_fsm_t *f)
{
    f->restart_count = 0;
    start_timer(f);
}

static void reset_options(ppp_fsm_t *f)
{
    f->failures = 0;
    if (f->ops->reset)
        f->ops->reset(f);
}

/* again != 0: a retransmission of the request that is outstanding. It keeps
 * its identifier (RFC 1661 5.1 allows that while the options are unchanged),
 * so an Ack that was slower than the Restart timer still matches. */
static void scr_id(ppp_fsm_t *f, int again)
{
    size_t n;

    if (f->restart_count > 0)
        f->restart_count--;
    if (!again)
        f->req_id = f->next_id++;
    n = f->ops->build_cr(f, f->scratch, f->scratch_size);
    f->ops->send(f, PPP_CONF_REQ, f->req_id, f->scratch, n, NULL, 0);
    start_timer(f);
}

static void scr(ppp_fsm_t *f) { scr_id(f, 0); }

static void str(ppp_fsm_t *f)
{
    if (f->restart_count > 0)
        f->restart_count--;
    f->ops->send(f, PPP_TERM_REQ, f->next_id++, NULL, 0, NULL, 0);
    start_timer(f);
}

static void sta(ppp_fsm_t *f, uint8_t id)
{
    f->ops->send(f, PPP_TERM_ACK, id, NULL, 0, NULL, 0);
}

/* Code-Reject: the data field is a copy of the rejected packet, truncated
 * so the whole Code-Reject fits the peer's MRU */
static void scj(ppp_fsm_t *f, const uint8_t *pkt, size_t len)
{
    size_t mru = f->peer_mru ? f->peer_mru : 1500;

    if (len > mru - 4)
        len = mru - 4;
    f->ops->send(f, PPP_CODE_REJ, f->next_id++, pkt, len, NULL, 0);
}

void ppp_fsm_init(ppp_fsm_t *f, const ppp_fsm_ops_t *ops, void *ctx,
                  uint8_t *scratch, size_t scratch_size)
{
    memset(f, 0, sizeof *f);
    f->ops = ops;
    f->ctx = ctx;
    f->scratch = scratch;
    f->scratch_size = scratch_size;
    f->restart_ms = 3000;
    f->max_configure = 10;
    f->max_terminate = 2;
    f->max_failure = 5;
    f->state = PPP_FSM_INITIAL;
    f->next_id = 1;
}

uint8_t ppp_fsm_new_id(ppp_fsm_t *f)
{
    return f->next_id++;
}

/* Up:   2  irc,scr/6  -  -  -  -  -  -  -  - */
void ppp_fsm_up(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_INITIAL:
        set_state(f, PPP_FSM_CLOSED);
        break;
    case PPP_FSM_STARTING:
        set_state(f, PPP_FSM_REQ_SENT);
        reset_options(f);
        irc_configure(f);
        scr(f);
        break;
    default:
        break;
    }
}

/* Down:  -  -  0  tls/1  0  1  1  1  1  tld/1 */
void ppp_fsm_down(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_CLOSED:
    case PPP_FSM_CLOSING:
        set_state(f, PPP_FSM_INITIAL);
        break;
    case PPP_FSM_STOPPED:
        set_state(f, PPP_FSM_STARTING);
        tls(f);
        break;
    case PPP_FSM_STOPPING:
    case PPP_FSM_REQ_SENT:
    case PPP_FSM_ACK_RCVD:
    case PPP_FSM_ACK_SENT:
        set_state(f, PPP_FSM_STARTING);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_STARTING);
        tld(f);
        break;
    default:
        break;
    }
}

/* Open:  tls/1  1  irc,scr/6  3r  5r  5r  6  7  8  9r
 * The optional restart behaviour [r] is not implemented. */
void ppp_fsm_open(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_INITIAL:
        set_state(f, PPP_FSM_STARTING);
        tls(f);
        break;
    case PPP_FSM_CLOSED:
        set_state(f, PPP_FSM_REQ_SENT);
        reset_options(f);
        irc_configure(f);
        scr(f);
        break;
    case PPP_FSM_CLOSING:
        set_state(f, PPP_FSM_STOPPING);
        break;
    default:
        break;
    }
}

/* Close:  0  tlf/0  2  2  4  4  irc,str/4  irc,str/4  irc,str/4
 *         tld,irc,str/4 */
void ppp_fsm_close(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_STARTING:
        set_state(f, PPP_FSM_INITIAL);
        tlf(f);
        break;
    case PPP_FSM_STOPPED:
        set_state(f, PPP_FSM_CLOSED);
        break;
    case PPP_FSM_STOPPING:
        set_state(f, PPP_FSM_CLOSING);
        break;
    case PPP_FSM_REQ_SENT:
    case PPP_FSM_ACK_RCVD:
    case PPP_FSM_ACK_SENT:
        set_state(f, PPP_FSM_CLOSING);
        irc_terminate(f);
        str(f);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_CLOSING);
        tld(f);
        irc_terminate(f);
        str(f);
        break;
    default:
        break;
    }
}

/* TO+:  -  -  -  -  str/4  str/5  scr/6  scr/6  scr/8  -
 * TO-:  -  -  -  -  tlf/2  tlf/3  tlf/3p tlf/3p tlf/3p -
 * The passive option [p] is not implemented. */
static void timeout(ppp_fsm_t *f)
{
    if (f->restart_count > 0) {
        switch (ST(f)) {
        case PPP_FSM_CLOSING:
        case PPP_FSM_STOPPING:
            str(f);
            break;
        case PPP_FSM_ACK_RCVD:
            set_state(f, PPP_FSM_REQ_SENT);
            scr(f);
            break;
        case PPP_FSM_REQ_SENT:
        case PPP_FSM_ACK_SENT:
            scr_id(f, 1);
            break;
        default:
            break;
        }
    } else {
        switch (ST(f)) {
        case PPP_FSM_CLOSING:
            set_state(f, PPP_FSM_CLOSED);
            tlf(f);
            break;
        case PPP_FSM_STOPPING:
        case PPP_FSM_REQ_SENT:
        case PPP_FSM_ACK_RCVD:
        case PPP_FSM_ACK_SENT:
            set_state(f, PPP_FSM_STOPPED);
            tlf(f);
            break;
        default:
            break;
        }
    }
}

void ppp_fsm_tick(ppp_fsm_t *f, uint32_t ms)
{
    if (!f->timer_on)
        return;
    if (ms < f->timer_left) {
        f->timer_left -= ms;
        return;
    }
    f->timer_on = 0;
    f->timer_left = 0;
    timeout(f);
}

/* RCR+:  -  -  sta/2  irc,scr,sca/8  4  5  sca/8  sca,tlu/9  sca/8
 *        tld,scr,sca/8
 * RCR-:  -  -  sta/2  irc,scr,scn/6  4  5  scn/6  scn/7      scn/6
 *        tld,scr,scn/6 */
static void rcr(ppp_fsm_t *f, uint8_t id, const uint8_t *opts, size_t len)
{
    size_t outlen = 0;
    int verdict, was_ack_rcvd = 0;

    switch (ST(f)) {
    case PPP_FSM_CLOSED:
        sta(f, id);
        return;
    case PPP_FSM_STOPPED:
        set_state(f, PPP_FSM_REQ_SENT);
        reset_options(f);
        irc_configure(f);
        scr(f);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_REQ_SENT);
        tld(f);
        reset_options(f);
        scr(f);
        break;
    case PPP_FSM_ACK_RCVD:
        was_ack_rcvd = 1;
        break;
    case PPP_FSM_REQ_SENT:
    case PPP_FSM_ACK_SENT:
        break;
    default:
        return;
    }
    /* our own request (if any) is out; scratch is free for the reply */
    verdict = f->ops->recv_cr(f, opts, len, f->scratch, f->scratch_size, &outlen);
    if (verdict == PPP_CONF_ACK) {
        f->failures = 0;
        f->ops->send(f, PPP_CONF_ACK, id, opts, len, NULL, 0);
        if (was_ack_rcvd) {
            set_state(f, PPP_FSM_OPENED);
            tlu(f);
        } else {
            set_state(f, PPP_FSM_ACK_SENT);
        }
    } else {
        if (verdict == PPP_CONF_NAK && f->failures < 255)
            f->failures++;
        f->ops->send(f, (uint8_t)verdict, id, f->scratch, outlen, NULL, 0);
        set_state(f, was_ack_rcvd ? PPP_FSM_ACK_RCVD : PPP_FSM_REQ_SENT);
    }
}

/* RCA:  -  -  sta/2  sta/3  4  5  irc/7  scr/6x  irc,tlu/9  tld,scr/6x */
static void rca(ppp_fsm_t *f, uint8_t id, const uint8_t *opts, size_t len)
{
    size_t n;

    switch (ST(f)) {
    case PPP_FSM_CLOSED:
    case PPP_FSM_STOPPED:
        sta(f, id);
        return;
    case PPP_FSM_CLOSING:
    case PPP_FSM_STOPPING:
        return;
    default:
        break;
    }
    /* RFC 1661 5.2: the identifier must match our last request and the
     * options must be exactly the ones we sent; otherwise discard */
    if (id != f->req_id)
        return;
    n = f->ops->build_cr(f, f->scratch, f->scratch_size);
    if (n != len || (n && memcmp(f->scratch, opts, n) != 0))
        return;

    switch (ST(f)) {
    case PPP_FSM_REQ_SENT:
        f->ops->recv_ack(f, opts, len);
        irc_configure(f);
        set_state(f, PPP_FSM_ACK_RCVD);
        break;
    case PPP_FSM_ACK_RCVD:          /* crossed connection */
        set_state(f, PPP_FSM_REQ_SENT);
        scr(f);
        break;
    case PPP_FSM_ACK_SENT:
        f->ops->recv_ack(f, opts, len);
        irc_configure(f);
        set_state(f, PPP_FSM_OPENED);
        tlu(f);
        break;
    case PPP_FSM_OPENED:            /* crossed connection */
        set_state(f, PPP_FSM_REQ_SENT);
        tld(f);
        reset_options(f);
        scr(f);
        break;
    default:
        break;
    }
}

/* RCN:  -  -  sta/2  sta/3  4  5  irc,scr/6  scr/6x  irc,scr/8
 *       tld,scr/6x */
static void rcn(ppp_fsm_t *f, uint8_t code, uint8_t id,
                const uint8_t *opts, size_t len)
{
    int st = ST(f);

    switch (st) {
    case PPP_FSM_CLOSED:
    case PPP_FSM_STOPPED:
        sta(f, id);
        return;
    case PPP_FSM_CLOSING:
    case PPP_FSM_STOPPING:
        return;
    default:
        break;
    }
    if (id != f->req_id)
        return;

    switch (st) {
    case PPP_FSM_REQ_SENT:
    case PPP_FSM_ACK_SENT:
        if (code == PPP_CONF_NAK)
            f->ops->recv_nak(f, opts, len);
        else
            f->ops->recv_rej(f, opts, len);
        if (ST(f) != st)
            return;                 /* the protocol gave up and closed */
        irc_configure(f);
        scr(f);
        break;
    case PPP_FSM_ACK_RCVD:          /* crossed connection */
        set_state(f, PPP_FSM_REQ_SENT);
        scr(f);
        break;
    case PPP_FSM_OPENED:            /* crossed connection */
        set_state(f, PPP_FSM_REQ_SENT);
        tld(f);
        reset_options(f);
        scr(f);
        break;
    default:
        break;
    }
}

/* RTR:  -  -  sta/2  sta/3  sta/4  sta/5  sta/6  sta/6  sta/6
 *       tld,zrc,sta/5 */
static void rtr(ppp_fsm_t *f, uint8_t id)
{
    switch (ST(f)) {
    case PPP_FSM_INITIAL:
    case PPP_FSM_STARTING:
        return;
    case PPP_FSM_ACK_RCVD:
    case PPP_FSM_ACK_SENT:
        set_state(f, PPP_FSM_REQ_SENT);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_STOPPING);
        tld(f);
        zrc(f);
        break;
    default:
        break;
    }
    sta(f, id);
}

/* RTA:  -  -  2  3  tlf/2  tlf/3  6  6  8  tld,scr/6 */
static void rta(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_CLOSING:
        set_state(f, PPP_FSM_CLOSED);
        tlf(f);
        break;
    case PPP_FSM_STOPPING:
        set_state(f, PPP_FSM_STOPPED);
        tlf(f);
        break;
    case PPP_FSM_ACK_RCVD:
        set_state(f, PPP_FSM_REQ_SENT);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_REQ_SENT);
        tld(f);
        reset_options(f);
        scr(f);
        break;
    default:
        break;
    }
}

/* RXJ+:  -  -  2  3  4  5  6  6  8  9 */
static void rxj_good(ppp_fsm_t *f)
{
    if (ST(f) == PPP_FSM_ACK_RCVD)
        set_state(f, PPP_FSM_REQ_SENT);
}

/* RXJ-:  -  -  tlf/2  tlf/3  tlf/2  tlf/3  tlf/3  tlf/3  tlf/3
 *        tld,irc,str/5 */
static void rxj_bad(ppp_fsm_t *f)
{
    switch (ST(f)) {
    case PPP_FSM_CLOSED:
    case PPP_FSM_CLOSING:
        set_state(f, PPP_FSM_CLOSED);
        tlf(f);
        break;
    case PPP_FSM_STOPPED:
    case PPP_FSM_STOPPING:
    case PPP_FSM_REQ_SENT:
    case PPP_FSM_ACK_RCVD:
    case PPP_FSM_ACK_SENT:
        set_state(f, PPP_FSM_STOPPED);
        tlf(f);
        break;
    case PPP_FSM_OPENED:
        set_state(f, PPP_FSM_STOPPING);
        tld(f);
        irc_terminate(f);
        str(f);
        break;
    default:
        break;
    }
}

void ppp_fsm_protocol_rejected(ppp_fsm_t *f)
{
    rxj_bad(f);
}

int ppp_fsm_options_ok(const uint8_t *o, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        if (len - pos < 2 || o[pos + 1] < 2 || o[pos + 1] > len - pos)
            return 0;
        pos += o[pos + 1];
    }
    return 1;
}

int ppp_fsm_next_option(const uint8_t *o, size_t len, size_t *pos,
                        uint8_t *type, const uint8_t **val, size_t *vlen)
{
    size_t p = *pos;

    if (p >= len || len - p < 2 || o[p + 1] < 2 || o[p + 1] > len - p)
        return 0;
    *type = o[p];
    *val = o + p + 2;
    *vlen = (size_t)o[p + 1] - 2;
    *pos = p + o[p + 1];
    return 1;
}

void ppp_fsm_input(ppp_fsm_t *f, const uint8_t *pkt, size_t len)
{
    uint8_t code, id;
    size_t plen;
    const uint8_t *data;
    size_t dlen;

    /* RFC 1661 5: octets beyond the Length field are padding; a Length
     * larger than what arrived, or below the header size, is invalid */
    if (len < 4)
        return;
    plen = ((size_t)pkt[2] << 8) | pkt[3];
    if (plen < 4 || plen > len)
        return;
    /* nothing can be received before the lower layer is up */
    if (ST(f) == PPP_FSM_INITIAL || ST(f) == PPP_FSM_STARTING)
        return;
    code = pkt[0];
    id = pkt[1];
    data = pkt + 4;
    dlen = plen - 4;

    switch (code) {
    case PPP_CONF_REQ:
        if (ppp_fsm_options_ok(data, dlen))
            rcr(f, id, data, dlen);
        break;
    case PPP_CONF_ACK:
        rca(f, id, data, dlen);
        break;
    case PPP_CONF_NAK:
    case PPP_CONF_REJ:
        if (ppp_fsm_options_ok(data, dlen))
            rcn(f, code, id, data, dlen);
        break;
    case PPP_TERM_REQ:
        rtr(f, id);
        break;
    case PPP_TERM_ACK:
        rta(f);
        break;
    case PPP_CODE_REJ:
        /* the rejected packet starts with its code. Losing any of the
         * codes 1..7 makes negotiation impossible; extension codes are
         * optional features we can do without. */
        if (dlen < 1)
            break;
        if (data[0] >= PPP_CONF_REQ && data[0] <= PPP_CODE_REJ) {
            rxj_bad(f);
        } else {
            if (data[0] < 16)
                f->rejected_codes |= (uint16_t)(1u << data[0]);
            rxj_good(f);
        }
        break;
    default: {
        int ext = f->ops->ext_code
                ? f->ops->ext_code(f, code, id, data, dlen)
                : PPP_FSM_EXT_RUC;
        switch (ext) {
        case PPP_FSM_EXT_RUC:      scj(f, pkt, plen); break;
        case PPP_FSM_EXT_RXJ_GOOD: rxj_good(f);       break;
        case PPP_FSM_EXT_RXJ_BAD:  rxj_bad(f);        break;
        default:                   break;   /* RXR and NONE: no transition */
        }
        break;
    }
    }
}
