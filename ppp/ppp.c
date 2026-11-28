/* PPP server session: phases (RFC 1661 3.2), LCP, PAP / CHAP / MS-CHAPv2
 * authenticator, IPCP with address assignment and DNS, CCP with PPP Deflate,
 * MPPC and MPPE.
 * See ppp.h for the API. */
#include <string.h>
#include "ppp.h"
#include "ppp_md5.h"

/* LCP configuration options (RFC 1661 section 6) */
#define LCP_OPT_MRU      1
#define LCP_OPT_ACCM     2
#define LCP_OPT_AUTH     3
#define LCP_OPT_MAGIC    5
#define LCP_OPT_PFC      7
#define LCP_OPT_ACFC     8
/* LCP codes beyond the common seven (RFC 1661 section 5) */
#define LCP_PROTO_REJ    8
#define LCP_ECHO_REQ     9
#define LCP_ECHO_REP     10
#define LCP_DISCARD_REQ  11
/* IPCP options (RFC 1332 section 3, RFC 1877 section 1) */
#define IPCP_OPT_COMP    2   /* IP-Compression-Protocol (RFC 1332 3.2) */
#define IPCP_OPT_ADDR    3
#define IPCP_OPT_DNS1    129
#define IPCP_OPT_DNS2    131
/* PAP codes (RFC 1334 2.2) and CHAP codes (RFC 1994 section 4) */
#define PAP_REQUEST      1
#define PAP_ACK          2
#define PAP_NAK          3
#define CHAP_CHALLENGE   1
#define CHAP_RESPONSE    2
#define CHAP_SUCCESS     3
#define CHAP_FAILURE     4
#define CHAP_ALG_MD5     5
#define CHAP_ALG_MSCHAP1 0x80   /* MS-CHAP v1 (RFC 2433) */
#define CHAP_ALG_MSCHAP2 0x81

/* CCP codes beyond the common seven (RFC 1962 section 3) and the options
 * we know (RFC 1962 section 4, RFC 2118 section 2, RFC 1979 section 3; 24
 * is the number PPP Deflate had in its draft, still offered by pppd) */
#define CCP_RESET_REQ    14
#define CCP_RESET_ACK    15
#define CCP_OPT_MPPC     18
#define CCP_OPT_DEFLATE_DRAFT 24
#define CCP_OPT_DEFLATE  26
#define DEFLATE_METHOD   8   /* RFC 1979 section 3 */
#define MPPC_BIT         1u  /* RFC 2118 section 2 */
/* RFC 3078 MPPE bits in the same option 18. We offer and accept one key
 * length, in either mode: 128-bit, or for a peer that has only shorter
 * keys the strongest of 56 and 40 it has (Windows 9x/ME without the
 * 128-bit upgrade: 40). We ask for stateless (what pppd does) and go
 * stateful when the peer Naks toward it or asks for it -- a Windows client
 * on a modem link does stateful only (issue #6). */
#define MPPE_BIT_S       0x00000040u  /* 128-bit session keys */
#define MPPE_BIT_M       0x00000080u  /* 56-bit */
#define MPPE_BIT_L       0x00000020u  /* 40-bit */
#define MPPE_KEYS        (MPPE_BIT_S | MPPE_BIT_M | MPPE_BIT_L)
#define MPPE_BIT_H       0x01000000u  /* stateless */
#define RESET_TRIES      3   /* Reset-Requests before CCP is negotiated anew */
#define CW_NAK_LIMIT     3   /* Naks of one of our options before we drop it */

#define LOOP_LIMIT       5   /* own magic seen this often: looped back */

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(((unsigned)p[0] << 8) | p[1]);
}

/* the three CHAP variants: MD5, MS-CHAP v1, MS-CHAPv2 */
static int is_chap(unsigned a)
{
    return a == PPP_AUTH_CHAP || a == PPP_AUTH_MSCHAPV1 || a == PPP_AUTH_MSCHAPV2;
}

static uint8_t chap_alg(unsigned a)
{
    return a == PPP_AUTH_MSCHAPV1 ? CHAP_ALG_MSCHAP1 : a == PPP_AUTH_MSCHAPV2 ? CHAP_ALG_MSCHAP2 : CHAP_ALG_MD5;
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* ---------------------------------------------------------------- */
/* nonce generator: an MD5-stirred pool. Its quality is that of the seed
 * and of whatever the caller feeds to ppp_add_entropy(). */

static void pool_mix(ppp_t *p, const uint8_t *data, size_t n, uint8_t tag,
                     uint8_t out[16])
{
    ppp_md5_t m;
    uint8_t extra[13];

    put32(extra, p->pool_ctr);
    put32(extra + 4, p->uptime_ms);
    put32(extra + 8, p->rx_octets);
    extra[12] = tag;
    ppp_md5_init(&m);
    ppp_md5_update(&m, p->pool, 16);
    ppp_md5_update(&m, extra, sizeof extra);
    ppp_md5_update(&m, data, n);
    ppp_md5_final(&m, out);
}

void ppp_add_entropy(ppp_t *p, const uint8_t *data, size_t n)
{
    p->pool_ctr++;
    pool_mix(p, data, n, 'e', p->pool);
}

static void nonce(ppp_t *p, uint8_t *out, size_t n)
{
    uint8_t block[16];

    while (n > 0) {
        size_t k = n < 16 ? n : 16;
        p->pool_ctr++;
        pool_mix(p, NULL, 0, 'o', block);     /* output */
        pool_mix(p, NULL, 0, 'n', p->pool);   /* next pool state */
        memcpy(out, block, k);
        out += k;
        n -= k;
    }
}

static uint32_t new_magic(ppp_t *p, uint32_t avoid)
{
    uint8_t b[4];
    uint32_t m;

    do {
        nonce(p, b, 4);
        m = get32(b);
    } while (m == 0 || m == avoid);
    return m;
}

/* ---------------------------------------------------------------- */
/* phase and termination bookkeeping */

static void set_phase(ppp_t *p, ppp_phase_t ph)
{
    if (p->phase == ph)
        return;
    p->phase = ph;
    if (p->cfg.phase_cb)
        p->cfg.phase_cb(p->cfg.ctx, ph);
}

static void set_end(ppp_t *p, ppp_end_t why)
{
    if (p->end_reason == PPP_END_NONE)
        p->end_reason = why;
}

/* ask for the link to be closed once the current packet is dealt with
 * (closing from inside an automaton callback would re-enter it) */
static void request_close(ppp_t *p, ppp_end_t why)
{
    if (p->pending_close == PPP_END_NONE)
        p->pending_close = why;
}

static void do_close(ppp_t *p, ppp_end_t why)
{
    if (p->phase == PPP_PHASE_DEAD)
        return;
    set_end(p, why);
    ppp_fsm_close(&p->lcp);
    if (p->lcp.state == PPP_FSM_CLOSING)
        set_phase(p, PPP_PHASE_TERMINATE);
}

static void run_pending(ppp_t *p)
{
    if (p->pending_close != PPP_END_NONE) {
        ppp_end_t why = p->pending_close;
        p->pending_close = PPP_END_NONE;
        do_close(p, why);
    }
}

/* ---------------------------------------------------------------- */
/* transmit path */

/* Frame one packet into the transmit buffer. The information field is
 * h + a + b. Returns 0, or -1 if there is no room (leaving `reserve`
 * octets untouched). */
/* cfg.trace: a control packet in or out, in up to three pieces */
static void trace(ppp_t *p, int out, uint16_t proto, const uint8_t *h, size_t hn,
                  const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    uint8_t buf[PPP_TRACE_MAX];
    size_t len = hn + an + bn, cap = PPP_TRACE_MAX, n = 0, k;

    if (!p->cfg.trace || proto < 0x8000)
        return;
    if (proto == PPP_PROTO_PAP || proto == PPP_PROTO_CHAP)
        cap = 4;                        /* no secrets in a log */
    k = hn < cap ? hn : cap; memcpy(buf, h, k); n = k;
    k = an < cap - n ? an : cap - n; if (k) memcpy(buf + n, a, k); n += k;
    k = bn < cap - n ? bn : cap - n; if (k) memcpy(buf + n, b, k); n += k;
    /* a Protocol-Reject carries the packet it rejects: one of PAP or CHAP
     * no further than that packet's own header */
    if (proto == PPP_PROTO_LCP && n >= 6 && buf[0] == 8) {
        unsigned rp = (unsigned)buf[4] << 8 | buf[5];
        if ((rp == PPP_PROTO_PAP || rp == PPP_PROTO_CHAP) && n > 10)
            n = 10;
    }
    p->cfg.trace(p->cfg.ctx, out, p->uptime_ms, proto, buf, n, len);
}

static int tx_frame(ppp_t *p, uint16_t proto,
                    const uint8_t *h, size_t hn,
                    const uint8_t *a, size_t an,
                    const uint8_t *b, size_t bn, size_t reserve)
{
    uint8_t hdr[4];
    size_t hl = 0, n = 0;
    uint32_t accm;
    int attempt;

    if (proto == PPP_PROTO_LCP) {
        /* RFC 1662 7: LCP always goes out in the default framing, so the
         * peer can read it whatever state its negotiation is in */
        accm = 0xffffffffu;
        hdr[hl++] = 0xff;
        hdr[hl++] = 0x03;
        hdr[hl++] = (uint8_t)(proto >> 8);
        hdr[hl++] = (uint8_t)proto;
    } else {
        accm = p->tx_accm;
        if (!p->tx_acfc) {
            hdr[hl++] = 0xff;
            hdr[hl++] = 0x03;
        }
        if (!(p->tx_pfc && proto < 0x100))
            hdr[hl++] = (uint8_t)(proto >> 8);
        hdr[hl++] = (uint8_t)proto;
    }

    if (p->tx_head == p->tx_tail)
        p->tx_head = p->tx_tail = 0;
    for (attempt = 0; attempt < 2 && n == 0; attempt++) {
        ppp_hdlc_tx_t tx;
        size_t room = PPP_TX_BUF - p->tx_tail;

        room = room > reserve ? room - reserve : 0;
        /* an opening flag only when the line has been idle */
        ppp_hdlc_tx_begin(&tx, p->txbuf + p->tx_tail, room, accm,
                          p->tx_head == p->tx_tail);
        ppp_hdlc_tx_add(&tx, hdr, hl);
        ppp_hdlc_tx_add(&tx, h, hn);
        ppp_hdlc_tx_add(&tx, a, an);
        ppp_hdlc_tx_add(&tx, b, bn);
        n = ppp_hdlc_tx_end(&tx);
        if (n == 0) {
            if (p->tx_head == 0)
                break;
            /* slide the unsent octets down and try once more */
            memmove(p->txbuf, p->txbuf + p->tx_head, p->tx_tail - p->tx_head);
            p->tx_tail -= p->tx_head;
            p->tx_head = 0;
        }
    }
    if (n == 0)
        return -1;
    p->tx_tail += n;
    p->cnt.tx_frames++;
    trace(p, 1, proto, h, hn, a, an, b, bn);
    return 0;
}

/* a control-protocol style packet: code, identifier, length, data */
static void tx_cp(ppp_t *p, uint16_t proto, uint8_t code, uint8_t id,
                  const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    uint8_t h[4];
    size_t len = 4 + an + bn;

    h[0] = code;
    h[1] = id;
    h[2] = (uint8_t)(len >> 8);
    h[3] = (uint8_t)len;
    if (tx_frame(p, proto, h, 4, a, an, b, bn, 0) != 0)
        p->cnt.tx_dropped++;
}

/* The transmit buffer holds whole frames one behind the other, each ended
 * by a flag, and the flag octet occurs nowhere else in it (RFC 1662 4.2).
 * So the frames still waiting can be told apart, which the two functions
 * below use for the CCP reset exchange: on a slow line without error
 * control the buffer holds half a minute of traffic, and a reset that had
 * to queue behind it would leave the link decompressing nothing for that
 * long. */

/* index just behind the frame that is going out now */
static size_t tx_cut(const ppp_t *p)
{
    size_t i = p->tx_head;

    while (i < p->tx_tail && p->txbuf[i] != PPP_FLAG)
        i++;
    return i < p->tx_tail ? i + 1 : p->tx_tail;
}

/* Take the compressed datagrams that have not begun to go out back out of
 * the buffer: the peer has asked for a reset and discards them anyway
 * (RFC 1962 3.1). Everything else stays, in its order. */
static void tx_purge_compressed(ppp_t *p)
{
    size_t rd = tx_cut(p), wr = rd;

    while (rd < p->tx_tail) {
        size_t e = rd, k = rd, hn = 0;
        uint8_t hd[4];

        while (e < p->tx_tail && p->txbuf[e] != PPP_FLAG)
            e++;
        if (e == p->tx_tail)
            e--;                            /* not reached: frames end in a flag */
        /* the start of the frame with the escapes undone: Address and
         * Control if they are there, then the Protocol field */
        while (k < e && hn < 4) {
            uint8_t c = p->txbuf[k++];

            if (c == PPP_ESCAPE && k < e)
                c = p->txbuf[k++] ^ 0x20;
            hd[hn++] = c;
        }
        k = hn >= 2 && hd[0] == 0xff && hd[1] == 0x03 ? 2 : 0;
        if (hn > k && (hd[k] == PPP_PROTO_COMP ||
                       (hn > k + 1 && hd[k] == 0 && hd[k + 1] == PPP_PROTO_COMP))) {
            p->cnt.comp_tx_purged++;
        } else {
            memmove(p->txbuf + wr, p->txbuf + rd, e + 1 - rd);
            wr += e + 1 - rd;
        }
        rd = e + 1;
    }
    p->tx_tail = wr;
}

/* A control packet without data that goes out next, in front of what is
 * waiting (its place among data frames means nothing). */
static void tx_cp_next(ppp_t *p, uint16_t proto, uint8_t code, uint8_t id)
{
    uint8_t tmp[40];
    size_t held = p->tx_tail - p->tx_head, start, cut, n;

    tx_cp(p, proto, code, id, NULL, 0, NULL, 0);
    if (p->tx_tail - p->tx_head <= held)
        return;                             /* no room: it was dropped */
    start = p->tx_head + held;
    n = p->tx_tail - start;
    cut = tx_cut(p);
    if (cut >= start || n > sizeof tmp)
        return;
    memcpy(tmp, p->txbuf + start, n);
    memmove(p->txbuf + cut + n, p->txbuf + cut, start - cut);
    memcpy(p->txbuf + cut, tmp, n);
}

static void fsm_send(ppp_fsm_t *f, uint8_t code, uint8_t id,
                     const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    ppp_t *p = (ppp_t *)f->ctx;

    tx_cp(p, f == &p->lcp ? PPP_PROTO_LCP : f == &p->ipcp ? PPP_PROTO_IPCP : PPP_PROTO_CCP,
          code, id, a, an, b, bn);
}

/* ---------------------------------------------------------------- */
/* option judging shared by LCP and IPCP */

/* Verdict on one option of the peer's request. For PPP_CONF_NAK the value
 * we would accept is written to sugg (at most 8 octets) and *slen. */
typedef int (*judge_fn)(ppp_t *p, uint8_t type, const uint8_t *val, size_t vlen,
                        uint8_t *sugg, size_t *slen);

/* RFC 1661 5.3/5.4: if anything must be rejected, answer with a Reject
 * listing only those options; otherwise, if anything is unacceptable,
 * a Nak listing only those with acceptable values; otherwise Ack. Once
 * Max-Failure Naks have gone out, further Naks become Rejects (4.6). */
static int negotiate(ppp_t *p, ppp_fsm_t *f, judge_fn judge,
                     const uint8_t *opts, size_t len,
                     uint8_t *out, size_t max, size_t *outlen)
{
    int exhausted = f->failures >= f->max_failure;
    int worst = PPP_CONF_ACK, pass;
    size_t n = 0, fit = (size_t)(f->peer_mru ? f->peer_mru : PPP_MAX_MRU) - 4u;

    /* the reply (its 4-octet header and the options) within the peer's MRU,
     * as every other packet we send: whole options, as many as fit */
    if (max > fit)
        max = fit;

    for (pass = 0; pass < 2; pass++) {
        size_t pos = 0, vlen, slen;
        uint8_t type, sugg[8];
        const uint8_t *val;

        while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
            int v;

            slen = 0;
            v = judge(p, type, val, vlen, sugg, &slen);
            if (v == PPP_CONF_NAK && exhausted)
                v = PPP_CONF_REJ;
            if (pass == 0) {
                if (v == PPP_CONF_REJ)
                    worst = PPP_CONF_REJ;
                else if (v == PPP_CONF_NAK && worst == PPP_CONF_ACK)
                    worst = PPP_CONF_NAK;
            } else if (v == worst) {
                if (worst == PPP_CONF_REJ) {
                    if (n + 2 + vlen <= max) {
                        memcpy(out + n, val - 2, 2 + vlen);
                        n += 2 + vlen;
                    }
                } else if (n + 2 + slen <= max) {
                    out[n] = type;
                    out[n + 1] = (uint8_t)(2 + slen);
                    memcpy(out + n + 2, sugg, slen);
                    n += 2 + slen;
                }
            }
        }
        if (worst == PPP_CONF_ACK)
            break;
    }
    *outlen = n;
    return worst;
}

