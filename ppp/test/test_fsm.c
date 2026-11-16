/* Automaton unit test: every cell of the RFC 1661 4.1 state transition
 * table, then restart timer/counter behaviour and packet validation. */
#include <string.h>
#include "test.h"
#include "ppp_fsm.h"

/* The table, transcribed from RFC 1661 section 4.1. Rows are events,
 * columns states 0..9. "-" marks an illegal transition (we expect it to be
 * ignored). Footnote letters (r, p, x) are dropped. */
enum { EV_UP, EV_DOWN, EV_OPEN, EV_CLOSE, EV_TOP, EV_TOM, EV_RCRP, EV_RCRM,
       EV_RCA, EV_RCN, EV_RTR, EV_RTA, EV_RUC, EV_RXJP, EV_RXJM, EV_RXR, EV_N };
static const char *ev_name[EV_N] = { "Up", "Down", "Open", "Close", "TO+", "TO-",
    "RCR+", "RCR-", "RCA", "RCN", "RTR", "RTA", "RUC", "RXJ+", "RXJ-", "RXR" };

static const char *table[EV_N][10] = {
/*          0        1            2             3                 4        5        6             7            8             9 */
/* Up   */ {"2",     "irc,scr/6", "-",          "-",              "-",     "-",     "-",          "-",         "-",          "-"},
/* Down */ {"-",     "-",         "0",          "tls/1",          "0",     "1",     "1",          "1",         "1",          "tld/1"},
/* Open */ {"tls/1", "1",         "irc,scr/6",  "3",              "5",     "5",     "6",          "7",         "8",          "9"},
/* Close*/ {"0",     "tlf/0",     "2",          "2",              "4",     "4",     "irc,str/4",  "irc,str/4", "irc,str/4",  "tld,irc,str/4"},
/* TO+  */ {"-",     "-",         "-",          "-",              "str/4", "str/5", "scr/6",      "scr/6",     "scr/8",      "-"},
/* TO-  */ {"-",     "-",         "-",          "-",              "tlf/2", "tlf/3", "tlf/3",      "tlf/3",     "tlf/3",      "-"},
/* RCR+ */ {"-",     "-",         "sta/2",      "irc,scr,sca/8",  "4",     "5",     "sca/8",      "sca,tlu/9", "sca/8",      "tld,scr,sca/8"},
/* RCR- */ {"-",     "-",         "sta/2",      "irc,scr,scn/6",  "4",     "5",     "scn/6",      "scn/7",     "scn/6",      "tld,scr,scn/6"},
/* RCA  */ {"-",     "-",         "sta/2",      "sta/3",          "4",     "5",     "irc/7",      "scr/6",     "irc,tlu/9",  "tld,scr/6"},
/* RCN  */ {"-",     "-",         "sta/2",      "sta/3",          "4",     "5",     "irc,scr/6",  "scr/6",     "irc,scr/8",  "tld,scr/6"},
/* RTR  */ {"-",     "-",         "sta/2",      "sta/3",          "sta/4", "sta/5", "sta/6",      "sta/6",     "sta/6",      "tld,zrc,sta/5"},
/* RTA  */ {"-",     "-",         "2",          "3",              "tlf/2", "tlf/3", "6",          "6",         "8",          "tld,scr/6"},
/* RUC  */ {"-",     "-",         "scj/2",      "scj/3",          "scj/4", "scj/5", "scj/6",      "scj/7",     "scj/8",      "scj/9"},
/* RXJ+ */ {"-",     "-",         "2",          "3",              "4",     "5",     "6",          "6",         "8",          "9"},
/* RXJ- */ {"-",     "-",         "tlf/2",      "tlf/3",          "tlf/2", "tlf/3", "tlf/3",      "tlf/3",     "tlf/3",      "tld,irc,str/5"},
/* RXR  */ {"-",     "-",         "2",          "3",              "4",     "5",     "6",          "7",         "8",          "ser/9"},
};

/* ---- mock protocol: logs every action as a token ---- */
static char logbuf[128];
static uint8_t last_code, last_id, last_data[64];
static size_t last_len;
static int n_sent;