/* ---------------------------------------------------------------- */
/* LCP */

/* Windows 98 SE Dial-Up Networking's first LCP request, and only it: ACCM,
 * magic number, PFC, ACFC and callback by CBCP (13, operation 6), in that
 * order, nothing else. (Windows ME, 2000 and XP add options; pppd asks for
 * no callback.) */
static int is_win98_lcp(const uint8_t *o, size_t len)
{
    static const uint8_t want[5][2] = {
        { LCP_OPT_ACCM, 6 }, { LCP_OPT_MAGIC, 6 }, { LCP_OPT_PFC, 2 }, { LCP_OPT_ACFC, 2 }, { 13, 3 } };
    size_t pos = 0, vlen, i = 0;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(o, len, &pos, &type, &val, &vlen)) {
        if (i == 5 || type != want[i][0] || vlen + 2 != want[i][1])
            return 0;
        if (type == 13 && val[0] != 6)
            return 0;
        i++;
    }
    return i == 5 && pos == len;
}

/* our first LCP request goes out: the caller's came (or none in time) */
static void lcp_release(ppp_t *p)
{
    p->lcp_wait_ms = 0;
    ppp_fsm_up(&p->lcp);
}

static void lcp_reset(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    p->mru = p->cfg.mru;
    p->w_mru = (p->mru != PPP_MAX_MRU);
    p->accm = p->cfg.accm;
    p->w_accm = (p->accm != 0xffffffffu);
    p->w_magic = !p->cfg.no_magic;
    p->magic = p->w_magic ? new_magic(p, 0) : 0;
    p->w_pfc = !p->cfg.no_pfc;
    p->w_acfc = !p->cfg.no_acfc;
    /* MS-CHAPv2 first; MS-CHAP v1 first for Windows 98 (it logs in with v2,
     * then cannot make MPPE keys from it and hangs up: error 742), or where
     * v1 is the only one allowed; then CHAP-MD5, PAP */
    if ((p->cfg.auth & PPP_AUTH_MSCHAPV1) &&
        (p->cnt.win98 || !(p->cfg.auth & PPP_AUTH_MSCHAPV2)))
        p->w_auth = PPP_AUTH_MSCHAPV1;
    else if (p->cfg.auth & PPP_AUTH_MSCHAPV2)
        p->w_auth = PPP_AUTH_MSCHAPV2;
    else if (p->cfg.auth & PPP_AUTH_CHAP)
        p->w_auth = PPP_AUTH_CHAP;
    else if (p->cfg.auth & PPP_AUTH_PAP)
        p->w_auth = PPP_AUTH_PAP;
    else
        p->w_auth = PPP_AUTH_NONE;
    p->auth_tried = p->w_auth;
    p->loop_count = 0;
}

static size_t lcp_build_cr(ppp_fsm_t *f, uint8_t *o, size_t max)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t n = 0;

    (void)max;   /* at most 25 octets; the scratch area is far larger */
    if (p->w_mru) {
        o[n++] = LCP_OPT_MRU;
        o[n++] = 4;
        o[n++] = (uint8_t)(p->mru >> 8);
        o[n++] = (uint8_t)p->mru;
    }
    if (p->w_accm) {
        o[n++] = LCP_OPT_ACCM;
        o[n++] = 6;
        put32(o + n, p->accm);
        n += 4;
    }
    if (is_chap(p->w_auth)) {
        o[n++] = LCP_OPT_AUTH;
        o[n++] = 5;
        o[n++] = (uint8_t)(PPP_PROTO_CHAP >> 8);
        o[n++] = (uint8_t)PPP_PROTO_CHAP;
        o[n++] = chap_alg(p->w_auth);
    } else if (p->w_auth == PPP_AUTH_PAP) {
        o[n++] = LCP_OPT_AUTH;
        o[n++] = 4;
        o[n++] = (uint8_t)(PPP_PROTO_PAP >> 8);
        o[n++] = (uint8_t)PPP_PROTO_PAP;
    }
    if (p->w_magic) {
        o[n++] = LCP_OPT_MAGIC;
        o[n++] = 6;
        put32(o + n, p->magic);
        n += 4;
    }
    if (p->w_pfc) {
        o[n++] = LCP_OPT_PFC;
        o[n++] = 2;
    }
    if (p->w_acfc) {
        o[n++] = LCP_OPT_ACFC;
        o[n++] = 2;
    }
    return n;
}

static int lcp_judge(ppp_t *p, uint8_t type, const uint8_t *val, size_t vlen,
                     uint8_t *sugg, size_t *slen)
{
    switch (type) {
    case LCP_OPT_MRU: {
        unsigned mru = vlen == 2 ? ((unsigned)val[0] << 8) | val[1] : 0;
        if (mru >= PPP_MIN_MRU) {
            /* a larger MRU than we can ever fill is fine: we just never
             * send more than PPP_MAX_MRU */
            p->pend_mru = (uint16_t)(mru > PPP_MAX_MRU ? PPP_MAX_MRU : mru);
            return PPP_CONF_ACK;
        }
        sugg[0] = (uint8_t)(PPP_MAX_MRU >> 8);
        sugg[1] = (uint8_t)(PPP_MAX_MRU & 0xff);
        *slen = 2;
        return PPP_CONF_NAK;
    }
    case LCP_OPT_ACCM:
        if (vlen != 4)
            return PPP_CONF_REJ;
        p->pend_accm = get32(val);
        return PPP_CONF_ACK;
    case LCP_OPT_MAGIC: {
        uint32_t m = vlen == 4 ? get32(val) : 0;
        /* zero is not a magic number; our own number means we may be
         * talking to ourselves (RFC 1661 6.4): Nak with a fresh one */
        if (m != 0 && !(p->w_magic && m == p->magic)) {
            p->pend_magic = m;
            return PPP_CONF_ACK;
        }
        put32(sugg, new_magic(p, p->magic));
        *slen = 4;
        return PPP_CONF_NAK;
    }
    case LCP_OPT_PFC:
        if (vlen != 0 || p->cfg.no_pfc)
            return PPP_CONF_REJ;
        p->pend_pfc = 1;
        return PPP_CONF_ACK;
    case LCP_OPT_ACFC:
        if (vlen != 0 || p->cfg.no_acfc)
            return PPP_CONF_REJ;
        p->pend_acfc = 1;
        return PPP_CONF_ACK;
    default:
        /* includes Authentication-Protocol (we are the authenticator and
         * do not authenticate ourselves) and Quality-Protocol */
        return PPP_CONF_REJ;
    }
}

static int lcp_recv_cr(ppp_fsm_t *f, const uint8_t *opts, size_t len,
                       uint8_t *out, size_t max, size_t *outlen)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    /* defaults for whatever the peer does not mention */
    p->pend_mru = PPP_MAX_MRU;
    p->pend_accm = 0xffffffffu;
    p->pend_magic = 0;
    p->pend_pfc = p->pend_acfc = 0;

    /* Loopback detection (RFC 1661 6.4): a request carrying the magic
     * number of our own last request may be that request coming back. A
     * Nak with a different number MUST be sent; this takes precedence over
     * judging the other options. Several in a row and we call it a loop. */
    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        if (type != LCP_OPT_MAGIC || vlen != 4)
            continue;
        if (p->w_magic && get32(val) == p->magic) {
            if (++p->loop_count >= LOOP_LIMIT)
                request_close(p, PPP_END_LOOPBACK);
            if (max >= 6) {
                out[0] = LCP_OPT_MAGIC;
                out[1] = 6;
                put32(out + 2, new_magic(p, p->magic));
                *outlen = 6;
                return PPP_CONF_NAK;
            }
        } else {
            p->loop_count = 0;
        }
    }
    return negotiate(p, f, lcp_judge, opts, len, out, max, outlen);
}

static void lcp_recv_ack(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    /* the automaton already checked the Ack equals our request; the
     * requested values take effect in lcp_up() */
    (void)f; (void)opts; (void)len;
}

/* The peer will not do the method we asked for. Pick another one we are
 * allowed to use and have not tried, or give up: authentication is not
 * optional on a dial-in server. */
static void lcp_auth_refused(ppp_t *p, unsigned suggestion)
{
    unsigned left = p->cfg.auth & ~(unsigned)p->auth_tried;
    unsigned next = 0;

    if (p->cfg.encrypt) {
        /* MPPE is keyed from MS-CHAPv2 only. Falling back to another login
         * would have the caller send its password (in clear, for PAP) for a
         * link that is then ended for want of keys: end it now. */
        request_close(p, PPP_END_ENCRYPT_REQUIRED);
        return;
    }
    /* the peer's suggestion if we allow it, else the strongest left: never
     * PAP (a password in clear) while CHAP-MD5 is still to be tried */
    if (suggestion & left)
        next = suggestion;
    else if (left & PPP_AUTH_MSCHAPV1)
        next = PPP_AUTH_MSCHAPV1;
    else if (left & PPP_AUTH_MSCHAPV2)
        next = PPP_AUTH_MSCHAPV2;
    else if (left & PPP_AUTH_CHAP)
        next = PPP_AUTH_CHAP;
    else if (left & PPP_AUTH_PAP)
        next = PPP_AUTH_PAP;
    if (!next) {
        request_close(p, PPP_END_AUTH_REFUSED);
        return;
    }
    p->w_auth = (uint8_t)next;
    p->auth_tried |= (uint8_t)next;
}

static void lcp_recv_nak(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        switch (type) {
        case LCP_OPT_MRU:
            if (p->w_mru && vlen == 2) {
                unsigned mru = ((unsigned)val[0] << 8) | val[1];
                if (mru >= PPP_MIN_MRU && mru <= PPP_MAX_MRU) {
                    p->mru = (uint16_t)mru;
                    p->w_mru = (mru != PPP_MAX_MRU);
                }
            }
            break;
        case LCP_OPT_ACCM:
            /* the peer needs more characters escaped: take the union */
            if (p->w_accm && vlen == 4)
                p->accm |= get32(val);
            break;
        case LCP_OPT_AUTH:
            if (p->w_auth) {
                unsigned sug = 0;
                if (vlen == 2 && val[0] == 0xc0 && val[1] == 0x23)
                    sug = PPP_AUTH_PAP;
                else if (vlen == 3 && val[0] == 0xc2 && val[1] == 0x23 &&
                         val[2] == CHAP_ALG_MD5)
                    sug = PPP_AUTH_CHAP;
                else if (vlen == 3 && val[0] == 0xc2 && val[1] == 0x23 &&
                         val[2] == CHAP_ALG_MSCHAP2)
                    sug = PPP_AUTH_MSCHAPV2;
                else if (vlen == 3 && val[0] == 0xc2 && val[1] == 0x23 &&
                         val[2] == CHAP_ALG_MSCHAP1)
                    sug = PPP_AUTH_MSCHAPV1;
                /* anything else (EAP, say) is no suggestion we can take */
                lcp_auth_refused(p, sug);
            }
            break;
        case LCP_OPT_MAGIC:
            if (p->w_magic)
                p->magic = new_magic(p, p->magic);
            break;
        default:
            /* options we did not request: the peer would like us to ask
             * for them; we do not have to */
            break;
        }
    }
}

static void lcp_recv_rej(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        switch (type) {
        case LCP_OPT_MRU:   p->w_mru = 0; p->mru = PPP_MAX_MRU; break;
        case LCP_OPT_ACCM:  p->w_accm = 0; break;
        case LCP_OPT_MAGIC: p->w_magic = 0; p->magic = 0; break;
        case LCP_OPT_PFC:   p->w_pfc = 0; break;
        case LCP_OPT_ACFC:  p->w_acfc = 0; break;
        case LCP_OPT_AUTH:
            /* the peer will not do this method: some old stacks Reject a
             * method they do not know (MS-CHAPv2) where they should Nak.
             * The next one allowed, if any is left; authentication stays
             * mandatory, so with none left the link ends. */
            if (p->w_auth)
                lcp_auth_refused(p, 0);
            break;
        default:
            break;
        }
    }
}

static void auth_start(ppp_t *p);
static void network_start(ppp_t *p);
static void network_stop(ppp_t *p);
static void ccp_start(ppp_t *p);

static void lcp_up(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    /* what the peer asked for governs what we send ... */
    p->peer_mru = p->pend_mru;
    p->tx_accm = p->pend_accm;
    p->peer_magic = p->pend_magic;
    p->tx_pfc = p->pend_pfc;
    p->tx_acfc = p->pend_acfc;
    p->lcp.peer_mru = p->peer_mru;
    p->ipcp.peer_mru = p->peer_mru;
    p->ccp.peer_mru = p->peer_mru;
    /* ... and what we asked for governs what we accept */
    p->rx.rx_accm = p->w_accm ? p->accm : 0xffffffffu;
    p->rx_pfc = p->w_pfc;
    p->rx_acfc = p->w_acfc;

    p->echo_timer = 0;
    p->echo_pending = 0;
    p->auth_proto = p->w_auth;
    p->auth_done = p->auth_ok = 0;
    if (p->auth_proto) {
        set_phase(p, PPP_PHASE_AUTHENTICATE);
        auth_start(p);
    } else {
        network_start(p);
    }
}