static void lg(const char *tok)
{
    if (logbuf[0]) strcat(logbuf, ",");
    strcat(logbuf, tok);
}

static void m_send(ppp_fsm_t *f, uint8_t code, uint8_t id,
                   const uint8_t *a, size_t alen, const uint8_t *b, size_t blen)
{
    static const char *nm[] = { "?", "scr", "sca", "scn", "scn", "str", "sta", "scj" };
    (void)f;
    lg(code <= 7 ? nm[code] : code == 10 ? "ser" : "s??");
    last_code = code;
    last_id = id;
    last_len = 0;
    if (alen + blen <= sizeof last_data) {
        if (alen) memcpy(last_data, a, alen);
        if (blen) memcpy(last_data + alen, b, blen);
        last_len = alen + blen;
    }
    n_sent++;
}
static void m_reset(ppp_fsm_t *f) { (void)f; }
static size_t m_build(ppp_fsm_t *f, uint8_t *o, size_t max)
{
    (void)f; (void)max;
    o[0] = 1; o[1] = 4; o[2] = 0x05; o[3] = 0xdc;
    return 4;
}
/* option value octet 'G' is good, anything else is Nak'd */
static int m_rcr(ppp_fsm_t *f, const uint8_t *o, size_t len,
                 uint8_t *out, size_t max, size_t *outlen)
{
    (void)f; (void)max;
    if (len >= 3 && o[2] == 'G') return PPP_CONF_ACK;
    out[0] = 9; out[1] = 3; out[2] = 'G';
    *outlen = 3;
    return PPP_CONF_NAK;
}
static int acks, naks, rejs;
static void m_ack(ppp_fsm_t *f, const uint8_t *o, size_t n) { (void)f; (void)o; (void)n; acks++; }
static void m_nak(ppp_fsm_t *f, const uint8_t *o, size_t n) { (void)f; (void)o; (void)n; naks++; }
static void m_rej(ppp_fsm_t *f, const uint8_t *o, size_t n) { (void)f; (void)o; (void)n; rejs++; }
static void m_up(ppp_fsm_t *f) { CHECK_EQ_I(f->state, PPP_FSM_OPENED); lg("tlu"); }
static void m_down(ppp_fsm_t *f) { CHECK(f->state != PPP_FSM_OPENED); lg("tld"); }
static void m_started(ppp_fsm_t *f) { (void)f; lg("tls"); }
static void m_finished(ppp_fsm_t *f) { (void)f; lg("tlf"); }
static int m_ext(ppp_fsm_t *f, uint8_t code, uint8_t id, const uint8_t *d, size_t n)
{
    if (code == 9) {                 /* echo request */
        if (f->state == PPP_FSM_OPENED)
            f->ops->send(f, 10, id, d, n, NULL, 0);
        return PPP_FSM_EXT_RXR;
    }
    return PPP_FSM_EXT_RUC;
}
static const ppp_fsm_ops_t mock = {
    m_send, m_reset, m_build, m_rcr, m_ack, m_nak, m_rej,
    m_up, m_down, m_started, m_finished, m_ext
};

static uint8_t scratch[256];
static ppp_fsm_t F;

/* put the automaton in a given state as if it had got there normally */
static void force(int state)
{
    ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
    F.state = (uint8_t)state;
    F.req_id = 0x42;
    F.next_id = 0x43;
    F.restart_count = 3;
    F.timer_on = (state >= PPP_FSM_CLOSING && state <= PPP_FSM_ACK_SENT);
    F.timer_left = F.restart_ms;
    logbuf[0] = 0;
    n_sent = 0;
}

static void pkt(uint8_t code, uint8_t id, const uint8_t *data, size_t n)
{
    uint8_t p[80];
    p[0] = code; p[1] = id; p[2] = 0; p[3] = (uint8_t)(4 + n);
    if (n) memcpy(p + 4, data, n);
    ppp_fsm_input(&F, p, 4 + n);
}