static void lcp_down(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    network_stop(p);
    p->auth_proto = PPP_AUTH_NONE;
    p->auth_done = p->auth_ok = 0;
    p->mppe_have = 0;
    p->mppe_started = 0;
    p->enc_wait_ms = 0;
    /* the keys of this login do not outlive it */
    ppp_wipe(p->mppe_send, sizeof p->mppe_send);
    ppp_wipe(p->mppe_recv, sizeof p->mppe_recv);
    ppp_wipe(p->mppe_short_send, sizeof p->mppe_short_send);
    ppp_wipe(p->mppe_short_recv, sizeof p->mppe_short_recv);
    ppp_wipe(p->mschap2_resp, sizeof p->mschap2_resp);
    ppp_mppe_reset(&p->mppe);
    /* back to the default framing */
    p->peer_mru = PPP_MAX_MRU;
    p->tx_accm = 0xffffffffu;
    p->peer_magic = 0;
    p->tx_pfc = p->tx_acfc = p->rx_pfc = p->rx_acfc = 0;
    p->rx.rx_accm = 0xffffffffu;
    p->lcp.peer_mru = 0;

    if (f->state == PPP_FSM_CLOSING || f->state == PPP_FSM_STOPPING)
        set_phase(p, PPP_PHASE_TERMINATE);
    else if (f->state != PPP_FSM_STARTING)
        set_phase(p, PPP_PHASE_ESTABLISH);   /* renegotiation */
}

static void lcp_finished(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    set_end(p, PPP_END_NEGOTIATION);
    set_phase(p, PPP_PHASE_DEAD);
}

static int lcp_ext_code(ppp_fsm_t *f, uint8_t code, uint8_t id,
                        const uint8_t *data, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    uint32_t ours = p->w_magic ? p->magic : 0;

    switch (code) {
    case LCP_PROTO_REJ: {
        unsigned proto;
        /* RFC 1661 5.7: only meaningful in the Opened state */
        if (f->state != PPP_FSM_OPENED || len < 2)
            return PPP_FSM_EXT_NONE;
        proto = ((unsigned)data[0] << 8) | data[1];
        if (proto == PPP_PROTO_LCP)
            return PPP_FSM_EXT_RXJ_BAD;
        if (proto == PPP_PROTO_VJC || proto == PPP_PROTO_VJUC) {
            /* it will not take VJ after all: plain IP from now on */
            ppp_vj_tx_init(&p->vj, 0, 0);
            p->cnt.vj_tx_slots = 0;
            return PPP_FSM_EXT_NONE;
        }
        if (proto == PPP_PROTO_IPCP || proto == PPP_PROTO_IP) {
            /* without IP this link has no purpose */
            if (p->phase == PPP_PHASE_NETWORK)
                ppp_fsm_protocol_rejected(&p->ipcp);
            request_close(p, PPP_END_IPCP_FAILED);
        } else if (proto == PPP_PROTO_CCP) {
            /* the peer has no CCP: the link goes on uncompressed -- unless
             * we require encryption, which only CCP can bring */
            ppp_fsm_protocol_rejected(&p->ccp);
            if (p->cfg.encrypt)
                request_close(p, PPP_END_ENCRYPT_REQUIRED);
        } else if (proto == PPP_PROTO_COMP) {
            /* it will not take compressed datagrams after all */
            ppp_fsm_close(&p->ccp);
        } else if ((proto == PPP_PROTO_PAP && p->auth_proto == PPP_AUTH_PAP) ||
                   (proto == PPP_PROTO_CHAP && is_chap(p->auth_proto))) {
            if (!p->auth_ok)
                request_close(p, PPP_END_AUTH_REFUSED);
        }
        return PPP_FSM_EXT_RXJ_GOOD;
    }
    case LCP_ECHO_REQ:
        if (f->state == PPP_FSM_OPENED && len >= 4) {
            uint8_t m[4];
            size_t room = (size_t)p->peer_mru - 8;
            size_t dn = len - 4;

            /* our own number in a request: the line is looped; no reply */
            if (ours != 0 && get32(data) == ours)
                return PPP_FSM_EXT_RXR;
            put32(m, ours);
            if (dn > room)
                dn = room;
            f->ops->send(f, LCP_ECHO_REP, id, m, 4, data + 4, dn);
        }
        return PPP_FSM_EXT_RXR;
    case LCP_ECHO_REP:
        if (f->state == PPP_FSM_OPENED && len >= 4) {
            /* a reply with our own magic number is our request coming
             * back, not a live peer */
            if (ours == 0 || get32(data) != ours)
                p->echo_pending = 0;
        }
        return PPP_FSM_EXT_RXR;
    case LCP_DISCARD_REQ:
        return PPP_FSM_EXT_RXR;
    default:
        return PPP_FSM_EXT_RUC;
    }
}

static const ppp_fsm_ops_t lcp_ops = {
    fsm_send, lcp_reset, lcp_build_cr, lcp_recv_cr,
    lcp_recv_ack, lcp_recv_nak, lcp_recv_rej,
    lcp_up, lcp_down, NULL, lcp_finished, lcp_ext_code
};

static void keepalive_tick(ppp_t *p, uint32_t ms)
{
    uint8_t m[4];

    if (!p->cfg.echo_interval_ms || p->lcp.state != PPP_FSM_OPENED)
        return;
    /* a peer that Code-Rejected Echo-Request cannot be probed */
    if (p->lcp.rejected_codes & (1u << LCP_ECHO_REQ))
        return;
    p->echo_timer += ms;
    if (p->echo_timer < p->cfg.echo_interval_ms)
        return;
    p->echo_timer = 0;
    if (p->echo_pending >= p->cfg.echo_max_fail) {
        request_close(p, PPP_END_KEEPALIVE);
        return;
    }
    p->echo_pending++;
    put32(m, p->w_magic ? p->magic : 0);
    tx_cp(p, PPP_PROTO_LCP, LCP_ECHO_REQ, ppp_fsm_new_id(&p->lcp), m, 4, NULL, 0);
}

/* ---------------------------------------------------------------- */
/* authentication (we are the authenticator) */

static const char msg_ok[] = "Welcome";
static const char msg_bad[] = "Authentication failed";

static int lookup_secret(ppp_t *p, const uint8_t *user, size_t ulen,
                         uint8_t *secret, size_t max)
{
    size_t i;

    if (p->cfg.secret_cb) {
        int n = p->cfg.secret_cb(p->cfg.ctx, user, ulen, secret, max);
        return (n < 0 || (size_t)n > max) ? -1 : n;
    }
    for (i = 0; i < p->cfg.n_users; i++) {
        const ppp_user_t *u = &p->cfg.users[i];
        size_t sl;

        if (strlen(u->name) != ulen || memcmp(u->name, user, ulen) != 0)
            continue;
        sl = strlen(u->secret);
        if (sl > max)
            return -1;
        memcpy(secret, u->secret, sl);
        return (int)sl;
    }
    return -1;
}

/* compare without an early exit, so timing does not tell how many leading
 * octets matched */
static int same_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;

    while (n--)
        diff |= (uint8_t)(*a++ ^ *b++);
    return diff == 0;
}

static void chap_send_challenge(ppp_t *p)
{
    uint8_t v[17];
    const char *name = p->cfg.hostname;

    /* MS-CHAP v1's challenge is 8 octets (RFC 2433 section 5), the others' 16 */
    size_t cl = p->auth_proto == PPP_AUTH_MSCHAPV1 ? 8 : 16;

    v[0] = (uint8_t)cl;
    memcpy(v + 1, p->challenge, cl);
    tx_cp(p, PPP_PROTO_CHAP, CHAP_CHALLENGE, p->auth_id, v, 1 + cl,
          (const uint8_t *)name, strlen(name));
}

static void auth_start(ppp_t *p)
{
    p->auth_timer = p->lcp.restart_ms;
    p->auth_tries = 1;
    if (is_chap(p->auth_proto)) {
        /* RFC 1994 4.1: a new identifier and an unpredictable value for
         * every new challenge (the 16-octet MS-CHAPv2 authenticator
         * challenge is sent the same way) */
        p->auth_id++;
        nonce(p, p->challenge, 16);
        chap_send_challenge(p);
    }
    /* PAP: the peer speaks first (RFC 1334 2.2); we only wait */
}

static void auth_tick(ppp_t *p, uint32_t ms)
{
    if (p->phase != PPP_PHASE_AUTHENTICATE || p->auth_done)
        return;
    while (ms >= p->auth_timer) {
        ms -= p->auth_timer;
        p->auth_timer = p->lcp.restart_ms;
        if (p->auth_tries >= p->lcp.max_configure) {
            request_close(p, PPP_END_AUTH_TIMEOUT);
            return;
        }
        p->auth_tries++;
        if (is_chap(p->auth_proto))
            chap_send_challenge(p);   /* retransmission: same id and value */
    }
    p->auth_timer -= ms;
}

static void auth_reply(ppp_t *p, uint8_t id)
{
    const char *msg = p->auth_ok ? msg_ok : msg_bad;
    size_t ml = strlen(msg);

    if (p->auth_proto == PPP_AUTH_MSCHAPV2) {
        /* RFC 2759 section 5: Success carries "S=<auth response> M=<text>",
         * Failure "E=<error> R=<retry> V=<version>". We do not offer a retry. */
        if (p->auth_ok) {
            uint8_t m[42 + 10];
            memcpy(m, p->mschap2_resp, 42);
            memcpy(m + 42, " M=Welcome", 10);
            tx_cp(p, PPP_PROTO_CHAP, CHAP_SUCCESS, id, m, sizeof m, NULL, 0);
        } else {
            /* RFC 2759 section 6: "E=691 R=0 C=<32 hex> V=3 M=<text>". C is
             * the challenge for a retry; we allow none (R=0) but send a fresh
             * one, as the format has it, for clients that parse it. */
            static const char hx[] = "0123456789ABCDEF";
            char e[] = "E=691 R=0 C=00000000000000000000000000000000 V=3 M=Authentication failed";
            uint8_t c[16];
            int i;
            nonce(p, c, sizeof c);
            for (i = 0; i < 16; i++) {
                e[12 + 2 * i] = hx[c[i] >> 4];
                e[13 + 2 * i] = hx[c[i] & 15];
            }
            tx_cp(p, PPP_PROTO_CHAP, CHAP_FAILURE, id, (const uint8_t *)e, sizeof e - 1, NULL, 0);
        }
    } else if (p->auth_proto == PPP_AUTH_MSCHAPV1) {
        /* RFC 2433 section 5: Success carries any text; Failure
         * "E=<error> R=<retry>" (no retry offered) */
        static const char e[] = "E=691 R=0";
        if (p->auth_ok)
            tx_cp(p, PPP_PROTO_CHAP, CHAP_SUCCESS, id, (const uint8_t *)msg, ml, NULL, 0);
        else
            tx_cp(p, PPP_PROTO_CHAP, CHAP_FAILURE, id, (const uint8_t *)e, sizeof e - 1, NULL, 0);
    } else if (p->auth_proto == PPP_AUTH_CHAP) {
        tx_cp(p, PPP_PROTO_CHAP, p->auth_ok ? CHAP_SUCCESS : CHAP_FAILURE, id,
              (const uint8_t *)msg, ml, NULL, 0);
    } else {
        uint8_t l = (uint8_t)ml;
        tx_cp(p, PPP_PROTO_PAP, p->auth_ok ? PAP_ACK : PAP_NAK, id,
              &l, 1, (const uint8_t *)msg, ml);
    }
}

static void auth_finish(ppp_t *p, uint8_t id, int ok,
                        const uint8_t *user, size_t ulen)
{
    size_t i;

    p->auth_done = 1;
    p->auth_ok = (uint8_t)(ok != 0);
    if (ok)
        p->cnt.auth_why = PPP_AUTH_WHY_NONE;
    auth_reply(p, id);
    /* the name it gave, for the log: on a failure only one that is a user
     * of ours (a name that is not may be a password in the wrong field) */
    if (!ok && p->cnt.auth_why == PPP_AUTH_WHY_USER)
        ulen = 0;
    if (ulen > PPP_NAME_MAX)
        ulen = PPP_NAME_MAX;
    for (i = 0; i < ulen; i++)       /* keep the name printable for logs */
        p->peer_name[i] = (user[i] >= 0x20 && user[i] < 0x7f) ? (char)user[i] : '?';
    p->peer_name[ulen] = 0;
    if (!ok) {
        /* RFC 1334 2.2.1 / RFC 1994 4.2: after a failure, end the link */
        request_close(p, PPP_END_AUTH_FAILED);
        return;
    }
    network_start(p);
}

/* validate the common header; returns the data length or -1 */
static int cp_header(const uint8_t *pkt, size_t n)
{
    size_t len;

    if (n < 4)
        return -1;
    len = ((size_t)pkt[2] << 8) | pkt[3];
    if (len < 4 || len > n)
        return -1;
    return (int)(len - 4);
}

static void pap_input(ppp_t *p, const uint8_t *pkt, size_t n)
{
    int dl = cp_header(pkt, n);
    const uint8_t *d = pkt + 4, *user, *pass;
    size_t ul, pl;
    int ok;

    if (dl < 0 || pkt[0] != PAP_REQUEST)
        return;
    if (p->auth_done) {
        /* our answer was probably lost: repeat it (RFC 1334 2.2.1) */
        auth_reply(p, pkt[1]);
        return;
    }
    if (dl < 1)
        return;
    ul = d[0];
    if ((size_t)dl < 1 + ul + 1)
        return;
    user = d + 1;
    pl = d[1 + ul];
    if ((size_t)dl < 2 + ul + pl)
        return;
    pass = d + 2 + ul;

    if (p->cfg.pap_cb) {
        ok = p->cfg.pap_cb(p->cfg.ctx, user, ul, pass, pl) != 0;
    } else {
        uint8_t secret[PPP_SECRET_MAX];
        int sl = lookup_secret(p, user, ul, secret, sizeof secret);
        ok = sl >= 0 && (size_t)sl == pl && same_bytes(secret, pass, pl);
        p->cnt.auth_why = (uint8_t)(sl < 0 ? PPP_AUTH_WHY_USER : PPP_AUTH_WHY_SECRET);
        memset(secret, 0, sizeof secret);
    }
    auth_finish(p, pkt[1], ok, user, ul);
}

static void chap_input(ppp_t *p, const uint8_t *pkt, size_t n)
{
    int dl = cp_header(pkt, n);
    const uint8_t *d = pkt + 4, *user;
    uint8_t secret[PPP_SECRET_MAX], want[24];
    size_t vs, ul;
    int sl, ok = 0;

    if (dl < 0 || pkt[0] != CHAP_RESPONSE)
        return;
    /* RFC 1994 4.1: the Response identifier must be the Challenge's */
    if (pkt[1] != p->auth_id)
        return;
    if (p->auth_done) {
        /* same answer for a repeated Response */
        auth_reply(p, pkt[1]);
        return;
    }
    if (dl < 1)
        return;
    vs = d[0];
    if ((size_t)dl < 1 + vs)
        return;
    user = d + 1 + vs;
    ul = (size_t)dl - 1 - vs;

    sl = lookup_secret(p, user, ul, secret, sizeof secret);
    if (p->auth_proto == PPP_AUTH_MSCHAPV2) {
        /* RFC 2759 8.2: the challenge hash takes the user name without a
         * domain in front ("WORKGROUP\alice": alice), which is what Windows
         * sends when told to include its logon domain; the user is looked up
         * by that name too when the whole one is not a user of ours. */
        size_t k = ul;
        while (k > 0 && user[k - 1] != '\\') k--;
        if (k > 0) {
            if (sl < 0) sl = lookup_secret(p, user + k, ul - k, secret, sizeof secret);
            user += k; ul -= k;
        }
        /* RFC 2759: the 49-octet value is PeerChallenge[16], Reserved[8],
         * NT-Response[24], Flags[1]. Our challenge is the authenticator
         * challenge. Match the NT-Response, then build the authenticator
         * response and derive the MPPE master keys. */
        if (vs == 49) {
            const uint8_t *peer = d + 1, *ntr = d + 1 + 24;
            /* an unknown user is hashed too (with an empty secret), so the
             * answer takes as long whether the name exists or not */
            ppp_mschap2_nt_response(p->challenge, peer, user, ul, secret, sl >= 0 ? (size_t)sl : 0, want);
            ok = sl >= 0 && same_bytes(want, ntr, 24);
            if (ok) {
                ppp_mschap2_auth_response(secret, (size_t)sl, ntr, peer, p->challenge,
                                          user, ul, p->mschap2_resp);
                ppp_mppe_master_keys(secret, (size_t)sl, ntr, 16, p->mppe_send, p->mppe_recv);
                /* RFC 3079 3.3: the short keys are the first 8 octets */
                memcpy(p->mppe_short_send, p->mppe_send, 8);
                memcpy(p->mppe_short_recv, p->mppe_recv, 8);
                p->mppe_have = 1;
            }
        }
    } else if (p->auth_proto == PPP_AUTH_MSCHAPV1) {
        /* RFC 2433 section 5: the 49-octet value is LM-Response[24],
         * NT-Response[24], Flags[1]. Only the NT response (flag 1) is
         * taken: the LM one alone (flag 0) proves only the password
         * upper-cased and cut to 14 octets. The keys (RFC 3079 section 2)
         * are the same both ways: 16 octets from the NT hash's hash and the
         * challenge, 8 for the short ones from the LAN Manager hash (which
         * has no challenge in it: Microsoft's design, and all Windows 98
         * does for 40/56-bit MPPE). */
        size_t k = ul;
        while (k > 0 && user[k - 1] != '\\') k--;
        if (k > 0) {
            if (sl < 0) sl = lookup_secret(p, user + k, ul - k, secret, sizeof secret);
            user += k; ul -= k;
        }
        if (vs == 49) {
            const uint8_t *ntr = d + 1 + 24;
            int use_nt = d[1 + 48] & 1;
            uint8_t h[16];
            int nt_ok, lm_ok;
            ppp_nt_password_hash(secret, sl >= 0 ? (size_t)sl : 0, h);
            ppp_mschap1_response(p->challenge, h, want);
            nt_ok = same_bytes(want, ntr, 24);
            ok = sl >= 0 && use_nt && nt_ok;
            if (!ok && sl >= 0) {
                /* why, for the log: the LAN Manager answer, which ignores
                 * letter case, tells a wrong password from one in the
                 * wrong case, and a refused LM-only answer from a wrong one */
                ppp_lm_password_hash(secret, (size_t)sl, h);
                ppp_mschap1_response(p->challenge, h, want);
                lm_ok = same_bytes(want, d + 1, 24);
                p->cnt.auth_why = (uint8_t)(!lm_ok ? PPP_AUTH_WHY_SECRET
                                            : use_nt ? PPP_AUTH_WHY_CASE : PPP_AUTH_WHY_LM_ONLY);
            }
            ppp_wipe(h, sizeof h);
            if (ok) {
                ppp_mschap1_mppe_start(secret, (size_t)sl, p->challenge, 16, p->mppe_send);
                memcpy(p->mppe_recv, p->mppe_send, 16);
                ppp_mschap1_mppe_start(secret, (size_t)sl, p->challenge, 8, h);
                memcpy(p->mppe_short_send, h, 8);
                memcpy(p->mppe_short_recv, h, 8);
                ppp_wipe(h, sizeof h);
                p->mppe_have = 1;
            }
        }
    } else if (vs == 16) {
        /* RFC 1994 4.1: MD5 over identifier, secret, challenge value (an
         * unknown user with an empty secret, as above) */
        ppp_md5_t m;
        ppp_md5_init(&m);
        ppp_md5_update(&m, &p->auth_id, 1);
        ppp_md5_update(&m, secret, sl >= 0 ? (size_t)sl : 0);
        ppp_md5_update(&m, p->challenge, 16);
        ppp_md5_final(&m, want);
        ok = sl >= 0 && same_bytes(want, d + 1, 16);
    }
    if (!ok && !p->cnt.auth_why)
        p->cnt.auth_why = (uint8_t)(sl < 0 ? PPP_AUTH_WHY_USER
                                    : (p->auth_proto == PPP_AUTH_CHAP ? vs != 16 : vs != 49)
                                    ? PPP_AUTH_WHY_FORMAT : PPP_AUTH_WHY_SECRET);
    ppp_wipe(secret, sizeof secret);
    ppp_wipe(want, sizeof want);
    auth_finish(p, pkt[1], ok, user, ul);
}

const char *ppp_auth_why_name(unsigned why)
{
    static const char *const n[] = {
        "", "no such user", "wrong password", "the password in another letter case",
        "a LAN Manager answer alone (refused; the password was right)", "a malformed answer" };
    return why < sizeof n / sizeof n[0] ? n[why] : "?";
}

/* ---------------------------------------------------------------- */
/* IPCP */

static void ipcp_reset(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    p->our_ip = p->cfg.our_ip;
    p->w_addr = (p->our_ip != 0);
    p->addr_naks = 0;
    p->w_vj = (uint8_t)!p->cfg.no_vj;
}

static size_t ipcp_build_cr(ppp_fsm_t *f, uint8_t *o, size_t max)
{
    ppp_t *p = (ppp_t *)f->ctx;

    size_t n = 0;

    (void)max;
    if (p->w_vj) {
        /* RFC 1332 3.2: VJ, Max-Slot-Id (slots - 1), Comp-Slot-Id (the
         * slot number may be left out of a compressed packet) */
        o[n++] = IPCP_OPT_COMP; o[n++] = 6;
        o[n++] = (uint8_t)(PPP_PROTO_VJC >> 8); o[n++] = (uint8_t)PPP_PROTO_VJC;
        o[n++] = PPP_VJ_SLOTS - 1; o[n++] = 1;
    }
    if (p->w_addr) {
        o[n++] = IPCP_OPT_ADDR;
        o[n++] = 6;
        put32(o + n, p->our_ip);
        n += 4;
    }
    return n;
}

static int ipcp_judge(ppp_t *p, uint8_t type, const uint8_t *val, size_t vlen,
                      uint8_t *sugg, size_t *slen)
{
    uint32_t v = vlen == 4 ? get32(val) : 0;
    uint32_t dns;

    switch (type) {
    case IPCP_OPT_ADDR:
        if (p->cfg.peer_ip == 0) {
            /* no address to hand out: take what the peer brings */
            if (v == 0)
                return PPP_CONF_REJ;
            p->pend_peer_ip = v;
            return PPP_CONF_ACK;
        }
        if (v == p->cfg.peer_ip) {
            p->pend_peer_ip = v;
            return PPP_CONF_ACK;
        }
        put32(sugg, p->cfg.peer_ip);
        *slen = 4;
        return PPP_CONF_NAK;
    case IPCP_OPT_DNS1:
    case IPCP_OPT_DNS2:
        dns = type == IPCP_OPT_DNS1 ? p->cfg.dns1 : p->cfg.dns2;
        if (dns == 0)
            return PPP_CONF_REJ;
        if (v == dns)
            return PPP_CONF_ACK;
        put32(sugg, dns);
        *slen = 4;
        return PPP_CONF_NAK;
    case IPCP_OPT_COMP:
        /* Van Jacobson (0x002d) in RFC 1332's form (Max-Slot-Id,
         * Comp-Slot-Id) or RFC 1172's older one without them (16 slots,
         * the slot number may be left out): how we compress towards the
         * peer. Any number of slots is fine: we use no more than 16 of
         * them. Another protocol (RFC 2507's IPHC, say) is not done here. */
        if (p->cfg.no_vj || vlen < 2 || get16(val) != PPP_PROTO_VJC || (vlen != 2 && vlen != 4))
            return PPP_CONF_REJ;
        p->pend_vj_slots = (uint8_t)(vlen == 4 ? (val[2] >= PPP_VJ_SLOTS ? PPP_VJ_SLOTS : val[2] + 1) : PPP_VJ_SLOTS);
        p->pend_vj_cid = (uint8_t)(vlen == 4 ? val[3] != 0 : 1);
        return PPP_CONF_ACK;
    default:
        /* the deprecated IP-Addresses option and NBNS are not supported */
        return PPP_CONF_REJ;
    }
}

static int ipcp_recv_cr(ppp_fsm_t *f, const uint8_t *opts, size_t len,
                        uint8_t *out, size_t max, size_t *outlen)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;
    int has_addr = 0, r;

    p->pend_peer_ip = 0;
    p->pend_vj_slots = 0;
    p->pend_vj_cid = 0;
    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen))
        if (type == IPCP_OPT_ADDR)
            has_addr = 1;
    r = negotiate(p, f, ipcp_judge, opts, len, out, max, outlen);
    if (r == PPP_CONF_ACK && !has_addr && p->cfg.peer_ip != 0) {
        /* RFC 1332 3.3: the peer did not ask for an address; a Nak may
         * add the option to tell it which one to use */
        if (f->failures < f->max_failure && max >= 6) {
            out[0] = IPCP_OPT_ADDR;
            out[1] = 6;
            put32(out + 2, p->cfg.peer_ip);
            *outlen = 6;
            return PPP_CONF_NAK;
        }
        p->pend_peer_ip = p->cfg.peer_ip;   /* it will not; assume ours */
    }
    return r;
}

static void ipcp_recv_ack(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    (void)f; (void)opts; (void)len;
}

static void ipcp_recv_nak(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        if (type == IPCP_OPT_COMP) {
            /* a Nak toward another compression protocol: not one we do */
            if (vlen < 2 || get16(val) != PPP_PROTO_VJC)
                p->w_vj = 0;
            continue;
        }
        if (type != IPCP_OPT_ADDR || vlen != 4)
            continue;
        if (p->cfg.our_ip == 0) {
            /* we were not given an address: let the peer name ours */
            p->our_ip = get32(val);
            p->w_addr = (p->our_ip != 0);
        } else if (++p->addr_naks >= f->max_failure) {
            /* our address is configured and not up for discussion */
            request_close(p, PPP_END_IPCP_FAILED);
        }
    }
}

static void ipcp_recv_rej(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        if (type == IPCP_OPT_ADDR)
            p->w_addr = 0;
        if (type == IPCP_OPT_COMP)
            p->w_vj = 0;
    }
}

static void ipcp_up(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    p->peer_ip = p->pend_peer_ip;
    /* header compression as each request was Acked: towards the peer as it
     * asked, from it as we did; every slot empty */
    ppp_vj_tx_init(&p->vj, p->pend_vj_slots, p->pend_vj_cid);
    ppp_vj_rx_init(&p->vj, p->w_vj ? PPP_VJ_SLOTS : 0);
    p->cnt.vj_tx_slots = p->pend_vj_slots;
    p->cnt.vj_rx_slots = (uint8_t)(p->w_vj ? PPP_VJ_SLOTS : 0);
}

static void ipcp_finished(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    /* IPCP gave up or was closed by the peer. IP is the only network
     * protocol here, so the link has nothing left to carry. */
    if (!p->ipcp_closing && p->phase == PPP_PHASE_NETWORK)
        request_close(p, PPP_END_IPCP_FAILED);
}

static const ppp_fsm_ops_t ipcp_ops = {
    fsm_send, ipcp_reset, ipcp_build_cr, ipcp_recv_cr,
    ipcp_recv_ack, ipcp_recv_nak, ipcp_recv_rej,
    ipcp_up, NULL, NULL, ipcp_finished, NULL
};

/* ---------------------------------------------------------------- */
/* CCP (RFC 1962)
 *
 * An option in a Configure-Request names an algorithm the sender of the
 * request is willing to decompress (RFC 1962 section 4). So our request
 * is about the direction we receive and the peer's request about the
 * direction we transmit; the two are settled independently and may come
 * out different, or one of them empty.
 *
 * Our request carries one algorithm at a time: PPP Deflate, then the same
 * under its draft number, then MPPC, each dropped when the peer rejects
 * it; in the end an empty request (nothing compressed towards us). That
 * way an Ack never leaves open which algorithm the peer will use.
 *
 * Of the peer's request we take one option: Deflate before Deflate under
 * the draft number before MPPC. Everything else in it is rejected, so the
 * peer comes back with that one alone and gets its Ack. */

static unsigned comp_of(unsigned opt)
{
    return opt == CCP_OPT_MPPC ? PPP_COMP_MPPC : opt ? PPP_COMP_DEFLATE : PPP_COMP_NONE;
}

/* MPPE key lengths, strongest first: the strength of a key bit, and the
 * strongest one in a set of them */
static int mppe_rank(uint32_t bit)
{
    return bit == MPPE_BIT_S ? 3 : bit == MPPE_BIT_M ? 2 : bit == MPPE_BIT_L ? 1 : 0;
}

static uint32_t mppe_best(uint32_t v)
{
    return v & MPPE_BIT_S ? MPPE_BIT_S : v & MPPE_BIT_M ? MPPE_BIT_M : v & MPPE_BIT_L ? MPPE_BIT_L : 0;
}

/* The peer has shown which key lengths it has. Its own Configure-Request
 * says so with authority: the length becomes the strongest it offers (and
 * that is the floor from then on). A Nak (one frame anyone on the line
 * could inject) may only bring the length down to a shorter one the peer
 * offers, never below the strongest of its own request: a peer that asked
 * for 128-bit is not talked down to 40. */