static void inject(int ev)
{
    static const uint8_t good[] = { 9, 3, 'G' }, bad[] = { 9, 3, 'B' };
    static const uint8_t mine[] = { 1, 4, 0x05, 0xdc };
    static const uint8_t rej_ext[] = { 0x77, 1, 0, 4 };   /* reject of an extension code */
    static const uint8_t rej_cr[] = { 1, 1, 0, 4 };       /* reject of Configure-Request */
    switch (ev) {
    case EV_UP:    ppp_fsm_up(&F); break;
    case EV_DOWN:  ppp_fsm_down(&F); break;
    case EV_OPEN:  ppp_fsm_open(&F); break;
    case EV_CLOSE: ppp_fsm_close(&F); break;
    case EV_TOP:   F.restart_count = 3; ppp_fsm_tick(&F, F.restart_ms); break;
    case EV_TOM:   F.restart_count = 0; ppp_fsm_tick(&F, F.restart_ms); break;
    case EV_RCRP:  pkt(PPP_CONF_REQ, 7, good, 3); break;
    case EV_RCRM:  pkt(PPP_CONF_REQ, 7, bad, 3); break;
    case EV_RCA:   pkt(PPP_CONF_ACK, 0x42, mine, 4); break;
    case EV_RCN:   pkt(PPP_CONF_NAK, 0x42, mine, 4); break;
    case EV_RTR:   pkt(PPP_TERM_REQ, 9, NULL, 0); break;
    case EV_RTA:   pkt(PPP_TERM_ACK, 9, NULL, 0); break;
    case EV_RUC:   pkt(0x77, 9, NULL, 0); break;
    case EV_RXJP:  pkt(PPP_CODE_REJ, 9, rej_ext, 4); break;
    case EV_RXJM:  pkt(PPP_CODE_REJ, 9, rej_cr, 4); break;
    case EV_RXR:   pkt(9, 9, mine, 4); break;
    }
}

static int has(const char *list, const char *tok)
{
    const char *p = list;
    size_t n = strlen(tok);
    while ((p = strstr(p, tok)) != NULL) {
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',' || p[n] == '/'))
            return 1;
        p += n;
    }
    return 0;
}

static int count_tokens(const char *s)
{
    int n = (*s && *s != '/') ? 1 : 0;
    for (; *s && *s != '/'; s++) if (*s == ',') n++;
    return n;
}

static void check_cell(int ev, int st)
{
    static const char *acts[] = { "tlu", "tld", "tls", "tlf", "scr", "sca", "scn",
                                  "str", "sta", "scj", "ser" };
    const char *cell = table[ev][st];
    const char *slash = strchr(cell, '/');
    int want_state, n_want = 0, ok = 1;
    int timer_states;

    force(st);
    inject(ev);

    if (cell[0] == '-') {
        /* illegal: nothing may happen */
        if (F.state != st || logbuf[0]) ok = 0;
    } else {
        want_state = slash ? slash[1] - '0' : cell[0] - '0';
        if (F.state != want_state) ok = 0;
        for (size_t i = 0; i < sizeof acts / sizeof acts[0]; i++) {
            int w = slash ? has(cell, acts[i]) : 0;
            if (w) n_want++;
            if (w != has(logbuf, acts[i])) ok = 0;
        }
        if (count_tokens(logbuf) != n_want) ok = 0;
        /* counters: irc loads Max-Configure or Max-Terminate, each
         * scr/str takes one off; zrc zeroes it and starts the timer */
        if (slash && has(cell, "irc")) {
            int c = has(cell, "str") ? F.max_terminate - 1
                  : has(cell, "scr") ? F.max_configure - 1 : F.max_configure;
            if (F.restart_count != c) ok = 0;
        }
        if (slash && has(cell, "zrc")) {
            if (F.restart_count != 0 || !F.timer_on) ok = 0;
        }
        if (slash && !has(cell, "irc") && (has(cell, "scr") || has(cell, "str"))) {
            if (F.restart_count != 2) ok = 0;   /* force() and TO+ start at 3 */
        }
        /* any scr/str/zrc restarts the timer at its full period */
        if (slash && (has(cell, "scr") || has(cell, "str") || has(cell, "zrc"))) {
            if (!F.timer_on || F.timer_left != F.restart_ms) ok = 0;
        }
        /* the timer runs exactly in states 4..8 */
        timer_states = (F.state >= PPP_FSM_CLOSING && F.state <= PPP_FSM_ACK_SENT);
        if ((F.timer_on != 0) != timer_states) ok = 0;
    }
    t_checks++;
    if (!ok) {
        t_fails++;
        printf("  FAIL cell [%s x state %d]: want \"%s\", got \"%s\" -> state %d (rc=%d timer=%d)\n",
               ev_name[ev], st, cell, logbuf, F.state, F.restart_count, F.timer_on);
    }
}

static void test_table(void)
{
    for (int ev = 0; ev < EV_N; ev++)
        for (int st = 0; st < 10; st++)
            check_cell(ev, st);
}

/* walk a normal open from scratch and look at the packets */
static void test_normal_open(void)
{
    static const uint8_t good[] = { 9, 3, 'G' };
    static const uint8_t mine[] = { 1, 4, 0x05, 0xdc };

    ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
    logbuf[0] = 0; acks = naks = rejs = 0;
    CHECK_EQ_I(F.state, PPP_FSM_INITIAL);
    CHECK_EQ_I(F.restart_ms, 3000);
    CHECK_EQ_I(F.max_configure, 10);
    CHECK_EQ_I(F.max_terminate, 2);
    CHECK_EQ_I(F.max_failure, 5);
    ppp_fsm_open(&F);
    ppp_fsm_up(&F);
    CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);
    CHECK_EQ_I(last_code, PPP_CONF_REQ);
    CHECK(last_len == 4 && memcmp(last_data, mine, 4) == 0);
    CHECK_EQ_I(last_id, F.req_id);

    /* an Ack with the wrong identifier is silently discarded */
    pkt(PPP_CONF_ACK, (uint8_t)(F.req_id + 1), mine, 4);
    CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);
    CHECK_EQ_I(acks, 0);
    /* so is an Ack whose options differ from what we requested */
    pkt(PPP_CONF_ACK, F.req_id, good, 3);
    CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);
    CHECK_EQ_I(acks, 0);
    /* and a Nak with the wrong identifier */
    pkt(PPP_CONF_NAK, (uint8_t)(F.req_id + 1), mine, 4);
    CHECK_EQ_I(naks, 0);

    pkt(PPP_CONF_ACK, F.req_id, mine, 4);
    CHECK_EQ_I(F.state, PPP_FSM_ACK_RCVD);
    CHECK_EQ_I(acks, 1);

    /* peer's request: the Ack echoes its identifier and options */
    pkt(PPP_CONF_REQ, 0x99, good, 3);
    CHECK_EQ_I(F.state, PPP_FSM_OPENED);
    CHECK_EQ_I(last_code, PPP_CONF_ACK);
    CHECK_EQ_I(last_id, 0x99);
    CHECK(last_len == 3 && memcmp(last_data, good, 3) == 0);
    CHECK(has(logbuf, "tlu"));

    /* Terminate-Request in Opened: Ack carries the same identifier */
    pkt(PPP_TERM_REQ, 0x31, NULL, 0);
    CHECK_EQ_I(last_code, PPP_TERM_ACK);
    CHECK_EQ_I(last_id, 0x31);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPING);
    /* zrc: one restart period later the layer is finished */
    logbuf[0] = 0;
    ppp_fsm_tick(&F, 2999);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPING);
    ppp_fsm_tick(&F, 1);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPED);
    CHECK(strcmp(logbuf, "tlf") == 0);
}