static void mppe_settle(ppp_t *p, uint32_t v, int request)
{
    uint32_t best = mppe_best(v & MPPE_KEYS);
    if (!best)
        return;
    if (request) {
        /* the floor only ever rises: a later request that claims less
         * than an earlier one offered (one injected, say) changes nothing */
        if (mppe_rank(best) >= mppe_rank(p->mppe_floor)) {
            p->mppe_key = best;
            p->mppe_floor = best;
        }
        return;
    }
    if (!(v & p->mppe_key) && mppe_rank(best) < mppe_rank(p->mppe_key) &&
        mppe_rank(best) >= mppe_rank(p->mppe_floor))
        p->mppe_key = best;
}

/* whether CCP is spoken at all: for compression, or for MPPE once the keys
 * are in hand (MPPE is negotiated inside CCP) */
static int ccp_wanted(const ppp_t *p)
{
    return p->cfg.comp != 0 || p->mppe_have;
}

/* On request: does this option-18 value ask for encryption? It names an
 * MPPE key length, and the keys are in hand. MPPC asked as well does not
 * change that: Windows XP with "Require data encryption" and software
 * compression asks 0xe1, Windows 98 0x21, and both hang up (error 742) when
 * Nak'd toward MPPC alone; Windows' optional encryption asks for MPPC only.
 * We do not do both at once, so such a caller gets MPPE. */
static int mppe_asks(const ppp_t *p, uint32_t v)
{
    return (v & MPPE_KEYS) && p->mppe_have;
}

/* MPPE is what our request carries and what we take: encryption required,
 * or (on request, the default) the caller has asked for it; with the keys
 * of an MS-CHAPv2 login either way */
static int mppe_want(const ppp_t *p)
{
    return p->mppe_have && (p->cfg.encrypt || p->mppe_asked);
}

/* nothing in the clear: encryption required, a caller that has asked for
 * it, or MPPE that has run (a CCP gone down must not leave the clear) */
static int mppe_hold(const ppp_t *p)
{
    return p->cfg.encrypt || (p->mppe_asked && p->mppe_have) || p->mppe_started;
}

static unsigned cw_bit(unsigned opt)
{
    return opt == CCP_OPT_DEFLATE ? 1u : opt == CCP_OPT_DEFLATE_DRAFT ? 2u : 4u;
}

/* the next algorithm to ask the peer for, 0 when none is left */
static void ccp_pick(ppp_t *p)
{
    static const uint8_t order[3] = { CCP_OPT_DEFLATE, CCP_OPT_DEFLATE_DRAFT, CCP_OPT_MPPC };
    int i;

    p->cw_opt = 0;
    p->cw_naks = 0;
    /* set again by ccp_build_cr if the new wish is MPPE: an MPPE request the
     * peer refused must not count as agreed when CCP opens on another one */
    p->cw_mppe = 0;
    if (p->cfg.comp_no_rx && !mppe_want(p))
        return;
    /* MPPE, when it is wanted, before any compression: the caller asked */
    if (mppe_want(p) && !(p->cw_tried & cw_bit(CCP_OPT_MPPC))) {
        p->cw_opt = CCP_OPT_MPPC;
        return;
    }
    for (i = 0; i < 3; i++) {
        unsigned opt = order[i];
        int want = (p->cfg.comp & comp_of(opt)) != 0;
        if (mppe_want(p))
            want = 0;               /* MPPE or nothing */
        if (want && !(p->cw_tried & cw_bit(opt))) {
            p->cw_opt = opt;
            return;
        }
    }
}

static void ccp_drop_wish(ppp_t *p)
{
    if (p->cw_opt)
        p->cw_tried |= (uint8_t)cw_bit(p->cw_opt);
    ccp_pick(p);
}

static void ccp_reset(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    /* what the peer refused earlier in this session stays refused */
    p->cw_wbits = p->cfg.deflate_wbits;
    p->cw_mppe = p->cpend_mppe = 0;
    ccp_pick(p);
}

static size_t ccp_build_cr(ppp_fsm_t *f, uint8_t *o, size_t max)
{
    ppp_t *p = (ppp_t *)f->ctx;

    (void)max;
    if (p->cw_opt == CCP_OPT_MPPC) {
        /* option 18: MPPE 128-bit when we encrypt and have keys (RFC 3078;
         * stateless unless the peer has asked for stateful), otherwise MPPC
         * (RFC 2118 section 2) */
        o[0] = CCP_OPT_MPPC;
        o[1] = 6;
        p->cw_mppe = (uint8_t)mppe_want(p);
        /* the mode is that of this request: what an Ack of it agrees */
        p->cw_mppe_h = p->cw_want_h;
        p->cw_mppe_key = p->mppe_key;
        put32(o + 2, p->cw_mppe ? (p->mppe_key | (p->cw_mppe_h ? MPPE_BIT_H : 0)) : MPPC_BIT);
        return 6;
    }
    if (p->cw_opt) {
        /* RFC 1979 section 3: window (log2 - 8) and method in one octet,
         * then MBZ and the check method, both zero. Four octets in all,
         * as the figure there shows (its text says "Length 3"). */
        o[0] = p->cw_opt;
        o[1] = 4;
        o[2] = (uint8_t)((p->cw_wbits - 8) << 4 | DEFLATE_METHOD);
        o[3] = 0;
        return 4;
    }
    return 0;
}

static int ccp_judge(ppp_t *p, uint8_t type, const uint8_t *val, size_t vlen,
                     uint8_t *sugg, size_t *slen)
{
    /* RFC 1962 section 4: an option we do not know MUST be rejected; one
     * we know but will not use next to the one we chose is rejected too */
    if (type != p->cchosen)
        return PPP_CONF_REJ;
    if (type == CCP_OPT_MPPC) {
        /* Option 18 carries MPPC (RFC 2118) and/or the MPPE encryption bits
         * (RFC 3078). We accept exactly 128-bit MPPE, stateless or stateful
         * (when we encrypt and have keys), or plain MPPC; anything else is
         * Nak'd toward what we do. Every copy of the option is judged here,
         * not only the one ccp_recv_cr chose, so the length is checked here. */
        uint32_t v = vlen == 4 ? get32(val) : 0;
        if (vlen == 4) {
            /* for the log, whether or not we encrypt: what the peer asked */
            p->cnt.mppe_peer_req = v;
            p->cnt.mppe_seen |= PPP_MPPE_SEEN_REQ;
            /* on request: a caller that asks for MPPE (with the keys in
             * hand) gets it, and from now on nothing in the clear */
            if (mppe_asks(p, v) && !p->mppe_asked) {
                p->mppe_asked = 1;
                if (p->cw_opt != CCP_OPT_MPPC || !p->cw_mppe)
                    p->ccp_switch = 1;      /* our own request becomes MPPE */
            }
        }
        if (mppe_want(p)) {
            uint32_t h = v & MPPE_BIT_H;
            if (vlen == 4 && (v & MPPE_KEYS) && !h)
                /* a peer that asks for stateful MPPE (Windows on a modem
                 * link) gets asked for the same from now on: it would Nak
                 * stateless down to it anyway */
                p->cw_want_h = 0;
            if (vlen == 4)
                mppe_settle(p, v, 1);
            if (vlen == 4 && (v & ~MPPE_BIT_H) == p->mppe_key) {
                p->cpend_opt = type;
                p->cpend_wbits = 0;
                p->cpend_mppe = 1;
                p->cpend_mppe_h = (uint8_t)(h != 0);
                p->cpend_mppe_key = p->mppe_key;
                return PPP_CONF_ACK;
            }
            /* RFC 3078 section 2: Nak with the one key length we do, in the
             * mode the peer asked for; the others, MPPC and the obsolete D
             * bit come off */
            put32(sugg, p->mppe_key | h);
            *slen = 4;
            return PPP_CONF_NAK;
        }
        /* plain MPPC is a downgrade when encryption is required: only when
         * we are not encrypting do we accept compression alone */
        if (!p->cfg.encrypt && (p->cfg.comp & PPP_COMP_MPPC) && v == MPPC_BIT) {
            p->cpend_opt = type;
            p->cpend_wbits = 0;
            p->cpend_mppe = 0;
            return PPP_CONF_ACK;
        }
        put32(sugg, MPPC_BIT);
        *slen = 4;
        return PPP_CONF_NAK;
    }
    /* PPP Deflate. The window is the most the peer's decompressor holds;
     * we may use less (RFC 1979 section 3), so any valid size is fine. */
    if (vlen == 2 && (val[0] & 0x0f) == DEFLATE_METHOD && val[1] == 0 &&
        (val[0] >> 4) + 8 <= 15) {
        p->cpend_opt = type;
        p->cpend_wbits = (uint8_t)((val[0] >> 4) + 8);
        return PPP_CONF_ACK;
    }
    sugg[0] = (uint8_t)((15 - 8) << 4 | DEFLATE_METHOD);
    if (vlen >= 1 && (val[0] >> 4) + 8 <= 15)
        sugg[0] = (uint8_t)((val[0] & 0xf0) | DEFLATE_METHOD);
    sugg[1] = 0;
    *slen = 2;
    return PPP_CONF_NAK;
}

static int ccp_recv_cr(ppp_fsm_t *f, const uint8_t *opts, size_t len,
                       uint8_t *out, size_t max, size_t *outlen)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;
    int rank = 0;

    p->cpend_opt = 0;
    p->cpend_wbits = 0;
    p->cpend_mppe = 0;
    p->cchosen = 0;
    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        int r = 0;

        if (type == CCP_OPT_DEFLATE && (p->cfg.comp & PPP_COMP_DEFLATE)) r = 3;
        else if (type == CCP_OPT_DEFLATE_DRAFT && (p->cfg.comp & PPP_COMP_DEFLATE)) r = 2;
        else if (type == CCP_OPT_MPPC && vlen == 4 && mppe_asks(p, get32(val))) r = 4;  /* encryption first */
        else if (type == CCP_OPT_MPPC && vlen == 4 &&
                 ((p->cfg.comp & PPP_COMP_MPPC) || mppe_want(p))) r = 1;
        if (r > rank) {
            rank = r;
            p->cchosen = type;
        }
    }
    return negotiate(p, f, ccp_judge, opts, len, out, max, outlen);
}

static void ccp_recv_ack(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    (void)f; (void)opts; (void)len;
}

static void ccp_recv_nak(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;
    int ours = 0;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        if (type != p->cw_opt)
            continue;
        ours = 1;
        if (type == CCP_OPT_MPPC && p->cw_mppe) {
            /* RFC 3078 section 2: the Nak names the one MPPE setting the
             * peer will do. 128-bit in either mode is ours too: ask for
             * that. Without the S bit (40/56-bit only) it is not with us,
             * and with encryption required the link will end. */
            uint32_t v = vlen == 4 ? get32(val) : 0;
            if (vlen == 4) {
                p->cnt.mppe_peer_nak = v;
                p->cnt.mppe_seen |= PPP_MPPE_SEEN_NAK;
            }
            if (vlen == 4)
                mppe_settle(p, v, 0);
            if ((v & p->mppe_key) && ++p->cw_naks <= CW_NAK_LIMIT)
                p->cw_want_h = (uint8_t)((v & MPPE_BIT_H) != 0);
            else
                ccp_drop_wish(p);
            continue;
        }
        if (type != CCP_OPT_MPPC && vlen == 2 && (val[0] & 0x0f) == DEFLATE_METHOD &&
            val[1] == 0 && (val[0] >> 4) + 8 <= p->cw_wbits) {
            /* the peer compresses with a smaller window than we offered */
            p->cw_wbits = (uint8_t)((val[0] >> 4) + 8);
            if (++p->cw_naks > CW_NAK_LIMIT)
                ccp_drop_wish(p);
        } else {
            /* a larger window than we have, another method, or MPPC with
             * bits we do not do: not with us */
            ccp_drop_wish(p);
        }
    }
    /* RFC 1962 section 4: a Nak lists what would be acceptable. If ours is
     * not in it, try the next one; the order gets to whatever it names. */
    if (!ours && len > 0)
        ccp_drop_wish(p);
}

static void ccp_recv_rej(ppp_fsm_t *f, const uint8_t *opts, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;
    size_t pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    while (ppp_fsm_next_option(opts, len, &pos, &type, &val, &vlen)) {
        if (type != p->cw_opt)
            continue;
        /* MPPE at a key length the peer has since shown it lacks (pppd
         * Rejects where Windows Naks): ask again at the one now settled */
        if (type == CCP_OPT_MPPC && p->cw_mppe && p->cw_mppe_key != p->mppe_key &&
            ++p->cw_naks <= CW_NAK_LIMIT)
            continue;
        ccp_drop_wish(p);
    }
}