static void test_nak_rej(void)
{
    static const uint8_t bad[] = { 9, 3, 'B' };
    static const uint8_t mine[] = { 1, 4, 0x05, 0xdc };
    static const uint8_t want_nak[] = { 9, 3, 'G' };
    uint8_t id;

    ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
    acks = naks = rejs = 0;
    ppp_fsm_open(&F);
    ppp_fsm_up(&F);
    pkt(PPP_CONF_REQ, 5, bad, 3);
    CHECK_EQ_I(last_code, PPP_CONF_NAK);
    CHECK_EQ_I(last_id, 5);
    CHECK(last_len == 3 && memcmp(last_data, want_nak, 3) == 0);
    CHECK_EQ_I(F.failures, 1);
    pkt(PPP_CONF_REQ, 6, bad, 3);
    CHECK_EQ_I(F.failures, 2);

    /* Nak / Reject of our request reach the protocol, and the new request
     * carries a new identifier */
    id = F.req_id;
    pkt(PPP_CONF_NAK, id, mine, 4);
    CHECK_EQ_I(naks, 1);
    CHECK_EQ_I(last_code, PPP_CONF_REQ);
    CHECK(F.req_id != id);
    id = F.req_id;
    pkt(PPP_CONF_REJ, id, mine, 4);
    CHECK_EQ_I(rejs, 1);
    CHECK(F.req_id != id);
    /* a stale Reject (old identifier) is ignored */
    pkt(PPP_CONF_REJ, id, mine, 4);
    CHECK_EQ_I(rejs, 1);
}

static void test_retransmit(void)
{
    int i;

    /* Req-Sent: Max-Configure requests in total, then give up */
    ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
    F.restart_ms = 1000;
    F.max_configure = 4;
    n_sent = 0; logbuf[0] = 0;
    ppp_fsm_open(&F);
    ppp_fsm_up(&F);
    CHECK_EQ_I(n_sent, 1);
    for (i = 0; i < 3; i++) {
        ppp_fsm_tick(&F, 999);
        CHECK_EQ_I(n_sent, 1 + i);
        ppp_fsm_tick(&F, 1);
        CHECK_EQ_I(n_sent, 2 + i);
        CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);
    }
    ppp_fsm_tick(&F, 1000);
    CHECK_EQ_I(n_sent, 4);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPED);
    CHECK(has(logbuf, "tlf"));
    CHECK_EQ_I(F.timer_on, 0);
    /* nothing more ever happens */
    ppp_fsm_tick(&F, 100000);
    CHECK_EQ_I(n_sent, 4);

    /* A retransmitted Configure-Request keeps its identifier (RFC 1661 5.1:
     * it MAY remain unchanged while the options do), so that on a slow line
     * the Ack to the first copy still counts after the timer has run out. */
    {
        static const uint8_t mine[] = { 1, 4, 0x05, 0xdc };
        uint8_t first;
        ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
        F.restart_ms = 1000;
        n_sent = 0; acks = 0;
        ppp_fsm_open(&F);
        ppp_fsm_up(&F);
        first = last_id;
        ppp_fsm_tick(&F, 1000);
        CHECK_EQ_I(n_sent, 2);
        CHECK_EQ_I(last_code, PPP_CONF_REQ);
        CHECK_EQ_I(last_id, first);
        ppp_fsm_tick(&F, 1000);
        CHECK_EQ_I(n_sent, 3);
        CHECK_EQ_I(last_id, first);
        pkt(PPP_CONF_ACK, first, mine, 4);          /* the late Ack */
        CHECK_EQ_I(acks, 1);
        CHECK_EQ_I(F.state, PPP_FSM_ACK_RCVD);
        /* a request sent for any other reason gets a new identifier */
        ppp_fsm_tick(&F, 1000);                     /* Ack-Rcvd timeout: back to Req-Sent */
        CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);
        CHECK(last_id != first);
    }

    /* one big tick must not fire more than one timeout */
    ppp_fsm_init(&F, &mock, NULL, scratch, sizeof scratch);
    n_sent = 0;
    ppp_fsm_open(&F);
    ppp_fsm_up(&F);
    ppp_fsm_tick(&F, 50000);
    CHECK_EQ_I(n_sent, 2);

    /* Closing: Max-Terminate Terminate-Requests, then Closed */
    force(PPP_FSM_OPENED);
    ppp_fsm_close(&F);
    CHECK_EQ_I(F.state, PPP_FSM_CLOSING);
    CHECK_EQ_I(last_code, PPP_TERM_REQ);
    CHECK_EQ_I(n_sent, 1);
    ppp_fsm_tick(&F, 3000);
    CHECK_EQ_I(n_sent, 2);
    CHECK_EQ_I(F.state, PPP_FSM_CLOSING);
    ppp_fsm_tick(&F, 3000);
    CHECK_EQ_I(n_sent, 2);
    CHECK_EQ_I(F.state, PPP_FSM_CLOSED);
}