static void ccp_up(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    /* MPPE is symmetric: only when both directions agreed it (and the keys
     * are in hand) do we encrypt, keying both from the MS-CHAPv2 master keys.
     * Otherwise the option-18 result is MPPC, settled below. */
    if (p->cw_mppe && p->cpend_mppe && p->mppe_have) {
        /* Re-keying MPPE from the master key restarts the RC4 key schedule at
         * coherency 0 -- the same keystream over new data. So key it only on
         * the first CCP open of an authentication; if CCP re-opens after MPPE
         * has run, end the link rather than reuse the keystream. */
        if (p->mppe_started) {
            p->tx_alg = p->rx_alg = PPP_COMP_NONE;
            request_close(p, PPP_END_ENCRYPT_REQUIRED);
            return;
        }
        if (p->cw_mppe_key != p->cpend_mppe_key) {
            /* our request was Acked at one key length and the peer's at
             * another (it showed its shorter keys only after Acking ours):
             * MPPE is one length both ways, so ask again at the one now
             * settled, before anything is keyed */
            p->tx_alg = p->rx_alg = PPP_COMP_NONE;
            p->ccp_redo = 1;
            return;
        }
        /* RFC 3079 3.3: the short keys are the first 8 octets of the
         * master start keys, their first octets then forced (3.4) */
        ppp_mppe_begin(&p->mppe, p->mppe_key == MPPE_BIT_S ? p->mppe_send : p->mppe_short_send,
                       p->mppe_key == MPPE_BIT_S ? p->mppe_recv : p->mppe_short_recv,
                       p->mppe_key == MPPE_BIT_S ? 16 : 8,
                       p->mppe_key == MPPE_BIT_M ? PPP_MPPE_FORCE_56
                       : p->mppe_key == MPPE_BIT_L ? PPP_MPPE_FORCE_40 : PPP_MPPE_FORCE_NONE);
        p->cnt.mppe_keybits = (uint8_t)(p->mppe_key == MPPE_BIT_S ? 128 : p->mppe_key == MPPE_BIT_M ? 56 : 40);
        /* the mode of each direction is the one its Ack agreed: the peer's
         * request is our transmit direction, ours our receive */
        ppp_mppe_stateful(&p->mppe, !p->cpend_mppe_h, !p->cw_mppe_h);
        p->cnt.mppe_stateful = (uint8_t)((p->cpend_mppe_h ? 0 : 1) | (p->cw_mppe_h ? 0 : 2));
        p->mppe_started = 1;
        p->tx_alg = p->rx_alg = PPP_COMP_MPPE;
        p->tx_seq = p->rx_seq = 0;
        p->rx_wait = 0;
        p->comp_credit = PPP_MAX_MRU;
        p->cnt.ccp_opens++;
        p->cnt.comp_tx = p->cnt.comp_rx = PPP_COMP_MPPE;
        p->cnt.comp_tx_wbits = p->cnt.comp_rx_wbits = 0;
        return;
    }
    if (p->cfg.encrypt) {
        /* encryption is required but MPPE was not agreed both ways: fail
         * closed. Never fall back to compression or cleartext -- end the
         * link instead of carrying the caller's traffic unencrypted. */
        p->tx_alg = p->rx_alg = PPP_COMP_NONE;
        request_close(p, PPP_END_ENCRYPT_REQUIRED);
        return;
    }
    if (mppe_want(p)) {
        /* on request: the caller asked for MPPE, but CCP came up without it
         * both ways (it also Acked our compression): ask again with MPPE,
         * a few times, then end rather than carry its traffic in the clear */
        p->tx_alg = p->rx_alg = PPP_COMP_NONE;
        if (++p->mppe_redos > 3) {
            request_close(p, PPP_END_ENCRYPT_REQUIRED);
            return;
        }
        p->cw_tried &= (uint8_t)~cw_bit(CCP_OPT_MPPC);
        p->ccp_redo = 1;
        return;
    }
    p->tx_alg = (uint8_t)comp_of(p->cpend_opt);
    p->rx_alg = (uint8_t)comp_of(p->cw_opt);
    /* RFC 1979 2.1 / RFC 2118 3.1: both counts start at 0 with an empty history */
    p->tx_seq = p->rx_seq = 0;
    p->rx_wait = 0;
    p->comp_credit = PPP_MAX_MRU;
    p->cnt.ccp_opens++;
    if (p->tx_alg == PPP_COMP_DEFLATE) {
        ppp_deflate_enc_init(&p->cenc.d, p->cpend_wbits, NULL);
        p->cnt.comp_tx_wbits = p->cenc.d.wbits;
    } else if (p->tx_alg == PPP_COMP_MPPC) {
        ppp_mppc_enc_init(&p->cenc.m);
        p->cnt.comp_tx_wbits = 0;
    }
    if (p->rx_alg == PPP_COMP_DEFLATE) {
        ppp_deflate_dec_init(&p->cdec.d);
        p->cnt.comp_rx_wbits = p->cw_wbits;
    } else if (p->rx_alg == PPP_COMP_MPPC) {
        ppp_mppc_dec_init(&p->cdec.m);
        p->cnt.comp_rx_wbits = 0;
    }
    if (p->tx_alg) p->cnt.comp_tx = p->tx_alg;
    if (p->rx_alg) p->cnt.comp_rx = p->rx_alg;
}

static void ccp_down(ppp_fsm_t *f)
{
    ppp_t *p = (ppp_t *)f->ctx;

    /* RFC 1962 2.1: compressed datagrams only while CCP is Opened */
    p->tx_alg = p->rx_alg = PPP_COMP_NONE;
    p->rx_wait = 0;
    ppp_mppe_reset(&p->mppe);
}

/* Negotiate CCP anew from the Opened state (RFC 1979 section 2 allows
 * this in place of a Reset-Request; here it is the way out when resets
 * get no answer). */
static void ccp_renegotiate(ppp_t *p)
{
    if (p->ccp.state != PPP_FSM_OPENED)
        return;
    ppp_fsm_down(&p->ccp);
    ppp_fsm_up(&p->ccp);
}

/* RFC 1962 3.1: the decompressor has lost step. Ask the peer to start its
 * compressor afresh; until it has, compressed packets are discarded. */
static void comp_rx_error(ppp_t *p)
{
    ppp_vj_rx_lost(&p->vj);
    p->cnt.comp_rx_errors++;
    p->cnt.comp_rx_dropped++;
    if (p->ccp.rejected_codes & (1u << CCP_RESET_REQ)) {
        ccp_renegotiate(p);                 /* the peer has no Reset-Request */
        return;
    }
    p->rx_wait = 1;
    p->rr_id = ppp_fsm_new_id(&p->ccp);
    p->rr_tries = 1;
    p->rr_timer = p->cfg.comp_reset_ms ? p->cfg.comp_reset_ms : p->ccp.restart_ms;
    tx_cp_next(p, PPP_PROTO_CCP, CCP_RESET_REQ, p->rr_id);
}

/* RFC 1962 3.1: the request again, with the same identifier; when that has
 * been done often enough, CCP from the start */
static void comp_reset_again(ppp_t *p)
{
    if (p->rr_tries >= RESET_TRIES) {
        ccp_renegotiate(p);
        return;
    }
    p->rr_tries++;
    p->rr_timer = p->cfg.comp_reset_ms ? p->cfg.comp_reset_ms : p->ccp.restart_ms;
    tx_cp_next(p, PPP_PROTO_CCP, CCP_RESET_REQ, p->rr_id);
}

/* An MPPE packet that could not be used. Stateful and out of step: one CCP
 * Reset-Request (RFC 3078 8.2), repeated a Restart period later only while
 * packets keep coming that cannot be decrypted. The peer's next FLUSHED
 * packet, or its next flag packet if it has no Reset-Request, brings us
 * back. No Reset-Ack is waited for. */
static void mppe_rx_error(ppp_t *p)
{
    ppp_vj_rx_lost(&p->vj);
    p->cnt.comp_rx_errors++;
    if (ppp_mppe_rx_late(&p->mppe) >= PPP_MPPE_LATE_MAX) {
        /* the peer's count is out of reach for good: end the link rather
         * than stay up carrying nothing (a re-key would reuse the keystream) */
        p->tx_alg = p->rx_alg = PPP_COMP_NONE;
        request_close(p, PPP_END_MPPE_LOST);
        return;
    }
    if (!ppp_mppe_rx_out_of_step(&p->mppe))
        return;
    p->cnt.comp_rx_dropped++;
    ppp_vj_rx_lost(&p->vj);     /* a VJ packet may have been in it (RFC 1144 4.2) */
    if (p->rx_wait && p->rr_timer > 0)
        return;
    if (p->ccp.rejected_codes & (1u << CCP_RESET_REQ))
        return;
    if (!p->rx_wait)
        p->rr_id = ppp_fsm_new_id(&p->ccp);
    p->rx_wait = 1;
    p->rr_timer = p->cfg.comp_reset_ms ? p->cfg.comp_reset_ms : p->ccp.restart_ms;
    tx_cp_next(p, PPP_PROTO_CCP, CCP_RESET_REQ, p->rr_id);
}

/* Encryption required: MPPE must be up both ways within CCP's own retry
 * span (a request every restart_ms, max_configure of them, and one more for
 * slack) of the network phase starting. CCP that never converges, a peer
 * that never answers it, anything that leaves IPCP up with MPPE down: the
 * link ends instead of staying up carrying nothing. */
static void encrypt_tick(ppp_t *p, uint32_t ms)
{
    uint32_t limit = p->cfg.restart_ms * (p->cfg.max_configure + 1u);

    if (!mppe_hold(p) || p->phase != PPP_PHASE_NETWORK || p->tx_alg == PPP_COMP_MPPE)
        return;
    p->enc_wait_ms += ms;
    if (p->enc_wait_ms >= limit)
        request_close(p, PPP_END_ENCRYPT_REQUIRED);
}

static void ccp_tick(ppp_t *p, uint32_t ms)
{
    uint32_t rate = p->cfg.comp_rate ? p->cfg.comp_rate : PPP_COMP_RATE;

    if (p->ccp.state != PPP_FSM_OPENED)
        return;
    if (p->comp_credit < PPP_MAX_MRU) {
        p->comp_credit += (int32_t)(ms * rate);
        if (p->comp_credit > PPP_MAX_MRU)
            p->comp_credit = PPP_MAX_MRU;
    }
    if (!p->rx_wait)
        return;
    if (ms < p->rr_timer) {
        p->rr_timer -= ms;
        return;
    }
    p->rr_timer = 0;
    /* With MPPC nothing answers a Reset-Request but the next packet, which
     * carries FLUSHED (RFC 2118 4.3; the Reset-Ack is not used). A peer
     * with nothing to send is not a peer that did not hear us: there the
     * request is repeated only when a packet shows that it is needed
     * (comp_input). */
    if (p->rx_alg == PPP_COMP_DEFLATE)
        comp_reset_again(p);
}

static int ccp_ext_code(ppp_fsm_t *f, uint8_t code, uint8_t id,
                        const uint8_t *data, size_t len)
{
    ppp_t *p = (ppp_t *)f->ctx;

    (void)data; (void)len;
    switch (code) {
    case CCP_RESET_REQ:
        if (f->state != PPP_FSM_OPENED)
            return PPP_FSM_EXT_NONE;
        if (p->tx_alg == PPP_COMP_MPPE) {
            /* RFC 3078 8.2: the next packet resynchronises the peer
             * (stateful; stateless packets all do). What waits in the
             * buffer the peer would only drop. MPPE needs no Reset-Ack,
             * but the Linux kernel's receiver stays in its error state
             * until it sees one; Windows Code-Rejects it (which the FSM
             * notes), after which none is sent. */
            p->cnt.comp_tx_resets++;
            if (p->mppe.tx.stateful) {
                tx_purge_compressed(p);
                ppp_mppe_tx_resync(&p->mppe);
            }
            if (!(f->rejected_codes & (1u << CCP_RESET_ACK)))
                f->ops->send(f, CCP_RESET_ACK, id, NULL, 0, NULL, 0);
            return PPP_FSM_EXT_NONE;
        }
        /* RFC 1962 3.1: reset the compressor, and a Reset-Ack MUST go out
         * with the request's identifier, every time. */
        if (p->tx_alg == PPP_COMP_DEFLATE) {
            /* RFC 1979 section 2: history cleared, sequence number 0 */
            ppp_deflate_enc_reset(&p->cenc.d);
            p->tx_seq = 0;
        } else if (p->tx_alg == PPP_COMP_MPPC) {
            /* RFC 2118 4.3: history flushed, FLUSHED on the next packet;
             * the coherency count goes on */
            ppp_mppc_enc_flush(&p->cenc.m);
        }
        p->cnt.comp_tx_resets++;
        /* what is still waiting was compressed before the reset: the peer
         * could do nothing with it, and the Reset-Ack would wait behind it */
        tx_purge_compressed(p);
        f->ops->send(f, CCP_RESET_ACK, id, NULL, 0, NULL, 0);
        return PPP_FSM_EXT_NONE;
    case CCP_RESET_ACK:
        /* RFC 1962 3.1: for each Reset-Ack with the expected identifier.
         * MPPC does not use it: there the FLUSHED bit of a packet says
         * where the peer started afresh (RFC 2118 4.3). */
        if (f->state == PPP_FSM_OPENED && p->rx_wait && id == p->rr_id &&
            p->rx_alg == PPP_COMP_DEFLATE) {
            ppp_deflate_dec_reset(&p->cdec.d);
            p->rx_seq = 0;
            p->rx_wait = 0;
        }
        return PPP_FSM_EXT_NONE;
    default:
        return PPP_FSM_EXT_RUC;
    }
}

static const ppp_fsm_ops_t ccp_ops = {
    fsm_send, ccp_reset, ccp_build_cr, ccp_recv_cr,
    ccp_recv_ack, ccp_recv_nak, ccp_recv_rej,
    ccp_up, ccp_down, NULL, NULL, ccp_ext_code
};

static void network_start(ppp_t *p)
{
    if (p->cfg.encrypt && !p->mppe_have) {
        /* encryption required, but the login gave no MPPE keys (it was not
         * MS-CHAPv2): nothing could ever be carried, so do not open IPCP */
        request_close(p, PPP_END_ENCRYPT_REQUIRED);
        return;
    }
    set_phase(p, PPP_PHASE_NETWORK);
    ppp_fsm_open(&p->ipcp);
    ppp_fsm_up(&p->ipcp);
    /* RFC 1962 section 2: CCP belongs to the Network-Layer Protocol phase */
    p->cw_tried = 0;
    p->cw_want_h = 1;           /* stateless first, as pppd asks */
    p->mppe_key = MPPE_BIT_S;   /* 128-bit until the peer shows it has less */
    p->mppe_floor = 0;          /* no request from the peer yet */
    p->mppe_asked = 0;
    p->ccp_switch = 0;
    p->mppe_redos = 0;
    p->ccp_redo = 0;
    p->enc_wait_ms = 0;
    p->ccp_wait_ms = 0;
    if (ccp_wanted(p)) {
        if (p->mppe_have && !p->cfg.encrypt)
            /* encryption on request: wait (a Restart period at most) for
             * the caller's request, so that ours can be MPPE if it asks
             * for it -- pppd with require-mppe ends the link on a server
             * whose first request is anything else */
            p->ccp_wait_ms = p->ccp.restart_ms ? p->ccp.restart_ms : 3000;
        else
            ccp_start(p);
    }
}

/* CCP from Initial: open it and send our request */
static void ccp_start(ppp_t *p)
{
    p->ccp_wait_ms = 0;
    ppp_fsm_open(&p->ccp);
    ppp_fsm_up(&p->ccp);
}

/* While CCP waits for the caller (encryption on request): what its first
 * request says about MPPE, before ours is built from it */
static void ccp_prescan(ppp_t *p, const uint8_t *info, size_t n)
{
    size_t len, pos = 0, vlen;
    uint8_t type;
    const uint8_t *val;

    if (n < 4 || info[0] != PPP_CONF_REQ)
        return;
    len = (size_t)info[2] << 8 | info[3];
    if (len < 4 || len > n)
        return;
    while (ppp_fsm_next_option(info + 4, len - 4, &pos, &type, &val, &vlen)) {
        uint32_t v;
        if (type != CCP_OPT_MPPC || vlen != 4)
            continue;
        v = get32(val);
        if (!mppe_asks(p, v))
            continue;
        p->mppe_asked = 1;
        mppe_settle(p, v, 1);
        p->cw_want_h = (uint8_t)((v & MPPE_BIT_H) != 0);
    }
}

/* take IPCP and CCP all the way back to Initial, whatever state they are in */
static void network_stop(ppp_t *p)
{
    p->ipcp_closing = 1;
    ppp_fsm_down(&p->ipcp);
    ppp_fsm_close(&p->ipcp);
    p->ipcp_closing = 0;
    p->peer_ip = 0;
    ppp_fsm_down(&p->ccp);
    ppp_fsm_close(&p->ccp);
    p->ccp_wait_ms = 0;         /* a wait for the caller's CCP request ends too */
}

/* ---------------------------------------------------------------- */
/* receive path */

static void protocol_reject(ppp_t *p, uint16_t proto,
                            const uint8_t *info, size_t n)
{
    uint8_t pr[2];
    size_t room = (size_t)p->peer_mru - 6;   /* LCP header + protocol */

    p->cnt.rx_unknown_proto++;
    pr[0] = (uint8_t)(proto >> 8);
    pr[1] = (uint8_t)proto;
    if (n > room)
        n = room;
    tx_cp(p, PPP_PROTO_LCP, LCP_PROTO_REJ, ppp_fsm_new_id(&p->lcp),
          pr, 2, info, n);
}

/* a network-layer packet, as it arrived or after decompression */
static void net_input(ppp_t *p, uint16_t proto, const uint8_t *info, size_t n, int encrypted)
{
    switch (proto) {
    case PPP_PROTO_IPCP:
        ppp_fsm_input(&p->ipcp, info, n);
        break;
    case PPP_PROTO_VJC:
    case PPP_PROTO_VJUC:
        /* Van Jacobson: only where we asked for it (otherwise as any
         * protocol not agreed), rebuilt into the datagram it was */
        if (!p->vj.rx_slots) {
            protocol_reject(p, proto, info, n);
            break;
        }
        if ((mppe_hold(p) && !encrypted) || p->ipcp.state != PPP_FSM_OPENED) {
            p->cnt.rx_discards++;
            break;
        }
        {
            int r = ppp_vj_uncompress(&p->vj, proto, info, n, p->vjbuf, sizeof p->vjbuf);
            if (r < 0) {
                p->cnt.vj_errors++;
                break;
            }
            if (proto == PPP_PROTO_VJC) p->cnt.vj_rx_comp++; else p->cnt.vj_rx_uncomp++;
            p->cnt.rx_ip++;
            if (p->cfg.ip_rx)
                p->cfg.ip_rx(p->cfg.ctx, p->vjbuf, (size_t)r);
        }
        break;
    case PPP_PROTO_IP:
        /* fail closed: with encryption required, IP only via MPPE, never a
         * cleartext datagram slipped past the negotiation */
        if (mppe_hold(p) && !encrypted) {
            p->cnt.rx_discards++;
        } else if (p->ipcp.state == PPP_FSM_OPENED) {
            p->cnt.rx_ip++;
            if (p->cfg.ip_rx)
                p->cfg.ip_rx(p->cfg.ctx, info, n);
        } else {
            p->cnt.rx_discards++;
        }
        break;
    default:
        protocol_reject(p, proto, info, n);
        break;
    }
}

/* a compressed datagram (protocol 0x00FD) while a decompressor is on */
static void comp_input(ppp_t *p, const uint8_t *info, size_t n)
{
    const uint8_t *d;
    unsigned hdr;
    uint16_t proto;
    size_t off;
    int r;

    p->cnt.comp_rx_in += (uint32_t)n;
    if (n < 3) {
        /* too short for a header and a protocol field. With MPPE it is
         * dropped like any bad frame, with no Reset-Request (see below) */
        if (p->rx_alg == PPP_COMP_MPPE)
            p->cnt.comp_rx_errors++;
        else if (!p->rx_wait)
            comp_rx_error(p);
        return;
    }
    hdr = (unsigned)info[0] << 8 | info[1];
    if (p->rx_alg == PPP_COMP_MPPE) {
        /* RFC 3078: decrypt; the plaintext begins with the PPP protocol
         * field, parsed below as for the compressors. */
        r = ppp_mppe_decrypt(&p->mppe, info, n, p->cbuf_rx, sizeof p->cbuf_rx);
        if (r < 1) {
            /* a bad coherency count or header: drop it and count it. The
             * stateless cipher resynchronises on the next good packet; a
             * stateful one asks the peer for a FLUSHED packet. Never a CCP
             * renegotiation: that would re-key MPPE and reuse the keystream. */
            mppe_rx_error(p);
            return;
        }
        p->rx_wait = 0;
        d = p->cbuf_rx;
    } else if (p->rx_alg == PPP_COMP_DEFLATE) {
        /* RFC 1979 section 2: everything is discarded between an error
         * and the Reset-Ack; the sequence number is checked before the
         * packet is decoded */
        if (p->rx_wait) {
            p->cnt.comp_rx_dropped++;
            ppp_vj_rx_lost(&p->vj);     /* a VJ packet may have been in it (RFC 1144 4.2) */
            return;
        }
        r = hdr == p->rx_seq ? ppp_deflate_decompress(&p->cdec.d, info + 2, n - 2, p->cbuf_rx, sizeof p->cbuf_rx) : -1;
        if (r < 1) {
            comp_rx_error(p);
            return;
        }
        p->rx_seq++;
        d = p->cbuf_rx;
    } else {
        /* RFC 2118 3.1 and 4.3 */
        if (hdr & PPP_MPPC_FLUSHED) {
            /* the peer started afresh with this packet: so do we, and its
             * count is the one to go by */
            ppp_mppc_dec_flush(&p->cdec.m);
            p->rx_seq = hdr & PPP_MPPC_COUNT_MASK;
            p->rx_wait = 0;
        } else if (p->rx_wait) {
            p->cnt.comp_rx_dropped++;
            ppp_vj_rx_lost(&p->vj);     /* a VJ packet may have been in it (RFC 1144 4.2) */
            /* still the old history, a Restart period after we asked */
            if (p->rr_timer == 0)
                comp_reset_again(p);
            return;
        }
        if ((hdr & PPP_MPPC_COUNT_MASK) != p->rx_seq || (hdr & PPP_MPPC_ENCRYPTED)) {
            comp_rx_error(p);
            return;
        }
        p->rx_seq = (p->rx_seq + 1) & PPP_MPPC_COUNT_MASK;
        if (hdr & PPP_MPPC_COMPRESSED) {
            r = ppp_mppc_decompress(&p->cdec.m, (hdr & PPP_MPPC_AT_FRONT) != 0,
                                    info + 2, n - 2, p->cbuf_rx, sizeof p->cbuf_rx);
            if (r < 1) {
                comp_rx_error(p);
                return;
            }
            d = p->cbuf_rx;
        } else {
            /* sent as it was because it would have grown (section 3,
             * "Data Expansion"); the peer flushes after it */
            d = info + 2;
            r = (int)(n - 2);
        }
    }
    /* The packet starts with its Protocol field (RFC 1979 2.1: in one
     * octet where the number allows; RFC 2118 3.1 does not say, so either
     * form is taken: the first octet of a two-octet number is even, RFC
     * 1661 section 2). */
    if (d[0] & 1) {
        proto = d[0];
        off = 1;
    } else if (r >= 2 && (d[1] & 1)) {
        proto = (uint16_t)((unsigned)d[0] << 8 | d[1]);
        off = 2;
    } else {
        p->cnt.rx_bad_header++;
        return;
    }
    if (p->rx_alg == PPP_COMP_MPPE && (proto < 0x21 || proto > 0xfa)) {
        /* RFC 3078 section 3: MPPE carries 0x0021..0x00FA only. Anything
         * else decrypted with the wrong key: out of step */
        ppp_mppe_rx_lost(&p->mppe);
        mppe_rx_error(p);
        return;
    }
    p->cnt.comp_rx_out += (uint32_t)((size_t)r - off);
    /* only network-layer packets are ever compressed (RFC 1979 section 2,
     * RFC 2118 section 3) */
    if (proto >= 0x4000 || proto == PPP_PROTO_COMP || proto == 0x00fb) {
        p->cnt.rx_bad_header++;
        return;
    }
    net_input(p, proto, d + off, (size_t)r - off, 1);   /* came decrypted/decompressed */
}

static void handle_frame(ppp_t *p)
{
    const uint8_t *info;
    uint16_t proto;
    size_t off, n;

    if (ppp_hdlc_parse(p->rx.buf, p->rx.len, p->rx_acfc, p->rx_pfc,
                       &proto, &off) != 0) {
        p->cnt.rx_bad_header++;
        return;
    }
    info = p->rx.buf + off;
    n = p->rx.len - off;
    trace(p, 0, proto, info, n, NULL, 0, NULL, 0);

    /* any frame from the caller shows it is there: the keepalive counts only
     * echoes nobody else answered (a WebTV box answers no Echo-Request, and
     * had its busy V.34 call ended at 2:15). Our own echoes looped back,
     * with our magic number, show nothing. */
    if (p->echo_pending && p->lcp.state == PPP_FSM_OPENED) {
        uint32_t ours = p->w_magic ? p->magic : 0;
        int looped = proto == PPP_PROTO_LCP && n >= 8 &&
                     (info[0] == LCP_ECHO_REQ || info[0] == LCP_ECHO_REP) &&
                     ours != 0 && get32(info + 4) == ours;
        if (!looped)
            p->echo_pending = 0;
    }

    if (p->lcp_wait_ms) {
        /* the caller's first frame while ours waits: its LCP request says
         * whether it is Windows 98 (MS-CHAP v1 first); anything else lets
         * ours go as it is */
        if (proto == PPP_PROTO_LCP && n >= 4 && info[0] == PPP_CONF_REQ) {
            size_t l = ((size_t)info[2] << 8) | info[3];
            if (l >= 4 && l <= n)
                p->cnt.win98 = (uint8_t)is_win98_lcp(info + 4, l - 4);
        }
        lcp_release(p);
    }

    if (proto == PPP_PROTO_LCP) {
        int was_open = p->lcp.state == PPP_FSM_OPENED;

        ppp_fsm_input(&p->lcp, info, n);
        if (was_open && p->lcp.state == PPP_FSM_STOPPING &&
            n > 0 && info[0] == PPP_TERM_REQ)
            set_end(p, PPP_END_PEER_TERMINATED);
        return;
    }
    /* RFC 1661 3.4: before LCP is Opened everything else is discarded */
    if (p->lcp.state != PPP_FSM_OPENED) {
        p->cnt.rx_discards++;
        return;
    }
    if (proto == PPP_PROTO_PAP || proto == PPP_PROTO_CHAP) {
        int match = proto == PPP_PROTO_PAP
                        ? p->auth_proto == PPP_AUTH_PAP
                        : is_chap(p->auth_proto);

        if (match) {
            if (proto == PPP_PROTO_PAP)
                pap_input(p, info, n);
            else
                chap_input(p, info, n);
        } else if (p->phase == PPP_PHASE_NETWORK) {
            protocol_reject(p, proto, info, n);
        } else {
            p->cnt.rx_discards++;
        }
        return;
    }
    /* RFC 1661 3.5: network-layer traffic only after authentication */
    if (p->phase != PPP_PHASE_NETWORK) {
        p->cnt.rx_discards++;
        return;
    }
    /* RFC 1979 section 2, "Data Expansion": with Deflate, a packet that
     * arrives in its native form was sent that way because it did not
     * shrink. It still went through the peer's compressor, so it goes
     * into our history and takes a sequence number: every protocol number
     * from 0x0000 to 0x3FFF except the two for compressed datagrams. The
     * Protocol field is taken in its one-octet form where it has one
     * (RFC 1979 2.1), however it came. */
    if (p->rx_alg == PPP_COMP_DEFLATE && !p->rx_wait && proto < 0x4000 &&
        proto != PPP_PROTO_COMP && proto != 0x00fb) {
        uint8_t pf[2];
        size_t pl = 0;

        if (proto > 0xff)
            pf[pl++] = (uint8_t)(proto >> 8);
        pf[pl++] = (uint8_t)proto;
        ppp_deflate_dec_add(&p->cdec.d, pf, pl, info, n);
        p->rx_seq++;
        p->cnt.comp_rx_in += (uint32_t)n;
        p->cnt.comp_rx_out += (uint32_t)n;
    }
    switch (proto) {
    case PPP_PROTO_CCP:
        if (ccp_wanted(p)) {
            if (p->ccp_wait_ms) {
                ccp_prescan(p, info, n);
                ccp_start(p);
            }
            ppp_fsm_input(&p->ccp, info, n);
            if (p->ccp_redo) {
                p->ccp_redo = 0;
                p->ccp_switch = 0;
                ccp_renegotiate(p);
            } else if (p->ccp_switch) {
                /* the caller asked for MPPE while our request is still for
                 * compression: the next one we send is MPPE */
                p->ccp_switch = 0;
                if (p->ccp.state != PPP_FSM_OPENED) {
                    /* send it now: back to Starting and up again (nothing
                     * was Opened, so nothing is torn down) */
                    p->cw_tried &= (uint8_t)~cw_bit(CCP_OPT_MPPC);
                    ppp_fsm_down(&p->ccp);
                    ppp_fsm_up(&p->ccp);
                }
            }
        } else
            protocol_reject(p, proto, info, n);
        break;
    case PPP_PROTO_COMP:
        if (p->rx_alg != PPP_COMP_NONE)
            comp_input(p, info, n);
        else if (ccp_wanted(p))
            p->cnt.rx_discards++;       /* RFC 1962 2.1: not before CCP is Opened */
        else
            protocol_reject(p, proto, info, n);
        break;
    default:
        net_input(p, proto, info, n, 0);        /* cleartext off the wire */
        break;
    }
}

/* ---------------------------------------------------------------- */
/* public API */

void ppp_config_default(ppp_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->hostname = "v90d";
    c->mru = PPP_MAX_MRU;
    c->restart_ms = 3000;
    c->max_configure = 10;
    c->max_terminate = 2;
    c->max_failure = 5;
    c->echo_max_fail = 3;
    c->comp = PPP_COMP_DEFLATE | PPP_COMP_MPPC;
}

static void fsm_setup(ppp_t *p, ppp_fsm_t *f, const ppp_fsm_ops_t *ops)
{
    ppp_fsm_init(f, ops, p, p->scratch, sizeof p->scratch);
    f->restart_ms = p->cfg.restart_ms;
    f->max_configure = p->cfg.max_configure;
    f->max_terminate = p->cfg.max_terminate;
    f->max_failure = p->cfg.max_failure;
}

void ppp_init(ppp_t *p, const ppp_config_t *cfg)
{
    uint8_t seed[4];

    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    /* zero means "default" for these */
    if (!p->cfg.hostname) p->cfg.hostname = "v90d";
    if (!p->cfg.restart_ms) p->cfg.restart_ms = 3000;
    if (!p->cfg.max_configure) p->cfg.max_configure = 10;
    if (!p->cfg.max_terminate) p->cfg.max_terminate = 2;
    if (!p->cfg.max_failure) p->cfg.max_failure = 5;
    if (!p->cfg.echo_max_fail) p->cfg.echo_max_fail = 3;
    if (p->cfg.mru == 0 || p->cfg.mru > PPP_MAX_MRU) p->cfg.mru = PPP_MAX_MRU;
    if (p->cfg.mru < PPP_MIN_MRU) p->cfg.mru = PPP_MIN_MRU;
    if (!p->cfg.users) p->cfg.n_users = 0;

    fsm_setup(p, &p->lcp, &lcp_ops);
    fsm_setup(p, &p->ipcp, &ipcp_ops);
    fsm_setup(p, &p->ccp, &ccp_ops);
    p->cfg.comp &= PPP_COMP_DEFLATE | PPP_COMP_MPPC;
    /* with encryption required CCP carries MPPE and nothing else: offering
     * or accepting Deflate or MPPC would only lead to a CCP without MPPE,
     * and the link ending (ppp.h, encrypt) */
    if (p->cfg.encrypt)
        p->cfg.comp = 0;
    if (p->cfg.deflate_wbits < PPP_DEFLATE_MIN_WBITS || p->cfg.deflate_wbits > PPP_DEFLATE_MAX_WBITS)
        p->cfg.deflate_wbits = PPP_DEFLATE_MAX_WBITS;
    ppp_hdlc_rx_init(&p->rx, PPP_HDLC_BUF);
    p->peer_mru = PPP_MAX_MRU;
    p->tx_accm = 0xffffffffu;
    p->phase = PPP_PHASE_DEAD;

    put32(seed, cfg->seed);
    ppp_add_entropy(p, seed, 4);
    if (cfg->entropy && cfg->entropy_n) ppp_add_entropy(p, cfg->entropy, cfg->entropy_n);
}