static void test_validation(void)
{
    uint8_t p[80];
    static const uint8_t trunc_opt[] = { 9, 5, 'G' };       /* option overruns */
    static const uint8_t zero_opt[] = { 9, 0, 'G' };        /* option length < 2 */

    /* too short, or length field larger than the packet: dropped */
    force(PPP_FSM_REQ_SENT);
    p[0] = PPP_TERM_REQ; p[1] = 1; p[2] = 0; p[3] = 4;
    ppp_fsm_input(&F, p, 3);
    CHECK_EQ_I(n_sent, 0);
    p[3] = 10;
    ppp_fsm_input(&F, p, 4);
    CHECK_EQ_I(n_sent, 0);
    p[3] = 3;
    ppp_fsm_input(&F, p, 4);
    CHECK_EQ_I(n_sent, 0);
    /* padding beyond the length field is ignored, packet accepted */
    p[3] = 4;
    ppp_fsm_input(&F, p, 20);
    CHECK_EQ_I(n_sent, 1);
    CHECK_EQ_I(last_code, PPP_TERM_ACK);

    /* malformed option lists in a Configure-Request: dropped */
    force(PPP_FSM_REQ_SENT);
    pkt(PPP_CONF_REQ, 1, trunc_opt, 3);
    pkt(PPP_CONF_REQ, 1, zero_opt, 3);
    CHECK_EQ_I(n_sent, 0);
    CHECK_EQ_I(F.state, PPP_FSM_REQ_SENT);

    /* Code-Reject carries the offending packet back */
    force(PPP_FSM_OPENED);
    {
        const uint8_t body[] = { 0xde, 0xad };
        const uint8_t want[] = { 0x77, 0x09, 0x00, 0x06, 0xde, 0xad };
        pkt(0x77, 9, body, 2);
        CHECK_EQ_I(last_code, PPP_CODE_REJ);
        CHECK(last_len == 6 && memcmp(last_data, want, 6) == 0);
    }
    /* a Code-Reject of an extension code is remembered */
    {
        const uint8_t rej9[] = { 9, 1, 0, 8, 0, 0, 0, 0 };
        pkt(PPP_CODE_REJ, 1, rej9, 8);
        CHECK(F.rejected_codes & (1u << 9));
        CHECK_EQ_I(F.state, PPP_FSM_OPENED);
    }
    /* Protocol-Reject of the whole protocol is RXJ- */
    force(PPP_FSM_OPENED);
    ppp_fsm_protocol_rejected(&F);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPING);
    force(PPP_FSM_REQ_SENT);
    ppp_fsm_protocol_rejected(&F);
    CHECK_EQ_I(F.state, PPP_FSM_STOPPED);

    /* option walker */
    {
        const uint8_t o[] = { 1, 4, 0x05, 0xdc, 7, 2, 5, 6, 1, 2, 3, 4 };
        size_t pos = 0, vlen;
        uint8_t type;
        const uint8_t *val;
        CHECK(ppp_fsm_options_ok(o, sizeof o));
        CHECK(!ppp_fsm_options_ok(o, sizeof o - 1));
        CHECK(ppp_fsm_options_ok(o, 0));
        CHECK(ppp_fsm_next_option(o, sizeof o, &pos, &type, &val, &vlen));
        CHECK(type == 1 && vlen == 2 && val[0] == 0x05);
        CHECK(ppp_fsm_next_option(o, sizeof o, &pos, &type, &val, &vlen));
        CHECK(type == 7 && vlen == 0);
        CHECK(ppp_fsm_next_option(o, sizeof o, &pos, &type, &val, &vlen));
        CHECK(type == 5 && vlen == 4 && val[3] == 4);
        CHECK(!ppp_fsm_next_option(o, sizeof o, &pos, &type, &val, &vlen));
    }
}

int main(void)
{
    test_table();
    test_normal_open();
    test_nak_rej();
    test_retransmit();
    test_validation();
    T_REPORT("test_fsm");
}