void ppp_lower_up(ppp_t *p)
{
    if (p->lcp.state != PPP_FSM_INITIAL || p->phase != PPP_PHASE_DEAD)
        ppp_lower_down(p);

    /* a new call: fresh framing state, counters and bookkeeping */
    ppp_hdlc_rx_init(&p->rx, PPP_HDLC_BUF);
    memset(&p->cnt, 0, sizeof p->cnt);
    p->tx_head = p->tx_tail = 0;
    p->end_reason = PPP_END_NONE;
    p->pending_close = PPP_END_NONE;
    p->peer_name[0] = 0;
    p->peer_ip = 0;
    p->our_ip = p->cfg.our_ip;

    set_phase(p, PPP_PHASE_ESTABLISH);
    ppp_fsm_open(&p->lcp);
    p->lcp_wait_ms = 0;
    if ((p->cfg.auth & PPP_AUTH_MSCHAPV1) && (p->cfg.auth & PPP_AUTH_MSCHAPV2))
        /* which MS-CHAP goes first depends on who calls (lcp_reset): ours
         * waits for the caller's first request, which says, up to a second
         * (callers send theirs at once; Windows retries after 3 s) */
        p->lcp_wait_ms = 1000;
    else
        ppp_fsm_up(&p->lcp);
}

void ppp_lower_down(ppp_t *p)
{
    if (p->phase != PPP_PHASE_DEAD)
        set_end(p, PPP_END_LOWER_DOWN);
    ppp_fsm_down(&p->lcp);        /* This-Layer-Down if it was Opened */
    network_stop(p);
    ppp_fsm_close(&p->lcp);       /* settle in Initial */
    p->lcp_wait_ms = 0;
    p->pending_close = PPP_END_NONE;
    p->tx_head = p->tx_tail = 0;  /* nothing can be sent any more */
    set_phase(p, PPP_PHASE_DEAD);
}

void ppp_close(ppp_t *p)
{
    do_close(p, PPP_END_LOCAL_CLOSE);
}

/* RFC 1144 4.2: a frame lost or damaged on the way in may have been a
 * compressed TCP packet; the decompressor tosses until it is sure again */
static void vj_check_lost(ppp_t *p)
{
    uint32_t lost = p->rx.bad_fcs + p->rx.aborts + p->rx.overlong + p->rx.runts;
    if (lost != p->vj_lost_seen) {
        p->vj_lost_seen = lost;
        ppp_vj_rx_lost(&p->vj);
    }
}

void ppp_input_bytes(ppp_t *p, const uint8_t *buf, size_t n)
{
    size_t i;

    for (i = 0; i < n && p->phase != PPP_PHASE_DEAD; i++) {
        p->rx_octets++;
        if (ppp_hdlc_rx_byte(&p->rx, buf[i])) {
            vj_check_lost(p);       /* one lost before this frame: toss first */
            handle_frame(p);
            run_pending(p);
        }
    }
    vj_check_lost(p);
}

size_t ppp_output_pending(const ppp_t *p)
{
    return p->tx_tail - p->tx_head;
}

size_t ppp_output_bytes(ppp_t *p, uint8_t *buf, size_t max)
{
    size_t n = p->tx_tail - p->tx_head;

    if (n > max)
        n = max;
    memcpy(buf, p->txbuf + p->tx_head, n);
    p->tx_head += n;
    return n;
}

/* An IP packet through the compressor agreed for our direction. Whatever
 * comes of it must go out, in this order: the peer's history follows
 * ours packet by packet. So room for the worst case is made sure of
 * before the compressor sees the packet. */
static int send_compressed(ppp_t *p, uint16_t proto, const uint8_t *pkt, size_t len)
{
    /* every octet escaped, header, FCS, flags; and the uncompressed MPPC
     * form has four octets in front of the packet */
    size_t need = 2 * (len + 10) + 2 + PPP_TX_RESERVE;
    uint8_t h[4];
    size_t cn, sent;
    int r;

    if (PPP_TX_BUF - (p->tx_tail - p->tx_head) < need)
        return PPP_ERR_FULL;
    if (p->comp_credit <= 0)
        return PPP_ERR_FULL;
    if (p->comp_queue && p->tx_tail - p->tx_head > p->comp_queue)
        return PPP_ERR_FULL;
    p->comp_credit -= (int32_t)len;

    if (p->tx_alg == PPP_COMP_MPPE) {
        /* RFC 3078: encrypt the PPP protocol (two octets) and the packet;
         * the 2-octet MPPE header (flags + coherency count) is the frame's
         * header, the ciphertext its information field. */
        uint8_t plain[PPP_MAX_MRU + 2];
        size_t en;

        plain[0] = (uint8_t)(proto >> 8); plain[1] = (uint8_t)proto;
        memcpy(plain + 2, pkt, len);
        en = ppp_mppe_encrypt(&p->mppe, plain, len + 2, p->cbuf_tx, sizeof p->cbuf_tx);
        if (!en) { p->cnt.tx_dropped++; return PPP_ERR_FULL; }
        r = tx_frame(p, PPP_PROTO_COMP, p->cbuf_tx, 2, p->cbuf_tx + 2, en - 2, NULL, 0, PPP_TX_RESERVE);
        sent = en;
    } else if (p->tx_alg == PPP_COMP_DEFLATE) {
        /* RFC 1979 2.1: the Protocol field in one octet, then the packet.
         * The sequence number counts every packet, sent compressed or not. */
        const uint8_t proto1[1] = { (uint8_t)proto };     /* 0x0021, 0x002d or 0x002f: one octet */

        h[0] = (uint8_t)(p->tx_seq >> 8);
        h[1] = (uint8_t)p->tx_seq;
        p->tx_seq++;
        /* compressed only if that is shorter, sequence number included
         * (RFC 1979 section 2, "Data Expansion"); then it also fits the
         * peer's MRU, since the packet itself does */
        cn = ppp_deflate_compress(&p->cenc.d, proto1, 1, pkt, len, p->cbuf_tx, len > 3 ? len - 3 : 0);
        if (cn) {
            r = tx_frame(p, PPP_PROTO_COMP, h, 2, p->cbuf_tx, cn, NULL, 0, PPP_TX_RESERVE);
            sent = cn + 2;
        } else {
            r = tx_frame(p, proto, NULL, 0, pkt, len, NULL, 0, PPP_TX_RESERVE);
            sent = len;
            p->cnt.comp_tx_native++;
        }
    } else {
        /* RFC 2118 3.1: the Protocol field (two octets here), then the
         * packet. A compressed packet may be at most one octet longer
         * than the packet was (beyond that it has expanded, section 3)
         * and must fit the peer's MRU with its two-octet header. */
        const uint8_t proto2[2] = { (uint8_t)(proto >> 8), (uint8_t)proto };
        size_t max = len + 1;
        unsigned bits;

        if (max > (size_t)p->peer_mru - 2)
            max = (size_t)p->peer_mru - 2;
        cn = ppp_mppc_compress(&p->cenc.m, proto2, 2, pkt, len, p->cbuf_tx, max, &bits);
        h[0] = (uint8_t)((bits | p->tx_seq) >> 8);
        h[1] = (uint8_t)p->tx_seq;
        if (cn) {
            r = tx_frame(p, PPP_PROTO_COMP, h, 2, p->cbuf_tx, cn, NULL, 0, PPP_TX_RESERVE);
            sent = cn + 2;
            p->tx_seq = (p->tx_seq + 1) & PPP_MPPC_COUNT_MASK;
        } else if (len + 4 <= p->peer_mru) {
            /* "the original data is sent as an uncompressed MPPC packet" */
            h[2] = proto2[0];
            h[3] = proto2[1];
            r = tx_frame(p, PPP_PROTO_COMP, h, 4, pkt, len, NULL, 0, PPP_TX_RESERVE);
            sent = len + 4;
            p->tx_seq = (p->tx_seq + 1) & PPP_MPPC_COUNT_MASK;
            p->cnt.comp_tx_native++;
        } else {
            /* that would be longer than the peer's MRU: in its standard
             * form (RFC 1962 2.1), which MPPC does not count */
            r = tx_frame(p, proto, NULL, 0, pkt, len, NULL, 0, PPP_TX_RESERVE);
            sent = len;
            p->cnt.comp_tx_native++;
        }
    }
    if (r != 0) {
        /* not reached: the room was checked. If it were, the peer would
         * see a gap in the sequence and ask for a reset. */
        p->cnt.tx_dropped++;
        return PPP_ERR_FULL;
    }
    p->cnt.comp_tx_in += (uint32_t)len;
    p->cnt.comp_tx_out += (uint32_t)sent;
    p->cnt.tx_ip++;
    return 0;
}

void ppp_set_comp_queue(ppp_t *p, size_t octets)
{
    p->comp_queue = octets;
}

int ppp_send_ip(ppp_t *p, const uint8_t *pkt, size_t len)
{
    if (p->phase != PPP_PHASE_NETWORK || p->ipcp.state != PPP_FSM_OPENED)
        return PPP_ERR_DOWN;
    /* fail closed: with encryption required, never put a datagram on the wire
     * until MPPE is up (IPCP can open before CCP does) */
    if (mppe_hold(p) && p->tx_alg != PPP_COMP_MPPE)
        return PPP_ERR_DOWN;
    if (len > p->peer_mru)
        return PPP_ERR_SIZE;
    /* MPPE has no native form to fall back on: the whole frame must fit */
    if (p->tx_alg == PPP_COMP_MPPE && len + PPP_MPPE_OVERHEAD > p->peer_mru)
        return PPP_ERR_SIZE;
    {
        /* Van Jacobson first (RFC 1144, on the datagram), then CCP's
         * compression or encryption of whatever that made of it */
        uint16_t proto = PPP_PROTO_IP;
        const uint8_t *d = pkt;
        size_t n = len;
        int r;
        if (p->vj.tx_slots && len > 0) {
            /* the slots change only once the packet is surely sent: keep
             * them to put back if it is not */
            static ppp_vj_slot_t keep[PPP_VJ_SLOTS];
            uint8_t keep_lru[PPP_VJ_SLOTS];
            int keep_last = p->vj.tx_last;
            memcpy(keep, p->vj.tx, sizeof keep);
            memcpy(keep_lru, p->vj.tx_lru, sizeof keep_lru);
            proto = ppp_vj_compress(&p->vj, pkt, len, p->vjbuf, sizeof p->vjbuf, &n);
            if (!proto) { proto = PPP_PROTO_IP; n = len; }
            else d = p->vjbuf;
            /* LCP, IPCP and CCP packets never come this way: only IP is compressed */
            r = p->tx_alg != PPP_COMP_NONE ? send_compressed(p, proto, d, n)
                : tx_frame(p, proto, NULL, 0, d, n, NULL, 0, PPP_TX_RESERVE) != 0 ? PPP_ERR_FULL : 0;
            if (r != 0) {
                memcpy(p->vj.tx, keep, sizeof keep);
                memcpy(p->vj.tx_lru, keep_lru, sizeof keep_lru);
                p->vj.tx_last = keep_last;
                return r;
            }
            if (proto == PPP_PROTO_VJC) p->cnt.vj_tx_comp++;
            else if (proto == PPP_PROTO_VJUC) p->cnt.vj_tx_uncomp++;
            if (p->tx_alg == PPP_COMP_NONE) p->cnt.tx_ip++;
            return 0;
        }
        if (p->tx_alg != PPP_COMP_NONE && len > 0)
            return send_compressed(p, proto, d, n);
        if (tx_frame(p, proto, NULL, 0, d, n, NULL, 0, PPP_TX_RESERVE) != 0)
            return PPP_ERR_FULL;
        p->cnt.tx_ip++;
        return 0;
    }
}

void ppp_tick(ppp_t *p, uint32_t ms)
{
    if (p->phase == PPP_PHASE_DEAD)
        return;
    p->uptime_ms += ms;
    if (p->lcp_wait_ms) {
        if (ms >= p->lcp_wait_ms)
            lcp_release(p);         /* no request from the caller: ours */
        else
            p->lcp_wait_ms -= ms;
    }
    if (p->ccp_wait_ms) {
        if (ms >= p->ccp_wait_ms)
            ccp_start(p);           /* the caller sent no request: ours */
        else
            p->ccp_wait_ms -= ms;
    }
    ppp_fsm_tick(&p->lcp, ms);
    ppp_fsm_tick(&p->ipcp, ms);
    ppp_fsm_tick(&p->ccp, ms);
    ccp_tick(p, ms);
    encrypt_tick(p, ms);
    auth_tick(p, ms);
    keepalive_tick(p, ms);
    run_pending(p);
}

void ppp_get_status(const ppp_t *p, ppp_status_t *st)
{
    int lcp_open = p->lcp.state == PPP_FSM_OPENED;

    memset(st, 0, sizeof *st);
    st->phase = p->phase;
    st->end_reason = p->end_reason;
    st->lcp_state = p->lcp.state;
    st->ipcp_state = p->ipcp.state;
    st->ccp_state = p->ccp.state;
    st->comp_tx = p->tx_alg;
    st->comp_rx = p->rx_alg;
    st->auth = p->auth_proto;
    st->authenticated = p->auth_done && p->auth_ok;
    memcpy(st->peer_name, p->peer_name, sizeof st->peer_name);
    st->ip_up = p->phase == PPP_PHASE_NETWORK && p->ipcp.state == PPP_FSM_OPENED;
    st->our_ip = p->our_ip;
    st->peer_ip = st->ip_up ? p->peer_ip : 0;
    st->dns1 = p->cfg.dns1;
    st->dns2 = p->cfg.dns2;
    st->our_mru = lcp_open ? p->mru : PPP_MAX_MRU;
    st->peer_mru = p->peer_mru;
    st->tx_accm = p->tx_accm;
    st->rx_accm = p->rx.rx_accm;
    st->our_magic = p->w_magic ? p->magic : 0;
    st->peer_magic = p->peer_magic;
    st->counters = p->cnt;
    st->counters.rx_frames = p->rx.frames_ok;
    st->counters.rx_bad_fcs = p->rx.bad_fcs;
    st->counters.rx_runts = p->rx.runts;
    st->counters.rx_overlong = p->rx.overlong;
    st->counters.rx_aborts = p->rx.aborts;
}
