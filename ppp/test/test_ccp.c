/* CCP (RFC 1962) and the compressed data path, session level: the server
 * is driven by a scripted peer with hand-built frames, and what it sends
 * is checked octet by octet. The peer's side of the data is done with the
 * coders of ppp_deflate.c and ppp_mppc.c, which test_deflate, test_mppc
 * and zlib_check.py verify on their own. */
#include <string.h>
#include "test.h"
#include "ppp.h"
#include "ppp_md5.h"
#include "gen.h"

#define OUR_IP   PPP_IP4(10, 0, 0, 1)
#define PEER_IP  PPP_IP4(10, 0, 0, 2)
#define P_CCP    PPP_PROTO_CCP
#define P_COMP   PPP_PROTO_COMP
#define RESET_REQ 14
#define RESET_ACK 15

static ppp_t S;

/* MPPE keys injected after ppp_init (this harness authenticates with PAP,
 * which does not derive them; MS-CHAPv2 would). inject_mppe turns it on. */
static int inject_mppe;
static uint8_t inj_s[16], inj_r[16];

/* test data: IP packets with a moving 40-octet header in front of the
 * payload; the random kind is random throughout (it must not compress) */
static uint32_t pk_seq;
#define gen_init(g, seed) ((g)->s = (seed))
#define gen_next(g) (gen_u32(g) >> 8)
static void mk(gen_t *g, int kind, uint8_t *p, size_t n)
{
    if (n >= 40 && kind != GEN_RANDOM) {
        gen_packet(g, kind, pk_seq, p, n);
        pk_seq += (uint32_t)n;
    } else {
        gen_fill(g, kind, p, n);
    }
}

/* ---- capture of what the server transmits ---- */
typedef struct {
    uint16_t proto;
    size_t   n;
    uint8_t  d[1600];
} frame_t;
#define MAXF 24
static frame_t out[MAXF];
static int n_out;
static ppp_hdlc_rx_t dec;
static uint8_t wire[PPP_TX_BUF];
static size_t wire_n;
static frame_t scr;                 /* the server's latest CCP Configure-Request */
static int scr_new;

static void pull(void)
{
    n_out = 0;
    wire_n = ppp_output_bytes(&S, wire, sizeof wire);
    for (size_t i = 0; i < wire_n; i++) {
        if (ppp_hdlc_rx_byte(&dec, wire[i]) && n_out < MAXF) {
            frame_t *f = &out[n_out];
            size_t off;
            if (ppp_hdlc_parse(dec.buf, dec.len, 1, 1, &f->proto, &off) == 0) {
                f->n = dec.len - off;
                memcpy(f->d, dec.buf + off, f->n);
                if (f->proto == P_CCP && f->d[0] == PPP_CONF_REQ) {
                    scr = *f;
                    scr_new = 1;
                }
                n_out++;
            }
        }
    }
}

static frame_t *find(uint16_t proto, uint8_t code)
{
    for (int i = 0; i < n_out; i++)
        if (out[i].proto == proto && (code == 0 || out[i].d[0] == code))
            return &out[i];
    return NULL;
}

static int count(uint16_t proto)
{
    int k = 0;
    for (int i = 0; i < n_out; i++)
        if (out[i].proto == proto) k++;
    return k;
}

/* ---- injection ---- */
static void push(uint16_t proto, const uint8_t *d, size_t n)
{
    static uint8_t w[3400];
    uint8_t h[4] = { 0xff, 0x03, (uint8_t)(proto >> 8), (uint8_t)proto };
    size_t k = ppp_hdlc_encode(w, sizeof w, h, 4, d, n, 0xffffffffu, 1);
    ppp_input_bytes(&S, w, k);
}

static void push2(uint16_t proto, const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    static uint8_t t[1700];
    memcpy(t, a, an);
    memcpy(t + an, b, bn);
    push(proto, t, an + bn);
}

static void push_cp(uint16_t proto, uint8_t code, uint8_t id, const uint8_t *d, size_t n)
{
    uint8_t p[1600];
    p[0] = code; p[1] = id; p[2] = (uint8_t)((n + 4) >> 8); p[3] = (uint8_t)(n + 4);
    if (n) memcpy(p + 4, d, n);
    push(proto, p, n + 4);
}

static uint8_t ip_got[1600];
static size_t ip_len;
static int ip_count;
static void on_ip(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    ip_count++;
    ip_len = len;
    memcpy(ip_got, pkt, len);
}

static const ppp_user_t users[] = { { "alice", "wonderland" } };

static void base_cfg(ppp_config_t *c)
{
    ppp_config_default(c);
    c->auth = PPP_AUTH_PAP;
    c->users = users;
    c->n_users = 1;
    c->our_ip = OUR_IP;
    c->peer_ip = PEER_IP;
    c->seed = 0x1234abcd;
    c->ip_rx = on_ip;
}

static ppp_status_t st;
static ppp_status_t *status(void)
{
    ppp_get_status(&S, &st);
    return &st;
}

/* LCP, PAP and IPCP by hand; the server's first CCP request is left in scr */
static void up_mru(const ppp_config_t *c, unsigned mru)
{
    static const uint8_t pap[] = { 5, 'a','l','i','c','e', 10, 'w','o','n','d','e','r','l','a','n','d' };
    static const uint8_t ip[] = { 3, 6, 10, 0, 0, 2 };
    uint8_t lcp[10] = { 5, 6, 0x11, 0x22, 0x33, 0x44, 1, 4, 0, 0 };
    frame_t *cr;

    ppp_init(&S, c);
    if (inject_mppe) {
        S.mppe_have = 1;
        memcpy(S.mppe_send, inj_s, 16);
        memcpy(S.mppe_recv, inj_r, 16);
    }
    ppp_hdlc_rx_init(&dec, PPP_HDLC_BUF);
    dec.rx_accm = 0;
    ip_count = 0;
    scr_new = 0;
    ppp_lower_up(&S);
    pull();
    cr = find(PPP_PROTO_LCP, PPP_CONF_REQ);
    CHECK(cr != NULL);
    if (!cr) return;
    push_cp(PPP_PROTO_LCP, PPP_CONF_ACK, cr->d[1], cr->d + 4, cr->n - 4);
    lcp[8] = (uint8_t)(mru >> 8); lcp[9] = (uint8_t)mru;
    push_cp(PPP_PROTO_LCP, PPP_CONF_REQ, 1, lcp, mru ? 10 : 6);
    push_cp(PPP_PROTO_PAP, 1, 1, pap, sizeof pap);
    pull();
    CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
    cr = find(PPP_PROTO_IPCP, PPP_CONF_REQ);
    CHECK(cr != NULL);
    if (!cr) return;
    push_cp(PPP_PROTO_IPCP, PPP_CONF_ACK, cr->d[1], cr->d + 4, cr->n - 4);
    push_cp(PPP_PROTO_IPCP, PPP_CONF_REQ, 1, ip, sizeof ip);
    pull();
    CHECK_EQ_I(status()->ip_up, 1);
}

static void up(const ppp_config_t *c) { up_mru(c, 0); }

/* ================================================================== */
/* negotiation, table-driven                                           */

#define O26(w)  26, 4, (uint8_t)((((w) - 8) << 4) | 8), 0
#define O24(w)  24, 4, (uint8_t)((((w) - 8) << 4) | 8), 0
#define OBSD    21, 3, 0x2f
#define OMPPC   18, 6, 0, 0, 0, 1
#define OMPPE   18, 6, 0x01, 0, 0, 0x61   /* MPPC, 40 and 128 bit keys, stateless */

typedef struct {
    char    who;            /* 'P': the peer requests; 'S': the server's request is answered */
    uint8_t n;              /* 'P': options the peer sends; 'S': options the server must have sent */
    uint8_t opts[16];
    uint8_t code;           /* 'P': the reply expected; 'S': the reply given */
    uint8_t rn;             /* its options ('S' with Ack: the request is echoed) */
    uint8_t ropts[16];
} step_t;

typedef struct {
    const char *name;
    unsigned comp;          /* cfg.comp */
    uint8_t  wbits;         /* cfg.deflate_wbits, 0 = default */
    step_t   steps[10];
    unsigned tx, rx;        /* in force at the end */
    unsigned tx_w, rx_w;
    int      opened;
} case_t;

#define BOTH (PPP_COMP_DEFLATE | PPP_COMP_MPPC)

static const case_t cases[] = {
    { "pppd: deflate 15, deflate-draft 15, bsd", BOTH, 0, {
        { 'P', 11, { O26(15), O24(15), OBSD }, PPP_CONF_REJ, 7, { O24(15), OBSD } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 4, { O26(15) }, PPP_CONF_ACK, 4, { O26(15) } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15, 1 },
    { "pppd, the other order on the wire", BOTH, 0, {
        { 'P', 11, { OBSD, O24(15), O26(15) }, PPP_CONF_REJ, 7, { OBSD, O24(15) } },
        { 'P', 4, { O26(15) }, PPP_CONF_ACK, 4, { O26(15) } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15, 1 },
    { "old pppd: the draft number only", BOTH, 0, {
        { 'P', 7, { O24(15), OBSD }, PPP_CONF_REJ, 3, { OBSD } },
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'S', 4, { O24(15) }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 4, { O24(15) }, PPP_CONF_ACK, 4, { O24(15) } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15, 1 },
    { "pppd with a small window each way", BOTH, 0, {
        { 'P', 4, { O26(9) }, PPP_CONF_ACK, 4, { O26(9) } },
        { 'S', 4, { O26(15) }, PPP_CONF_NAK, 4, { O26(12) } },
        { 'S', 4, { O26(12) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 9, 12, 1 },
    { "window 8 is taken as offered", BOTH, 0, {
        { 'P', 4, { O26(8) }, PPP_CONF_ACK, 4, { O26(8) } },
        { 'S', 4, { O26(15) }, PPP_CONF_NAK, 4, { O26(8) } },
        { 'S', 4, { O26(8) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 8, 8, 1 },
    { "our window limited by configuration", BOTH, 12, {
        { 'S', 4, { O26(12) }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 4, { O26(15) }, PPP_CONF_ACK, 4, { O26(15) } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 12, 1 },
    { "a Nak for more window than we have", BOTH, 12, {
        { 'S', 4, { O26(12) }, PPP_CONF_NAK, 4, { O26(15) } },
        { 'S', 4, { O24(12) }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_NONE, PPP_COMP_DEFLATE, 0, 12, 1 },
    { "Deflate options that are not valid get a Nak", BOTH, 0, {
        { 'P', 4, { 26, 4, 0x79, 0 }, PPP_CONF_NAK, 4, { O26(15) } },      /* method 9 */
        { 'P', 4, { 26, 4, 0x58, 1 }, PPP_CONF_NAK, 4, { O26(13) } },      /* check method 1 */
        { 'P', 3, { 26, 3, 0x78 }, PPP_CONF_NAK, 4, { O26(15) } },         /* "Length 3" of the RFC's text */
        { 'P', 4, { O26(15) }, PPP_CONF_ACK, 4, { O26(15) } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15, 1 },
    { "Windows: MPPC only", BOTH, 0, {
        { 'P', 6, { OMPPC }, PPP_CONF_ACK, 6, { OMPPC } },
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'S', 4, { O24(15) }, PPP_CONF_REJ, 4, { O24(15) } },
        { 'S', 6, { OMPPC }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0, 1 },
    { "Windows: MPPC with MPPE bits, Nak down to MPPC", BOTH, 0, {
        { 'P', 6, { OMPPE }, PPP_CONF_NAK, 6, { OMPPC } },
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'P', 6, { OMPPC }, PPP_CONF_ACK, 6, { OMPPC } },
        { 'S', 4, { O24(15) }, PPP_CONF_REJ, 4, { O24(15) } },
        { 'S', 6, { OMPPC }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0, 1 },
    { "a peer that insists on encryption towards it", BOTH, 0, {
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'S', 4, { O24(15) }, PPP_CONF_REJ, 4, { O24(15) } },
        { 'S', 6, { OMPPC }, PPP_CONF_NAK, 6, { OMPPE } },
        { 'S', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 6, { OMPPC }, PPP_CONF_ACK, 6, { OMPPC } },
      }, PPP_COMP_MPPC, PPP_COMP_NONE, 0, 0, 1 },
    { "Deflate before MPPC when both are offered", BOTH, 0, {
        { 'P', 10, { OMPPC, O26(15) }, PPP_CONF_REJ, 6, { OMPPC } },
        { 'P', 4, { O26(15) }, PPP_CONF_ACK, 4, { O26(15) } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_DEFLATE, PPP_COMP_DEFLATE, 15, 15, 1 },
    { "the two directions differ", BOTH, 0, {
        { 'P', 6, { OMPPC }, PPP_CONF_ACK, 6, { OMPPC } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_MPPC, PPP_COMP_DEFLATE, 0, 15, 1 },
    { "options nobody here knows", BOTH, 0, {
        { 'P', 9, { 99, 2, 17, 5, 1, 2, 3, 1, 2 }, PPP_CONF_REJ, 9, { 99, 2, 17, 5, 1, 2, 3, 1, 2 } },
        { 'P', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
        { 'S', 4, { O26(15) }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_NONE, PPP_COMP_DEFLATE, 0, 15, 1 },
    { "MPPC with a wrong length is not MPPC", BOTH, 0, {
        { 'P', 5, { 18, 5, 0, 0, 1 }, PPP_CONF_REJ, 5, { 18, 5, 0, 0, 1 } },
      }, PPP_COMP_NONE, PPP_COMP_NONE, 0, 0, 0 },
    { "the peer rejects everything of ours", BOTH, 0, {
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'S', 4, { O24(15) }, PPP_CONF_REJ, 4, { O24(15) } },
        { 'S', 6, { OMPPC }, PPP_CONF_REJ, 6, { OMPPC } },
        { 'S', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_NONE, PPP_COMP_NONE, 0, 0, 1 },
    { "a Nak that names another algorithm", BOTH, 0, {
        { 'S', 4, { O26(15) }, PPP_CONF_NAK, 6, { OMPPC } },
        { 'S', 4, { O24(15) }, PPP_CONF_NAK, 6, { OMPPC } },
        { 'S', 6, { OMPPC }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_NONE, PPP_COMP_MPPC, 0, 0, 1 },
    { "configured for Deflate only", PPP_COMP_DEFLATE, 0, {
        { 'P', 6, { OMPPC }, PPP_CONF_REJ, 6, { OMPPC } },
        { 'S', 4, { O26(15) }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'S', 4, { O24(15) }, PPP_CONF_REJ, 4, { O24(15) } },
        { 'S', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 0, { 0 }, PPP_CONF_ACK, 0, { 0 } },
      }, PPP_COMP_NONE, PPP_COMP_NONE, 0, 0, 1 },
    { "configured for MPPC only", PPP_COMP_MPPC, 0, {
        { 'S', 6, { OMPPC }, PPP_CONF_ACK, 0, { 0 } },
        { 'P', 10, { O26(15), OMPPC }, PPP_CONF_REJ, 4, { O26(15) } },
        { 'P', 6, { OMPPC }, PPP_CONF_ACK, 6, { OMPPC } },
      }, PPP_COMP_MPPC, PPP_COMP_MPPC, 0, 0, 1 },
};

static int same(const frame_t *f, const uint8_t *o, size_t n)
{
    return f && f->n == n + 4 && (n == 0 || memcmp(f->d + 4, o, n) == 0) &&
           (((size_t)f->d[2] << 8) | f->d[3]) == n + 4;
}

static void run_case(const case_t *c)
{
    ppp_config_t cfg;
    uint8_t id = 1;
    int bad = 0;

    base_cfg(&cfg);
    cfg.comp = c->comp;
    if (c->wbits) cfg.deflate_wbits = c->wbits;
    up(&cfg);
    for (int i = 0; i < 10 && c->steps[i].who; i++) {
        const step_t *s = &c->steps[i];
        if (s->who == 'P') {
            frame_t *r;
            push_cp(P_CCP, PPP_CONF_REQ, ++id, s->opts, s->n);
            pull();
            r = find(P_CCP, s->code);
            if (!(r && r->d[1] == id && same(r, s->ropts, s->rn))) bad = 10 + i;
            CHECK(r && r->d[1] == id && same(r, s->ropts, s->rn));
            /* one reply to one request, never two */
            CHECK_EQ_I(!!find(P_CCP, PPP_CONF_ACK) + !!find(P_CCP, PPP_CONF_NAK) + !!find(P_CCP, PPP_CONF_REJ), 1);
        } else {
            if (!(scr_new && same(&scr, s->opts, s->n))) bad = 30 + i;
            CHECK(scr_new);
            CHECK(same(&scr, s->opts, s->n));
            scr_new = 0;
            if (s->code == PPP_CONF_ACK)
                push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
            else
                push_cp(P_CCP, s->code, scr.d[1], s->ropts, s->rn);
            pull();
        }
    }
    if (c->opened) CHECK_EQ_I(scr_new, 0);
    CHECK_EQ_I(status()->ccp_state == PPP_FSM_OPENED, c->opened);
    CHECK_EQ_I(status()->comp_tx, c->opened ? c->tx : 0);
    CHECK_EQ_I(status()->comp_rx, c->opened ? c->rx : 0);
    if (c->opened && c->tx == PPP_COMP_DEFLATE) CHECK_EQ_I(status()->counters.comp_tx_wbits, c->tx_w);
    if (c->opened && c->rx == PPP_COMP_DEFLATE) CHECK_EQ_I(status()->counters.comp_rx_wbits, c->rx_w);
    CHECK_EQ_I(status()->ip_up, 1);
    if (bad) printf("  case \"%s\": step %d\n", c->name, bad % 10);
}

static void test_negotiation(void)
{
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        run_case(&cases[i]);
}

/* ================================================================== */
/* the scripted peer's data side                                       */

static ppp_deflate_enc_t penc;      /* peer -> server */
static ppp_deflate_dec_t pdec;      /* server -> peer */
static ppp_mppc_enc_t pmenc;
static ppp_mppc_dec_t pmdec;
static uint16_t ptx_seq, prx_seq;
static uint8_t cbuf[1700], dbuf[1700];

/* bring CCP up with one algorithm both ways (the server configured for
 * both: what it asks for and the peer does not want is rejected) */
static void ccp_open(unsigned alg, int w, unsigned mru)
{
    ppp_config_t cfg;
    const uint8_t d[] = { O26(w) };
    const uint8_t m[] = { OMPPC };
    int i;

    base_cfg(&cfg);
    cfg.deflate_wbits = (uint8_t)w;
    up_mru(&cfg, mru);
    CHECK(scr_new);
    for (i = 0; i < 2 && scr.n > 4 && scr.d[4] != (alg == PPP_COMP_DEFLATE ? 26 : 18); i++) {
        push_cp(P_CCP, PPP_CONF_REJ, scr.d[1], scr.d + 4, scr.n - 4);
        pull();
    }
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    if (alg == PPP_COMP_DEFLATE) push_cp(P_CCP, PPP_CONF_REQ, 7, d, 4);
    else push_cp(P_CCP, PPP_CONF_REQ, 7, m, 6);
    pull();
    scr_new = 0;
    CHECK_EQ_I(status()->ccp_state, PPP_FSM_OPENED);
    CHECK_EQ_I(status()->comp_tx, alg);
    CHECK_EQ_I(status()->comp_rx, alg);
    ppp_deflate_enc_init(&penc, w, NULL);
    ppp_deflate_dec_init(&pdec);
    ppp_mppc_enc_init(&pmenc);
    ppp_mppc_dec_init(&pmdec);
    ptx_seq = prx_seq = 0;
}

/* the peer sends one IP packet through Deflate; `lose` drops it on the way */
static void peer_send_deflate(const uint8_t *pkt, size_t n, int lose)
{
    static const uint8_t pf[1] = { 0x21 };
    uint8_t h[2] = { (uint8_t)(ptx_seq >> 8), (uint8_t)ptx_seq };
    size_t cn = ppp_deflate_compress(&penc, pf, 1, pkt, n, cbuf, n > 3 ? n - 3 : 0);

    ptx_seq++;
    if (lose) return;
    if (cn) push2(P_COMP, h, 2, cbuf, cn);
    else push(PPP_PROTO_IP, pkt, n);
}

/* take the server's next data frame apart; returns the IP length or -1 */
static int peer_recv_deflate(const frame_t *f)
{
    static const uint8_t pf[1] = { 0x21 };
    int r;

    if (f->proto == PPP_PROTO_IP) {
        ppp_deflate_dec_add(&pdec, pf, 1, f->d, f->n);
        prx_seq++;
        memcpy(dbuf, f->d, f->n);
        return (int)f->n;
    }
    if (f->proto != P_COMP || f->n < 3) return -1;
    if ((((unsigned)f->d[0] << 8) | f->d[1]) != prx_seq) return -1;
    r = ppp_deflate_decompress(&pdec, f->d + 2, f->n - 2, cbuf, sizeof cbuf);
    /* RFC 1979 2.1: the Protocol field in its one-octet form */
    if (r < 1 || cbuf[0] != 0x21) return -1;
    prx_seq++;
    memcpy(dbuf, cbuf + 1, (size_t)r - 1);
    return r - 1;
}

static void tick(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) ppp_tick(&S, 10);
}

/* one packet from the server to the peer, checked */
static frame_t *server_send(const uint8_t *pkt, size_t n)
{
    frame_t *f;
    tick(40);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, n), 0);
    pull();
    f = find(P_COMP, 0);
    if (!f) f = find(PPP_PROTO_IP, 0);
    CHECK(f != NULL);
    return f;
}

static void test_deflate_data(void)
{
    static uint8_t pkt[1500], rnd[1500];
    gen_t g;
    frame_t *f;
    int r, native = 0;
    unsigned long in = 0;
    ppp_counters_t *c;

    for (int w = 8; w <= 15; w += 7) {
        ccp_open(PPP_COMP_DEFLATE, w, 0);
        gen_init(&g, 77 + (uint32_t)w);
        native = 0; in = 0;
        for (int i = 0; i < 300; i++) {
            int kind = i % 10 == 9 ? GEN_RANDOM : i % 3 ? GEN_HTML : GEN_TEXT;
            size_t n = i % 7 == 0 ? 1500 : 40 + (size_t)(gen_next(&g) % 1400);
            mk(&g, kind, pkt, n);
            /* server -> peer */
            f = server_send(pkt, n);
            if (!f) break;
            if (f->proto == PPP_PROTO_IP) native++;
            else CHECK(f->n < n);                    /* never longer than the packet */
            CHECK_EQ_I(count(P_COMP) + count(PPP_PROTO_IP), 1);
            r = peer_recv_deflate(f);
            CHECK(r == (int)n && memcmp(dbuf, pkt, n) == 0);
            in += n;
            /* peer -> server */
            mk(&g, kind, pkt, n);
            ip_len = 0;
            peer_send_deflate(pkt, n, 0);
            CHECK(ip_len == n && memcmp(ip_got, pkt, n) == 0);
        }
        /* the random packets went out as they were, and both histories
         * kept step through them */
        if (!(native >= 25 && native <= 45)) printf("  native %d\n", native);
        CHECK(native >= 25 && native <= 45);
        c = &status()->counters;
        CHECK_EQ_I(c->comp_tx_native, native);
        CHECK_EQ_I(c->comp_tx_in, in);
        CHECK(c->comp_tx_out < c->comp_tx_in);
        CHECK(c->comp_rx_out > c->comp_rx_in);
        CHECK_EQ_I(c->comp_rx_errors, 0);
        CHECK_EQ_I(c->comp_tx, PPP_COMP_DEFLATE);
        CHECK_EQ_I(c->comp_tx_wbits, w);
        CHECK_EQ_I(ip_count, 300);
        pull();
        CHECK_EQ_I(n_out, 0);
    }

    /* a packet sent native is in the history: the same one again is a
     * handful of octets */
    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 5);
    mk(&g, GEN_RANDOM, rnd, 1200);
    f = server_send(rnd, 1200);
    CHECK(f && f->proto == PPP_PROTO_IP);
    if (f) CHECK_EQ_I(peer_recv_deflate(f), 1200);
    f = server_send(rnd, 1200);
    CHECK(f && f->proto == P_COMP && f->n < 40);
    /* the sequence number counted the native one (RFC 1979 section 2) */
    if (f) CHECK(f->d[0] == 0 && f->d[1] == 1);
    if (f) CHECK(peer_recv_deflate(f) == 1200 && memcmp(dbuf, rnd, 1200) == 0);
    /* and the same from the peer */
    ip_len = 0;
    peer_send_deflate(rnd, 1200, 0);
    CHECK(ip_len == 1200);
    ip_len = 0;
    peer_send_deflate(rnd, 1200, 0);
    CHECK(ip_len == 1200 && memcmp(ip_got, rnd, 1200) == 0);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 0);

    /* tiny packets: one octet, and the empty one, which is never compressed */
    pkt[0] = 0x45;
    f = server_send(pkt, 1);
    CHECK(f && f->proto == PPP_PROTO_IP && f->n == 1);
    if (f) CHECK_EQ_I(peer_recv_deflate(f), 1);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 0), 0);
    pull();
    CHECK(find(PPP_PROTO_IP, 0) && find(PPP_PROTO_IP, 0)->n == 0);
    f = server_send(rnd, 1200);
    if (f) CHECK(peer_recv_deflate(f) == 1200);

    /* a two-octet Protocol field inside is taken as well */
    {
        static const uint8_t pf2[2] = { 0x00, 0x21 };
        uint8_t h[2] = { (uint8_t)(ptx_seq >> 8), (uint8_t)ptx_seq };
        size_t cn;
        mk(&g, GEN_TEXT, pkt, 700);
        cn = ppp_deflate_compress(&penc, pf2, 2, pkt, 700, cbuf, sizeof cbuf);
        ptx_seq++;
        ip_len = 0;
        push2(P_COMP, h, 2, cbuf, cn);
        CHECK(ip_len == 700 && memcmp(ip_got, pkt, 700) == 0);
    }
}

/* ---- what goes compressed and what does not ---- */
static void test_only_ip_is_compressed(void)
{
    static uint8_t pkt[600];
    static const uint8_t magic[4] = { 0x11, 0x22, 0x33, 0x44 };
    static const uint8_t ip[] = { 3, 6, 10, 0, 0, 2 };
    static const uint8_t pf_lcp[2] = { 0xc0, 0x21 }, pf_ipcp[2] = { 0x80, 0x21 }, pf_x[1] = { 0x2b };
    uint8_t h[2];
    size_t cn;
    gen_t g;
    frame_t *f;

    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 9);
    mk(&g, GEN_TEXT, pkt, 600);
    f = server_send(pkt, 600);
    CHECK(f && f->proto == P_COMP);
    if (f) CHECK_EQ_I(peer_recv_deflate(f), 600);

    /* LCP Echo: the reply is an LCP frame as ever */
    push_cp(PPP_PROTO_LCP, 9, 40, magic, 4);
    pull();
    CHECK(find(PPP_PROTO_LCP, 10) != NULL);
    CHECK_EQ_I(count(P_COMP), 0);
    /* IPCP renegotiated while CCP stays up: plain IPCP frames */
    push_cp(PPP_PROTO_IPCP, PPP_CONF_REQ, 9, ip, sizeof ip);
    pull();
    CHECK(find(PPP_PROTO_IPCP, PPP_CONF_ACK) != NULL);
    f = find(PPP_PROTO_IPCP, PPP_CONF_REQ);
    CHECK(f != NULL);
    CHECK_EQ_I(count(P_COMP), 0);
    if (f) push_cp(PPP_PROTO_IPCP, PPP_CONF_ACK, f->d[1], f->d + 4, f->n - 4);
    pull();
    CHECK_EQ_I(status()->ip_up, 1);
    CHECK_EQ_I(status()->ccp_state, PPP_FSM_OPENED);
    /* a CCP packet: Code-Reject for an unknown code, as CCP */
    push_cp(P_CCP, 77, 3, NULL, 0);
    pull();
    CHECK(find(P_CCP, 7) != NULL);
    CHECK_EQ_I(count(P_COMP), 0);
    /* and the histories are still in step after all that */
    f = server_send(pkt, 600);
    CHECK(f && f->proto == P_COMP);
    if (f) CHECK_EQ_I(peer_recv_deflate(f), 600);

    /* A control protocol inside a compressed packet is not acted on (RFC
     * 1979 section 2: LCP and NCP packets are never compressed). */
    h[0] = (uint8_t)(ptx_seq >> 8); h[1] = (uint8_t)ptx_seq; ptx_seq++;
    memset(pkt, 0, 8); pkt[0] = 5; pkt[1] = 1; pkt[3] = 4;      /* LCP Terminate-Request */
    cn = ppp_deflate_compress(&penc, pf_lcp, 2, pkt, 4, cbuf, sizeof cbuf);
    push2(P_COMP, h, 2, cbuf, cn);
    pull();
    CHECK_EQ_I(n_out, 0);
    CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
    h[0] = (uint8_t)(ptx_seq >> 8); h[1] = (uint8_t)ptx_seq; ptx_seq++;
    cn = ppp_deflate_compress(&penc, pf_ipcp, 2, pkt, 4, cbuf, sizeof cbuf);
    push2(P_COMP, h, 2, cbuf, cn);
    pull();
    CHECK_EQ_I(n_out, 0);
    CHECK_EQ_I(status()->ip_up, 1);
    CHECK_EQ_I(status()->counters.rx_bad_header, 2);
    /* a network protocol we do not have, compressed: Protocol-Reject */
    h[0] = (uint8_t)(ptx_seq >> 8); h[1] = (uint8_t)ptx_seq; ptx_seq++;
    cn = ppp_deflate_compress(&penc, pf_x, 1, pkt, 4, cbuf, sizeof cbuf);
    push2(P_COMP, h, 2, cbuf, cn);
    pull();
    f = find(PPP_PROTO_LCP, 8);
    CHECK(f && f->d[4] == 0x00 && f->d[5] == 0x2b);
    /* the same protocol sent native is counted by Deflate (it went through
     * the peer's compressor): the next compressed packet has the number
     * after it */
    push(0x002b, pkt, 4);
    ppp_deflate_compress(&penc, pf_x, 1, pkt, 4, cbuf, 0);
    ptx_seq++;
    pull();
    CHECK(find(PPP_PROTO_LCP, 8) != NULL);
    mk(&g, GEN_TEXT, pkt, 500);
    ip_len = 0;
    peer_send_deflate(pkt, 500, 0);
    CHECK(ip_len == 500 && memcmp(ip_got, pkt, 500) == 0);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 0);
}

/* ---- loss, garbage and the reset exchange (RFC 1962 3.1, RFC 1979 2) ---- */
static void test_deflate_reset(void)
{
    static uint8_t pkt[800];
    gen_t g;
    frame_t *f;
    uint8_t id;
    int i;

    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 21);
    for (i = 0; i < 5; i++) {
        mk(&g, GEN_HTML, pkt, 800);
        peer_send_deflate(pkt, 800, 0);
    }
    CHECK_EQ_I(ip_count, 5);
    /* one packet lost on the line: the next one has the wrong number */
    mk(&g, GEN_HTML, pkt, 800);
    peer_send_deflate(pkt, 800, 1);
    mk(&g, GEN_HTML, pkt, 800);
    peer_send_deflate(pkt, 800, 0);
    pull();
    f = find(P_CCP, RESET_REQ);
    CHECK(f != NULL && f->n == 4);
    id = f ? f->d[1] : 0;
    CHECK_EQ_I(ip_count, 5);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 1);
    /* everything compressed is discarded until the Reset-Ack, quietly */
    for (i = 0; i < 3; i++) {
        mk(&g, GEN_HTML, pkt, 800);
        peer_send_deflate(pkt, 800, 0);
    }
    pull();
    CHECK_EQ_I(n_out, 0);
    CHECK_EQ_I(ip_count, 5);
    CHECK_EQ_I(status()->counters.comp_rx_dropped, 4);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 1);
    /* the request is repeated with the same identifier */
    tick(2900);
    pull();
    CHECK_EQ_I(n_out, 0);
    tick(200);
    pull();
    f = find(P_CCP, RESET_REQ);
    CHECK(f != NULL && f->d[1] == id);
    /* a Reset-Ack with another identifier is not the answer */
    push_cp(P_CCP, RESET_ACK, (uint8_t)(id + 1), NULL, 0);
    mk(&g, GEN_HTML, pkt, 800);
    peer_send_deflate(pkt, 800, 0);
    CHECK_EQ_I(ip_count, 5);
    /* the right one: history cleared on both sides, number 0 again */
    ppp_deflate_enc_reset(&penc);
    ptx_seq = 0;
    push_cp(P_CCP, RESET_ACK, id, NULL, 0);
    for (i = 0; i < 5; i++) {
        mk(&g, GEN_HTML, pkt, 800);
        ip_len = 0;
        peer_send_deflate(pkt, 800, 0);
        CHECK(ip_len == 800 && memcmp(ip_got, pkt, 800) == 0);
    }
    CHECK_EQ_I(ip_count, 10);
    pull();
    CHECK_EQ_I(n_out, 0);
    /* a second, unsolicited Reset-Ack changes nothing */
    push_cp(P_CCP, RESET_ACK, id, NULL, 0);
    mk(&g, GEN_HTML, pkt, 800);
    peer_send_deflate(pkt, 800, 0);
    CHECK_EQ_I(ip_count, 11);

    /* garbage with the right number */
    {
        uint8_t junk[40];
        for (i = 0; i < 40; i++) junk[i] = (uint8_t)(gen_next(&g) >> 8);
        junk[0] = (uint8_t)(ptx_seq >> 8); junk[1] = (uint8_t)ptx_seq;
        junk[2] = 0x07;         /* block type 3, which does not exist */
        push(P_COMP, junk, 40);
        pull();
        f = find(P_CCP, RESET_REQ);
        CHECK(f != NULL && f->d[1] != id);
        id = f ? f->d[1] : 0;
        /* a compressed frame too short to hold anything, while waiting */
        push(P_COMP, junk, 2);
        pull();
        CHECK_EQ_I(n_out, 0);
    }
    /* no answer at all: after three requests CCP is negotiated anew */
    tick(3100);
    pull();
    CHECK(find(P_CCP, RESET_REQ) != NULL);
    tick(3100);
    pull();
    CHECK(find(P_CCP, RESET_REQ) != NULL);
    scr_new = 0;
    tick(3100);
    pull();
    CHECK(find(P_CCP, RESET_REQ) == NULL);
    CHECK(scr_new);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_NONE);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_NONE);
    /* IP goes on in its native form in the meantime */
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 800), 0);
    pull();
    CHECK(find(PPP_PROTO_IP, 0) != NULL);
    ip_len = 0;
    push(PPP_PROTO_IP, pkt, 800);
    CHECK_EQ_I(ip_len, 800);
    /* and compressed again once the peer has answered */
    {
        const uint8_t d[] = { O26(15) };
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
        push_cp(P_CCP, PPP_CONF_REQ, 50, d, 4);
        pull();
        CHECK_EQ_I(status()->comp_rx, PPP_COMP_DEFLATE);
        CHECK_EQ_I(status()->counters.ccp_opens, 2);
        ppp_deflate_enc_init(&penc, 15, NULL);
        ppp_deflate_dec_init(&pdec);
        ptx_seq = prx_seq = 0;
        mk(&g, GEN_HTML, pkt, 800);
        ip_len = 0;
        peer_send_deflate(pkt, 800, 0);
        CHECK(ip_len == 800 && memcmp(ip_got, pkt, 800) == 0);
        f = server_send(pkt, 800);
        if (f) CHECK_EQ_I(peer_recv_deflate(f), 800);
    }

    /* The other direction: the peer lost one of ours and asks. Reset-Ack
     * with its identifier, then number 0 and an empty history. */
    for (i = 0; i < 4; i++) {
        mk(&g, GEN_TEXT, pkt, 700);
        f = server_send(pkt, 700);
        if (f && i < 2) CHECK_EQ_I(peer_recv_deflate(f), 700);
    }
    CHECK_EQ_I(f ? peer_recv_deflate(f) : 0, -1);
    push_cp(P_CCP, RESET_REQ, 0x5a, NULL, 0);
    pull();
    f = find(P_CCP, RESET_ACK);
    CHECK(f != NULL && f->d[1] == 0x5a && f->n == 4);
    ppp_deflate_dec_reset(&pdec);
    prx_seq = 0;
    for (i = 0; i < 4; i++) {
        mk(&g, GEN_TEXT, pkt, 700);
        f = server_send(pkt, 700);
        if (f) CHECK(f->proto == P_COMP && f->d[0] == 0 && f->d[1] == i);
        if (f) CHECK(peer_recv_deflate(f) == 700 && memcmp(dbuf, pkt, 700) == 0);
    }
    /* the request repeated (our Ack was lost): answered every time */
    push_cp(P_CCP, RESET_REQ, 0x5a, NULL, 0);
    push_cp(P_CCP, RESET_REQ, 0x5a, NULL, 0);
    pull();
    CHECK_EQ_I(count(P_CCP), 2);
    CHECK_EQ_I(status()->counters.comp_tx_resets, 3);
    ppp_deflate_dec_reset(&pdec);
    prx_seq = 0;
    f = server_send(pkt, 700);
    if (f) CHECK_EQ_I(peer_recv_deflate(f), 700);

    /* a peer that Code-Rejects Reset-Request: renegotiation right away */
    {
        uint8_t rej[4] = { RESET_REQ, 1, 0, 4 };
        mk(&g, GEN_HTML, pkt, 800);
        peer_send_deflate(pkt, 800, 1);
        peer_send_deflate(pkt, 800, 0);
        pull();
        CHECK(find(P_CCP, RESET_REQ) != NULL);
        push_cp(P_CCP, 7, 9, rej, 4);
        scr_new = 0;
        tick(3100);
        pull();
        tick(3100);
        pull();
        tick(3100);
        pull();
        CHECK(scr_new);
        CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
    }
}

/* ---- CCP going down and up in mid-call ---- */
static void test_down_up(void)
{
    static uint8_t pkt[600];
    const uint8_t d[] = { O26(15) }, m[] = { OMPPC };
    gen_t g;
    frame_t *f;

    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 31);
    mk(&g, GEN_TEXT, pkt, 600);
    f = server_send(pkt, 600);
    CHECK(f && f->proto == P_COMP);
    /* the peer closes CCP: Terminate-Ack, and IP goes on uncompressed */
    push_cp(P_CCP, 5, 20, NULL, 0);
    pull();
    CHECK(find(P_CCP, 6) != NULL);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_NONE);
    CHECK_EQ_I(status()->ip_up, 1);
    f = server_send(pkt, 600);
    CHECK(f && f->proto == PPP_PROTO_IP && f->n == 600);
    ip_len = 0;
    push(PPP_PROTO_IP, pkt, 600);
    CHECK_EQ_I(ip_len, 600);
    /* a compressed datagram now is dropped: CCP is not Opened (RFC 1962 2.1) */
    ip_len = 0;
    peer_send_deflate(pkt, 600, 0);
    pull();
    CHECK_EQ_I(ip_len, 0);
    CHECK_EQ_I(n_out, 0);
    /* what the algorithm was stays readable after the fact */
    CHECK_EQ_I(status()->counters.comp_tx, PPP_COMP_DEFLATE);

    /* the peer opens it again, with MPPC this time. (We are in Stopping
     * until the restart timer has run once, RFC 1661 4.2; a request that
     * comes earlier is the peer's to repeat.) */
    tick(3100);
    pull();
    CHECK_EQ_I(n_out, 0);
    scr_new = 0;
    push_cp(P_CCP, PPP_CONF_REQ, 21, m, 6);
    pull();
    CHECK(find(P_CCP, PPP_CONF_ACK) != NULL);
    CHECK(scr_new);
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    CHECK_EQ_I(status()->ccp_state, PPP_FSM_OPENED);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_MPPC);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_DEFLATE);
    f = server_send(pkt, 600);
    /* RFC 2118 3.1: the first packet has the count 0 and FLUSHED set */
    CHECK(f && f->proto == P_COMP && (f->d[0] & 0xe0) == 0xe0 && (f->d[0] & 0x0f) == 0 && f->d[1] == 0);

    /* renegotiation from Opened (a new Configure-Request, RFC 1661 4.1):
     * down, and up again with what is agreed now, both sides from zero */
    scr_new = 0;
    push_cp(P_CCP, PPP_CONF_REQ, 22, d, 4);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_NONE);
    f = server_send(pkt, 600);
    CHECK(f && f->proto == PPP_PROTO_IP);
    CHECK(scr_new && find(P_CCP, PPP_CONF_ACK));
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_DEFLATE);
    ppp_deflate_dec_init(&pdec);
    prx_seq = 0;
    f = server_send(pkt, 600);
    CHECK(f && f->proto == P_COMP);
    if (f) CHECK(peer_recv_deflate(f) == 600 && memcmp(dbuf, pkt, 600) == 0);
    CHECK_EQ_I(status()->counters.ccp_opens, 3);

    /* the peer has no CCP at all: Protocol-Reject, and that is the end of it */
    {
        ppp_config_t cfg;
        uint8_t rej[12] = { 0x80, 0xfd };
        base_cfg(&cfg);
        up(&cfg);
        CHECK(scr_new);
        memcpy(rej + 2, scr.d, 8);
        push_cp(PPP_PROTO_LCP, 8, 30, rej, 10);
        scr_new = 0;
        tick(10000);
        pull();
        CHECK_EQ_I(scr_new, 0);
        CHECK_EQ_I(count(P_CCP), 0);
        CHECK_EQ_I(status()->ip_up, 1);
        f = server_send(pkt, 600);
        CHECK(f && f->proto == PPP_PROTO_IP);
    }
    /* compression switched off: CCP is an unknown protocol, and so are
     * compressed datagrams */
    {
        ppp_config_t cfg;
        base_cfg(&cfg);
        cfg.comp = 0;
        up(&cfg);
        CHECK_EQ_I(scr_new, 0);
        push_cp(P_CCP, PPP_CONF_REQ, 1, d, 4);
        pull();
        f = find(PPP_PROTO_LCP, 8);
        CHECK(f && f->d[4] == 0x80 && f->d[5] == 0xfd);
        CHECK_EQ_I(count(P_CCP), 0);
        push(P_COMP, pkt, 20);
        pull();
        f = find(PPP_PROTO_LCP, 8);
        CHECK(f && f->d[4] == 0x00 && f->d[5] == 0xfd);
        tick(10000);
        pull();
        CHECK_EQ_I(count(P_CCP), 0);
    }
    /* no answer to CCP at all (a peer that just drops it): we stop asking
     * after Max-Configure and the call is not harmed */
    {
        ppp_config_t cfg;
        base_cfg(&cfg);
        up(&cfg);
        tick(60000);
        pull();
        scr_new = 0;
        tick(10000);
        pull();
        CHECK_EQ_I(scr_new, 0);
        CHECK_EQ_I(status()->ip_up, 1);
        CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
        f = server_send(pkt, 600);
        CHECK(f && f->proto == PPP_PROTO_IP);
        /* ... and a late Configure-Request still gets CCP up */
        push_cp(P_CCP, PPP_CONF_REQ, 1, d, 4);
        pull();
        CHECK(find(P_CCP, PPP_CONF_ACK) != NULL);
        CHECK(scr_new);
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
        CHECK_EQ_I(status()->comp_tx, PPP_COMP_DEFLATE);
    }
    /* LCP going down takes CCP with it; the next call starts clean */
    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    push_cp(PPP_PROTO_LCP, 5, 77, NULL, 0);
    pull();
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_NONE);
    CHECK(status()->ccp_state != PPP_FSM_OPENED);
}

/* ================================================================== */
/* MPPC at session level                                               */

static void peer_send_mppc(const uint8_t *pkt, size_t n, int lose, int pfc)
{
    static const uint8_t pf[2] = { 0x00, 0x21 };
    size_t pl = pfc ? 1 : 2;
    unsigned bits;
    uint8_t h[4];
    size_t cn = ppp_mppc_compress(&pmenc, pf + 2 - pl, pl, pkt, n, cbuf, n + pl - 1, &bits);

    h[0] = (uint8_t)((bits | ptx_seq) >> 8); h[1] = (uint8_t)ptx_seq;
    ptx_seq = (ptx_seq + 1) & 0xfff;
    if (lose) return;
    if (cn) { push2(P_COMP, h, 2, cbuf, cn); return; }
    memcpy(h + 2, pf + 2 - pl, pl);
    memcpy(cbuf, h, 2 + pl);
    memcpy(cbuf + 2 + pl, pkt, n);
    push(P_COMP, cbuf, 2 + pl + n);
}

static int mppc_wait;       /* the peer's decompressor is waiting for FLUSHED */

/* returns the IP length, -1 for a packet out of step, -2 for one discarded */
static int peer_recv_mppc(const frame_t *f)
{
    unsigned hdr;
    const uint8_t *d = cbuf;
    int r;

    if (f->proto == PPP_PROTO_IP) {
        memcpy(dbuf, f->d, f->n);
        return (int)f->n;
    }
    if (f->proto != P_COMP || f->n < 3) return -1;
    hdr = ((unsigned)f->d[0] << 8) | f->d[1];
    if (hdr & PPP_MPPC_ENCRYPTED) return -1;
    if (hdr & PPP_MPPC_FLUSHED) {
        ppp_mppc_dec_flush(&pmdec);
        prx_seq = hdr & 0xfff;
        mppc_wait = 0;
    } else if (mppc_wait) {
        return -2;
    }
    if ((hdr & 0xfff) != prx_seq) { mppc_wait = 1; return -1; }
    prx_seq = (prx_seq + 1) & 0xfff;
    if (hdr & PPP_MPPC_COMPRESSED) {
        r = ppp_mppc_decompress(&pmdec, (hdr & PPP_MPPC_AT_FRONT) != 0, f->d + 2, f->n - 2, cbuf, sizeof cbuf);
        if (r < 3) { mppc_wait = 1; return -1; }
    } else {
        d = f->d + 2;
        r = (int)f->n - 2;
    }
    /* our side sends the Protocol field in two octets */
    if (d[0] != 0x00 || d[1] != 0x21) return -1;
    memcpy(dbuf, d + 2, (size_t)r - 2);
    return r - 2;
}

/* ---- resets do not wait behind the transmit queue ---- */
static void part(size_t n)
{
    /* the first n octets of what is waiting go out on the line */
    size_t k = ppp_output_bytes(&S, wire, n);
    for (size_t i = 0; i < k; i++)
        CHECK(!ppp_hdlc_rx_byte(&dec, wire[i]));
}

static void test_reset_queue(void)
{
    static uint8_t pkt[6][700], rnd[300];
    static const uint8_t magic[4] = { 0x11, 0x22, 0x33, 0x44 };
    gen_t g;
    frame_t *f;
    int alg, i;

    for (alg = PPP_COMP_DEFLATE; alg <= (int)PPP_COMP_MPPC; alg++) {
        /* Five compressed packets, an LCP Echo-Reply and a packet that
         * went native are waiting, the first of them already going out,
         * when the peer asks for a reset. */
        ccp_open((unsigned)alg, 15, 0);
        mppc_wait = 0;
        gen_init(&g, 4711);
        mk(&g, GEN_RANDOM, rnd, 300);
        for (i = 0; i < 6; i++) mk(&g, GEN_HTML, pkt[i], 700);
        tick(200);
        CHECK_EQ_I(ppp_send_ip(&S, pkt[0], 700), 0);
        tick(200);
        CHECK_EQ_I(ppp_send_ip(&S, pkt[1], 700), 0);
        push_cp(PPP_PROTO_LCP, 9, 40, magic, 4);
        tick(200);
        CHECK_EQ_I(ppp_send_ip(&S, pkt[2], 700), 0);
        if (alg == (int)PPP_COMP_DEFLATE) {
            tick(200);
            CHECK_EQ_I(ppp_send_ip(&S, rnd, 300), 0);
        }
        tick(200);
        CHECK_EQ_I(ppp_send_ip(&S, pkt[3], 700), 0);
        tick(200);
        CHECK_EQ_I(ppp_send_ip(&S, pkt[4], 700), 0);
        part(20);
        push_cp(P_CCP, RESET_REQ, 0x44, NULL, 0);
        pull();
        /* the frame in transmission is finished; the other compressed
         * ones are gone; the rest is there in its order; then the Ack */
        CHECK_EQ_I(n_out, alg == (int)PPP_COMP_DEFLATE ? 4 : 3);
        CHECK(n_out >= 3 && out[0].proto == P_COMP);
        CHECK(out[1].proto == PPP_PROTO_LCP && out[1].d[0] == 10);
        if (alg == (int)PPP_COMP_DEFLATE)
            CHECK(out[2].proto == PPP_PROTO_IP && out[2].n == 300 && memcmp(out[2].d, rnd, 300) == 0);
        f = &out[n_out - 1];
        CHECK(f->proto == P_CCP && f->d[0] == RESET_ACK && f->d[1] == 0x44);
        CHECK_EQ_I(status()->counters.comp_tx_purged, 4);
        CHECK_EQ_I(status()->counters.tx_dropped, 0);
        /* and what follows starts afresh */
        ppp_deflate_dec_reset(&pdec);
        prx_seq = 0;
        mppc_wait = 1;
        f = server_send(pkt[5], 700);
        if (f && alg == (int)PPP_COMP_DEFLATE) CHECK(f->d[0] == 0 && f->d[1] == 0 && peer_recv_deflate(f) == 700);
        if (f && alg == (int)PPP_COMP_MPPC) CHECK((f->d[0] & 0x80) && (f->d[0] & 0x0f) == 0 && f->d[1] == 5 && peer_recv_mppc(f) == 700);
        CHECK(memcmp(dbuf, pkt[5], 700) == 0);

        /* Nothing going out yet (the line is busy elsewhere): all of the
         * compressed ones go */
        for (i = 0; i < 3; i++) {
            tick(200);
            CHECK_EQ_I(ppp_send_ip(&S, pkt[i], 700), 0);
        }
        push_cp(P_CCP, RESET_REQ, 0x45, NULL, 0);
        pull();
        CHECK_EQ_I(n_out, 1);
        CHECK(out[0].proto == P_CCP && out[0].d[0] == RESET_ACK && out[0].d[1] == 0x45);
        CHECK_EQ_I(status()->counters.comp_tx_purged, 7);

        /* Our own Reset-Request passes what is waiting: it goes out right
         * behind the frame in transmission */
        ccp_open((unsigned)alg, 15, 0);
        for (i = 0; i < 4; i++) {
            tick(200);
            CHECK_EQ_I(ppp_send_ip(&S, pkt[i], 700), 0);
        }
        part(20);
        {
            uint8_t junk[8] = { 0x0f, 0xff, 7, 7, 7, 7, 7, 7 };     /* a count far from the one due */
            push(P_COMP, junk, 8);
        }
        pull();
        CHECK_EQ_I(n_out, 5);
        CHECK(out[0].proto == P_COMP);
        CHECK(out[1].proto == P_CCP && out[1].d[0] == RESET_REQ && out[1].n == 4);
        CHECK(out[2].proto == P_COMP && out[3].proto == P_COMP && out[4].proto == P_COMP);
        /* the data frames are whole and in order */
        for (i = 0; i < 5; i++) {
            if (i == 1) continue;
            CHECK_EQ_I(alg == (int)PPP_COMP_DEFLATE ? peer_recv_deflate(&out[i]) : peer_recv_mppc(&out[i]), 700);
            CHECK(memcmp(dbuf, pkt[i ? i - 1 : 0], 700) == 0);
        }
        /* the repeat on an idle line simply goes out (Deflate; with MPPC
         * there is none without cause, see test_mppc_reset_idle) */
        tick(3100);
        pull();
        CHECK_EQ_I(n_out, alg == (int)PPP_COMP_DEFLATE);
        if (n_out) CHECK(out[0].proto == P_CCP && out[0].d[0] == RESET_REQ);
    }

    /* every octet of the frames escaped (the default map), Address and
     * Control and a two-octet Protocol field present: found all the same */
    {
        ppp_config_t cfg;
        const uint8_t d[] = { O26(15) };
        base_cfg(&cfg);
        cfg.no_pfc = cfg.no_acfc = 1;
        up(&cfg);
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
        push_cp(P_CCP, PPP_CONF_REQ, 7, d, 4);
        pull();
        CHECK_EQ_I(status()->comp_tx, PPP_COMP_DEFLATE);
        CHECK_EQ_I(status()->tx_accm, 0xffffffffu);
        for (i = 0; i < 4; i++) {
            tick(200);
            CHECK_EQ_I(ppp_send_ip(&S, pkt[i], 700), 0);
        }
        part(20);
        push_cp(P_CCP, RESET_REQ, 0x46, NULL, 0);
        pull();
        CHECK_EQ_I(n_out, 2);
        CHECK(out[0].proto == P_COMP && out[1].proto == P_CCP && out[1].d[0] == RESET_ACK);
        CHECK_EQ_I(status()->counters.comp_tx_purged, 3);
    }
}

static void test_mppc_data(void)
{
    static uint8_t pkt[1500], rnd[1500];
    gen_t g;
    frame_t *f;
    unsigned want = 0, hdr;
    int r, i, wraps = 0, fronts = 0, plain = 0;

    /* 4500 packets each way: the coherency count passes 4095 and goes on
     * at 0 (RFC 2118 3.1), the history wraps hundreds of times */
    ccp_open(PPP_COMP_MPPC, 15, 0);
    mppc_wait = 0;
    gen_init(&g, 404);
    for (i = 0; i < 4500; i++) {
        int kind = i % 16 == 15 ? GEN_RANDOM : i % 3 ? GEN_HTML : GEN_BINARY;
        size_t n = i % 9 == 0 ? 1496 : 30 + (size_t)(gen_next(&g) % 600);
        mk(&g, kind, pkt, n);
        f = server_send(pkt, n);
        if (!f) break;
        CHECK_EQ_I(f->proto, P_COMP);
        hdr = ((unsigned)f->d[0] << 8) | f->d[1];
        CHECK_EQ_I(hdr & 0xfff, want);
        if (want == 0 && i > 0) wraps++;
        want = (want + 1) & 0xfff;
        CHECK_EQ_I(hdr & PPP_MPPC_ENCRYPTED, 0);
        /* FLUSHED on the first packet and after each one sent as it was */
        CHECK_EQ_I((hdr & PPP_MPPC_FLUSHED) != 0, i == 0 || plain);
        plain = !(hdr & PPP_MPPC_COMPRESSED);
        if (plain) {
            /* RFC 2118 section 3: sent uncompressed because it grew */
            CHECK_EQ_I(f->n, n + 4);
            CHECK(memcmp(f->d + 4, pkt, n) == 0);
        } else {
            CHECK(f->n <= n + 3);
        }
        if ((hdr & PPP_MPPC_AT_FRONT) && !(hdr & PPP_MPPC_FLUSHED)) fronts++;
        r = peer_recv_mppc(f);
        CHECK(r == (int)n && memcmp(dbuf, pkt, n) == 0);
        /* and the peer's packets to us, with either form of the inner
         * Protocol field */
        mk(&g, kind, pkt, n);
        ip_len = 0;
        peer_send_mppc(pkt, n, 0, i & 1);
        CHECK(ip_len == n && memcmp(ip_got, pkt, n) == 0);
    }
    CHECK_EQ_I(wraps, 1);
    if (fronts <= 10) printf("  fronts %d\n", fronts);
    CHECK(fronts > 10);
    CHECK_EQ_I(ip_count, 4500);
    CHECK_EQ_I(status()->counters.comp_rx_errors, 0);
    CHECK(status()->counters.comp_tx_native > 200);
    CHECK(status()->counters.comp_tx_out < status()->counters.comp_tx_in);
    pull();
    CHECK_EQ_I(n_out, 0);

    /* a full-size packet that does not compress: four octets more than the
     * peer's MRU as an MPPC packet, so it goes in its standard form (RFC
     * 1962 2.1) and the count does not move */
    mk(&g, GEN_RANDOM, rnd, 1500);
    f = server_send(rnd, 1500);
    CHECK(f && f->proto == PPP_PROTO_IP && f->n == 1500);
    f = server_send(pkt, 400);
    hdr = f ? ((unsigned)f->d[0] << 8) | f->d[1] : 0;
    CHECK_EQ_I(hdr & 0xfff, want);
    want = (want + 1) & 0xfff;
    /* the history was flushed by the attempt */
    CHECK(hdr & PPP_MPPC_FLUSHED);
    if (f) CHECK(peer_recv_mppc(f) == 400);

    /* a packet of ours lost on the way: the peer sees the count jump and
     * sends Reset-Request; we answer and flush (RFC 2118 4.3) */
    f = server_send(pkt, 400);
    want = (want + 1) & 0xfff;
    f = server_send(pkt, 400);
    want = (want + 1) & 0xfff;
    CHECK_EQ_I(f ? peer_recv_mppc(f) : 0, -1);
    f = server_send(pkt, 400);
    want = (want + 1) & 0xfff;
    CHECK_EQ_I(f ? peer_recv_mppc(f) : 0, -2);
    push_cp(P_CCP, RESET_REQ, 0x33, NULL, 0);
    pull();
    f = find(P_CCP, RESET_ACK);
    CHECK(f && f->d[1] == 0x33);
    f = server_send(pkt, 400);
    hdr = f ? ((unsigned)f->d[0] << 8) | f->d[1] : 0;
    CHECK(hdr & PPP_MPPC_FLUSHED);
    CHECK_EQ_I(hdr & 0xfff, want);          /* the count goes on */
    want = (want + 1) & 0xfff;
    if (f) CHECK(peer_recv_mppc(f) == 400 && memcmp(dbuf, pkt, 400) == 0);

    /* a packet of the peer's lost: Reset-Request from us; what follows is
     * dropped until a packet with FLUSHED comes, whatever its count */
    mk(&g, GEN_HTML, pkt, 500);
    peer_send_mppc(pkt, 500, 1, 0);
    ip_count = 0;
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    f = find(P_CCP, RESET_REQ);
    CHECK(f != NULL);
    peer_send_mppc(pkt, 500, 0, 0);
    peer_send_mppc(pkt, 500, 0, 0);
    CHECK_EQ_I(ip_count, 0);
    pull();
    CHECK_EQ_I(n_out, 0);
    ppp_mppc_enc_flush(&pmenc);
    if (f) push_cp(P_CCP, RESET_ACK, f->d[1], NULL, 0);
    ip_len = 0;
    peer_send_mppc(pkt, 500, 0, 0);
    CHECK(ip_len == 500 && memcmp(ip_got, pkt, 500) == 0);
    peer_send_mppc(pkt, 500, 0, 1);
    CHECK_EQ_I(ip_count, 2);
    tick(7000);
    pull();
    CHECK_EQ_I(count(P_CCP), 0);            /* no more Reset-Requests: in step */

    /* garbage: a copy reaching back before the start of the history */
    {
        uint8_t bad[6] = { 0, 0, 0xff, 0xff, 0xff, 0xff };
        bad[0] = (uint8_t)(0x20 | (ptx_seq >> 8)); bad[1] = (uint8_t)ptx_seq;
        ppp_mppc_enc_flush(&pmenc);
        ip_count = 0;
        push(P_COMP, bad, 6);
        pull();
        CHECK(find(P_CCP, RESET_REQ) != NULL);
        /* the encryption bit, which we never agreed to */
        ptx_seq = (ptx_seq + 1) & 0xfff;
        peer_send_mppc(pkt, 500, 0, 0);     /* FLUSHED: in step again */
        CHECK_EQ_I(ip_count, 1);
        bad[0] = (uint8_t)(0x10 | (ptx_seq >> 8)); bad[1] = (uint8_t)ptx_seq;
        push(P_COMP, bad, 6);
        pull();
        CHECK(find(P_CCP, RESET_REQ) != NULL);
        CHECK_EQ_I(ip_count, 1);
    }
}

/* With MPPC the answer to a Reset-Request is a packet with FLUSHED and
 * nothing else (RFC 2118 4.3). A peer that has gone quiet is not asked
 * again and again; one that goes on with its old history is. */
static void test_mppc_reset_idle(void)
{
    static uint8_t pkt[500];
    gen_t g;
    frame_t *f;
    uint8_t id;
    unsigned long opens;

    ccp_open(PPP_COMP_MPPC, 15, 0);
    gen_init(&g, 606);
    mk(&g, GEN_HTML, pkt, 500);
    peer_send_mppc(pkt, 500, 0, 0);
    peer_send_mppc(pkt, 500, 1, 0);
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    f = find(P_CCP, RESET_REQ);
    CHECK(f != NULL);
    id = f ? f->d[1] : 0;
    opens = status()->counters.ccp_opens;
    /* silence from the peer: nothing more from us either */
    tick(30000);
    pull();
    CHECK_EQ_I(count(P_CCP), 0);
    CHECK_EQ_I(status()->ccp_state, PPP_FSM_OPENED);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_MPPC);
    /* a packet without FLUSHED: the request did not get through */
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    f = find(P_CCP, RESET_REQ);
    CHECK(f != NULL && f->d[1] == id);
    /* more of them within the Restart period: one request is enough */
    peer_send_mppc(pkt, 500, 0, 0);
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    CHECK_EQ_I(n_out, 0);
    tick(3100);
    pull();
    CHECK_EQ_I(n_out, 0);
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    CHECK(find(P_CCP, RESET_REQ) != NULL);
    /* three requests and the peer still goes on: CCP anew */
    tick(3100);
    scr_new = 0;
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    CHECK(find(P_CCP, RESET_REQ) == NULL);
    CHECK(scr_new);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_NONE);
    CHECK_EQ_I(status()->counters.ccp_opens, opens);
    CHECK_EQ_I(ip_count, 1);

    /* the usual case: the peer flushes and its next packet says so,
     * whenever that comes */
    ccp_open(PPP_COMP_MPPC, 15, 0);
    peer_send_mppc(pkt, 500, 1, 0);
    peer_send_mppc(pkt, 500, 0, 0);
    pull();
    CHECK(find(P_CCP, RESET_REQ) != NULL);
    tick(60000);
    ppp_mppc_enc_flush(&pmenc);
    ip_len = 0;
    peer_send_mppc(pkt, 500, 0, 0);
    CHECK(ip_len == 500 && memcmp(ip_got, pkt, 500) == 0);
    pull();
    CHECK_EQ_I(n_out, 0);
}

/* ---- the bound on the queue of compressed packets (lossy links) ---- */
static void test_comp_queue(void)
{
    static uint8_t pkt[1000];
    gen_t g;
    int i, sent = 0;

    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    ppp_set_comp_queue(&S, 1500);
    gen_init(&g, 707);
    for (i = 0; i < 40; i++) {
        int r;
        mk(&g, GEN_HTML, pkt, 1000);
        tick(100);
        r = ppp_send_ip(&S, pkt, 1000);
        CHECK(r == 0 || r == PPP_ERR_FULL);
        if (r == 0) sent++;
    }
    /* the first is some 600 octets compressed, the others half of that:
     * accepted while 1500 or fewer wait */
    if (!(sent >= 3 && sent <= 6)) printf("  sent %d\n", sent);
    CHECK(sent >= 3 && sent <= 6);
    CHECK(S.tx_tail - S.tx_head > 1500 && S.tx_tail - S.tx_head < 1500 + 700);
    pull();
    CHECK_EQ_I(n_out, sent);
    for (i = 0; i < n_out; i++) CHECK_EQ_I(peer_recv_deflate(&out[i]), 1000);
    /* the bound is on compressed traffic only */
    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    push_cp(P_CCP, 5, 20, NULL, 0);
    pull();
    ppp_set_comp_queue(&S, 600);
    sent = 0;
    for (i = 0; i < 5; i++)
        if (ppp_send_ip(&S, pkt, 1000) == 0) sent++;
    CHECK_EQ_I(sent, 5);
}

/* ---- the peer's MRU (RFC 1661 6.1) holds for compressed packets too ---- */
static void test_mru(void)
{
    static uint8_t pkt[1500];
    gen_t g;
    frame_t *f;
    int alg, i, r, native = 0;

    for (alg = PPP_COMP_DEFLATE; alg <= (int)PPP_COMP_MPPC; alg++) {
        ccp_open((unsigned)alg, 15, 296);
        mppc_wait = 0;
        gen_init(&g, 600 + (uint32_t)alg);
        CHECK_EQ_I(ppp_send_ip(&S, pkt, 297), PPP_ERR_SIZE);
        for (i = 0; i < 400; i++) {
            size_t n = i % 3 ? 296 : 280 + (size_t)(gen_next(&g) % 17);
            mk(&g, i % 4 == 3 ? GEN_RANDOM : i % 4 == 2 ? GEN_BINARY : GEN_HTML, pkt, n);
            f = server_send(pkt, n);
            if (!f) break;
            /* the information field of whatever went out */
            CHECK(f->n <= 296);
            if (f->proto == PPP_PROTO_IP) native++;
            r = alg == PPP_COMP_DEFLATE ? peer_recv_deflate(f) : peer_recv_mppc(f);
            CHECK(r == (int)n && memcmp(dbuf, pkt, n) == 0);
        }
        CHECK(native > 50);
    }
}

/* ---- the bound on the work per unit of time ---- */
static void test_credit(void)
{
    static uint8_t pkt[1500];
    ppp_config_t cfg;
    gen_t g;
    frame_t *f;
    int i, sent = 0, full = 0, r;
    const uint8_t d[] = { O26(15) };

    /* 40 octets per millisecond by default: one full packet per 10 ms
     * tick goes through at once, the second has to wait */
    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 808);
    mk(&g, GEN_HTML, pkt, 1500);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 1500), 0);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 1500), PPP_ERR_FULL);
    CHECK_EQ_I(ppp_send_ip(&S, pkt, 1500), PPP_ERR_FULL);
    pull();
    CHECK_EQ_I(count(P_COMP), 1);
    CHECK_EQ_I(peer_recv_deflate(&out[0]), 1500);
    /* a refused packet did not touch the history or the number */
    for (i = 0; i < 1000; i++) {
        ppp_tick(&S, 10);
        r = ppp_send_ip(&S, pkt, 1500);
        if (r == 0) sent++;
        else full++;
        CHECK(r == 0 || r == PPP_ERR_FULL);
        pull();
        CHECK_EQ_I(count(P_COMP) + count(PPP_PROTO_IP), r == 0);
        if (r == 0) CHECK_EQ_I(peer_recv_deflate(&out[0]), 1500);
    }
    /* 10 s at 40 octets/ms is 400000 octets, 266 packets of 1500 */
    CHECK(sent >= 260 && sent <= 270);
    CHECK(full > 700);
    /* small packets: several per tick, to the same total */
    sent = 0;
    for (i = 0; i < 1000; i++) {
        ppp_tick(&S, 10);
        while (ppp_send_ip(&S, pkt, 100) == 0) sent++;
        pull();
    }
    CHECK(sent >= 3900 && sent <= 4100);

    /* the rate is a configuration value */
    base_cfg(&cfg);
    cfg.comp_rate = 150;
    up(&cfg);
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    push_cp(P_CCP, PPP_CONF_REQ, 7, d, 4);
    pull();
    sent = 0;
    for (i = 0; i < 100; i++) {
        ppp_tick(&S, 10);
        if (ppp_send_ip(&S, pkt, 1500) == 0) sent++;
        pull();
    }
    CHECK_EQ_I(sent, 100);

    /* the transmit buffer nearly full: refused before the compressor
     * sees the packet, so nothing is lost from the sequence */
    ccp_open(PPP_COMP_DEFLATE, 15, 0);
    gen_init(&g, 809);
    sent = 0;
    for (i = 0; i < 60; i++) {
        mk(&g, GEN_RANDOM, pkt, 1500);
        tick(40);
        r = ppp_send_ip(&S, pkt, 1500);
        CHECK(r == 0 || r == PPP_ERR_FULL);
        if (r == 0) sent++;
    }
    CHECK(sent >= 2 && sent < 60);
    pull();
    CHECK_EQ_I(n_out, sent);
    for (i = 0; i < n_out; i++)
        CHECK_EQ_I(peer_recv_deflate(&out[i]), 1500);
    mk(&g, GEN_HTML, pkt, 900);
    f = server_send(pkt, 900);
    CHECK(f && f->proto == P_COMP);
    if (f) CHECK(peer_recv_deflate(f) == 900 && memcmp(dbuf, pkt, 900) == 0);
    CHECK_EQ_I(status()->counters.tx_dropped, 0);
}

/* ---- anything at all on the CCP and compressed-data protocols ---- */
static void test_fuzz(void)
{
    static uint8_t pkt[1500];
    gen_t g;
    int alg, i;

    for (alg = PPP_COMP_DEFLATE; alg <= (int)PPP_COMP_MPPC; alg++) {
        ccp_open((unsigned)alg, 15, 0);
        gen_init(&g, 9000 + (uint32_t)alg);
        for (i = 0; i < 30000; i++) {
            size_t n = (size_t)(gen_next(&g) % (i % 50 ? 60 : 1500));
            uint32_t r = gen_next(&g);
            mk(&g, GEN_RANDOM, pkt, n);
            if (i % 16 == 0 && S.ccp.state != PPP_FSM_OPENED) {
                /* the peer brings CCP back up, so that most of the run
                 * meets a decompressor that is on */
                const uint8_t d[] = { O26(15) }, m[] = { OMPPC };
                tick(3100);
                scr_new = 0;
                push_cp(P_CCP, PPP_CONF_REQ, (uint8_t)i, alg == (int)PPP_COMP_MPPC ? m : d, alg == (int)PPP_COMP_MPPC ? 6 : 4);
                pull();
                if (scr_new) push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
                pull();
            }
            switch (r % 5) {
            case 0:
                /* a well-formed header in front of the noise half the time,
                 * and of those mostly the two reset codes */
                if (n >= 4 && (r & 0x1000)) {
                    pkt[0] = (uint8_t)((r & 0x6000) ? 14 + (r >> 16 & 1) : 1 + r / 5 % 16);
                    pkt[2] = (uint8_t)(n >> 8); pkt[3] = (uint8_t)n;
                }
                push(P_CCP, pkt, n);
                break;
            case 1:
                if (n >= 2) { pkt[0] = (uint8_t)(S.rx_seq >> 8) | (uint8_t)(alg == PPP_COMP_MPPC ? (r >> 8 & 0xe0) : 0); pkt[1] = (uint8_t)S.rx_seq; }
                push(P_COMP, pkt, n);
                break;
            case 2:
                push(P_COMP, pkt, n);
                break;
            case 3:
                push(PPP_PROTO_IP, pkt, n);
                break;
            default:
                tick(r / 5 % 4000);
                if (S.ccp.state == PPP_FSM_OPENED) (void)ppp_send_ip(&S, pkt, n);
                break;
            }
            pull();
            for (int k = 0; k < n_out; k++) {
                /* whatever we say on CCP is well-formed */
                if (out[k].proto == P_CCP)
                    CHECK(out[k].n >= 4 && ((size_t)out[k].d[2] << 8 | out[k].d[3]) == out[k].n);
                CHECK(out[k].n <= 1500);
                if (out[k].proto == P_CCP && out[k].d[0] == PPP_CONF_REQ && (r & 0x300))
                    push_cp(P_CCP, PPP_CONF_ACK, out[k].d[1], out[k].d + 4, out[k].n - 4);
            }
        }
        /* the link itself came to no harm */
        CHECK_EQ_I(status()->phase, PPP_PHASE_NETWORK);
        CHECK_EQ_I(status()->ip_up, 1);
        if (status()->counters.comp_rx_errors <= 100) printf("  errors %u opens %u\n", (unsigned)status()->counters.comp_rx_errors, (unsigned)status()->counters.ccp_opens);
        CHECK(status()->counters.comp_rx_errors > 100);
    }
}

/* MPPE (issue #6): the server offers and accepts 128-bit stateless MPPE in
 * CCP, encrypts what it sends and decrypts what it receives. The far end is
 * modelled with ppp_mppe directly (its keys are the server's, swapped). */
static void test_mppe(void)
{
    ppp_config_t cfg;
    ppp_mppe_t peer;
    frame_t *f;
    static const uint8_t ompe[6] = { 18, 6, 0x01, 0x00, 0x00, 0x40 };  /* 128-bit stateless */
    static const uint8_t ip[60] = { 0x45, 0x00, 0x00, 0x3c, 0x12, 0x34 };
    uint8_t enc[128], pt[128], plain[2 + 60];
    unsigned i;
    size_t n;
    int d;

    for (i = 0; i < 16; i++) { inj_s[i] = (uint8_t)(i + 1); inj_r[i] = (uint8_t)(i + 101); }
    base_cfg(&cfg);
    cfg.comp = 0;              /* encryption only */
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;

    /* the server's own CCP request is 128-bit stateless MPPE on option 18 */
    CHECK(scr_new && same(&scr, ompe, 6));
    /* the peer requests the same: Ack (our transmit direction) */
    push_cp(P_CCP, PPP_CONF_REQ, 20, ompe, 6);
    pull();
    f = find(P_CCP, PPP_CONF_ACK);
    CHECK(f && same(f, ompe, 6));
    /* the peer Acks the server's request (our receive); CCP opens */
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    CHECK(status()->ccp_state == PPP_FSM_OPENED);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_MPPE);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_MPPE);

    /* far end: its receive key is the server's send key, and vice versa */
    ppp_mppe_begin(&peer, inj_r, inj_s, 16, PPP_MPPE_FORCE_NONE);

    /* server -> peer: the datagram goes out MPPE-encrypted (0x00FD) */
    CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
    pull();
    f = find(P_COMP, 0);
    CHECK(f != NULL);
    if (f) {
        d = ppp_mppe_decrypt(&peer, f->d, f->n, pt, sizeof pt);
        CHECK_EQ_I(d, (int)sizeof ip + 2);
        CHECK(d == (int)sizeof ip + 2 && pt[0] == 0x00 && pt[1] == 0x21 &&
              memcmp(pt + 2, ip, sizeof ip) == 0);
    }

    /* peer -> server: an MPPE frame is decrypted and delivered as IP */
    plain[0] = 0x00; plain[1] = 0x21;
    memcpy(plain + 2, ip, sizeof ip);
    n = ppp_mppe_encrypt(&peer, plain, sizeof plain, enc, sizeof enc);
    ip_count = 0;
    push(P_COMP, enc, n);
    CHECK_EQ_I(ip_count, 1);
    CHECK_EQ_I((int)ip_len, (int)sizeof ip);
    CHECK(memcmp(ip_got, ip, sizeof ip) == 0);

    /* a CCP re-open must not re-key MPPE (that would reuse the RC4
     * keystream over new data): the link ends instead. A Configure-Request
     * in Opened makes CCP renegotiate; completing it drives ccp_up again. */
    push_cp(P_CCP, PPP_CONF_REQ, 40, ompe, 6);
    pull();
    if (scr_new)
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    ppp_tick(&S, 10);
    pull();
    CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);
}

/* bring MPPE up both ways on a fresh session (keys injected) */
static void mppe_open(ppp_config_t *cfg)
{
    static const uint8_t ompe[6] = { 18, 6, 0x01, 0x00, 0x00, 0x40 };
    unsigned i;
    for (i = 0; i < 16; i++) { inj_s[i] = (uint8_t)(i + 1); inj_r[i] = (uint8_t)(i + 101); }
    base_cfg(cfg);
    cfg->encrypt = 1;
    inject_mppe = 1;
    up(cfg);
    inject_mppe = 0;
    push_cp(P_CCP, PPP_CONF_REQ, 20, ompe, 6);
    pull();
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
}

/* the corners of MPPE found in review */
static void test_mppe_edges(void)
{
    static const uint8_t ompe[6] = { 18, 6, 0x01, 0x00, 0x00, 0x40 };
    static uint8_t big[1500];
    ppp_config_t cfg;
    uint32_t errs;
    unsigned ms;

    /* with encryption the compressors are off whatever comp says: the
     * server's request is MPPE, not Deflate (base_cfg leaves comp at its
     * default, Deflate and MPPC) */
    mppe_open(&cfg);
    CHECK_EQ_I(status()->comp_tx, PPP_COMP_MPPE);
    CHECK_EQ_I(status()->comp_rx, PPP_COMP_MPPE);

    /* MRU: MPPE adds 4 octets and has no native form to fall back on */
    CHECK_EQ_I(ppp_send_ip(&S, big, 1500 - PPP_MPPE_OVERHEAD + 1), PPP_ERR_SIZE);
    CHECK_EQ_I(ppp_send_ip(&S, big, 1500 - PPP_MPPE_OVERHEAD), 0);
    pull();

    /* a frame too short for MPPE is dropped and counted, with no CCP
     * Reset-Request (which could only lead to a renegotiation) */
    errs = status()->counters.comp_rx_errors;
    push(P_COMP, ompe, 2);
    pull();
    CHECK(find(P_CCP, 14) == NULL);
    CHECK(status()->counters.comp_rx_errors > errs);
    CHECK_EQ_I(status()->end_reason, PPP_END_NONE);

    /* the peer refuses our MPPE request but asks for MPPE itself: our
     * receive direction is not MPPE, so CCP opening must end the link, not
     * encrypt toward a peer that agreed to nothing of the kind */
    base_cfg(&cfg);
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;
    push_cp(P_CCP, PPP_CONF_REJ, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    if (scr_new)                                   /* the server's empty request */
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    push_cp(P_CCP, PPP_CONF_REQ, 21, ompe, 6);
    pull();
    ppp_tick(&S, 10);
    pull();
    CHECK(status()->comp_tx != PPP_COMP_MPPE);
    CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);

    /* a request with a second, short option 18 is not Acked (each copy is
     * judged with its own length) */
    base_cfg(&cfg);
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;
    {
        static const uint8_t two[10] = { 18, 6, 0x01, 0x00, 0x00, 0x40, 18, 4, 0x00, 0x40 };
        push_cp(P_CCP, PPP_CONF_REQ, 22, two, sizeof two);
        pull();
        CHECK(find(P_CCP, PPP_CONF_ACK) == NULL);
        CHECK(find(P_CCP, PPP_CONF_NAK) != NULL);
    }

    /* the peer has no CCP at all: with encryption required the link ends */
    base_cfg(&cfg);
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;
    {
        uint8_t rej[2 + 8] = { 0x80, 0xfd };
        memcpy(rej + 2, scr.d, 8);
        push_cp(PPP_PROTO_LCP, 8, 9, rej, sizeof rej);      /* LCP Protocol-Reject of CCP */
        pull();
        ppp_tick(&S, 10);
        pull();
        CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);
    }

    /* CCP never answered: after its whole retry span the link ends rather
     * than stay up carrying nothing */
    base_cfg(&cfg);
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;
    for (ms = 0; ms < 3000u * 10u && status()->end_reason == PPP_END_NONE; ms += 100) {
        ppp_tick(&S, 100);
        pull();
    }
    CHECK_EQ_I(status()->end_reason, PPP_END_NONE);            /* not before the span */
    for (; ms < 3000u * 12u && status()->end_reason == PPP_END_NONE; ms += 100) {
        ppp_tick(&S, 100);
        pull();
    }
    CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);
}

/* ---- a Windows dial-up client's MPPE, modelled from RFC 3078 (issue #6) ----
 *
 * Windows does stateless ("historyless", H bit) MPPE only on its VPN links;
 * on a modem or PPPoE link it asks for stateful MPPE, Naks a request for
 * stateless down to stateful, and cannot be talked into stateless (pppd's
 * require-mppe-128 without mppe-stateful loses Windows PPPoE clients the
 * same way: linux-kernel 2011-03, "MPPE stateful mode problem").
 *
 * Its stateful data path, as RFC 3078 has it and as that report saw it:
 * one RC4 stream per key; the key changes on "flag" packets (count low
 * octet 0xFF), which Windows sends WITHOUT the FLUSHED bit; FLUSHED means
 * only "RC4 started again from the current key" (set on its first packet
 * and after a CCP Reset-Request, which it answers with no Reset-Ack and
 * Code-Rejects one sent to it). Written here apart from ppp_mppe.c; only
 * the key derivation (proven against the kernel) and RC4 are shared. */
typedef struct {
    uint8_t start[16], session[16];
    uint16_t count;
    ppp_rc4_t rc4;
    int flush_next;     /* tx: RC4 again from the current key, FLUSHED set */
    int discard;        /* rx: lost step, waiting for FLUSHED (RFC 3078 8.2) */
} win_dir_t;

static void win_begin(win_dir_t *d, const uint8_t start[16])
{
    memcpy(d->start, start, 16);
    ppp_mppe_initial_key(d->start, d->session, 16, PPP_MPPE_FORCE_NONE);
    ppp_rc4_init(&d->rc4, d->session, 16);
    d->count = 0xfff;
    d->flush_next = 1;  /* Windows sets FLUSHED on its first packet */
    d->discard = 0;
}

static size_t win_encrypt(win_dir_t *d, const uint8_t *in, size_t n, uint8_t *out)
{
    unsigned hdr = PPP_MPPE_ENCRYPT;
    d->count = (uint16_t)((d->count + 1) & 0xfff);
    if ((d->count & 0xff) == 0xff) {         /* flag packet: new key, no FLUSHED */
        ppp_mppe_change_key(d->start, d->session, 16, PPP_MPPE_FORCE_NONE);
        ppp_rc4_init(&d->rc4, d->session, 16);
    }
    if (d->flush_next) {
        ppp_rc4_init(&d->rc4, d->session, 16);
        hdr |= PPP_MPPE_FLUSHED;
        d->flush_next = 0;
    }
    hdr |= d->count;
    out[0] = (uint8_t)(hdr >> 8); out[1] = (uint8_t)hdr;
    ppp_rc4(&d->rc4, in, out + 2, n);
    return n + 2;
}

/* 0 decrypted, -1 dropped (lost step: the caller would send a Reset-Request) */
static int win_decrypt(win_dir_t *d, const uint8_t *in, size_t n, uint8_t *out)
{
    unsigned hdr = (unsigned)in[0] << 8 | in[1], c = hdr & 0xfff;
    int flushed = (hdr & PPP_MPPE_FLUSHED) != 0;
    if (!d->discard && c == ((d->count + 1u) & 0xfff)) {
        d->count = (uint16_t)c;
        if ((c & 0xff) == 0xff) {
            ppp_mppe_change_key(d->start, d->session, 16, PPP_MPPE_FORCE_NONE);
            ppp_rc4_init(&d->rc4, d->session, 16);
        }
        if (flushed)
            ppp_rc4_init(&d->rc4, d->session, 16);
    } else if (!flushed) {
        d->discard = 1;
        return -1;
    } else {
        /* the flag packets in between changed the key, and this one if it is one */
        while (d->count != c) {
            d->count = (uint16_t)((d->count + 1) & 0xfff);
            if ((d->count & 0xff) == 0xff)
                ppp_mppe_change_key(d->start, d->session, 16, PPP_MPPE_FORCE_NONE);
        }
        ppp_rc4_init(&d->rc4, d->session, 16);
        d->discard = 0;
    }
    ppp_rc4(&d->rc4, in + 2, out, n - 2);
    return 0;
}

/* CCP as a Windows dial-up client negotiates it; `ask` is what its
 * Configure-Request carries on option 18 (0x60: 40 or 128 bit, "require
 * encryption"; 0x40: "maximum strength"; 0x61: "optional", with software
 * compression on), `nak` what it answers a request for stateless with */
static void win_ccp(uint32_t ask, uint32_t nak)
{
    static const uint8_t s128[6] = { 18, 6, 0, 0, 0, 0x40 };   /* stateful 128 */
    uint8_t req[6] = { 18, 6, 0, 0, 0, 0 }, nk[6] = { 18, 6, 0, 0, 0, 0 };
    frame_t *f;
    req[2] = (uint8_t)(ask >> 24); req[3] = (uint8_t)(ask >> 16); req[4] = (uint8_t)(ask >> 8); req[5] = (uint8_t)ask;
    nk[2] = (uint8_t)(nak >> 24); nk[3] = (uint8_t)(nak >> 16); nk[4] = (uint8_t)(nak >> 8); nk[5] = (uint8_t)nak;

    /* its request: the server Naks it down to one key length, in the mode
     * it asked for (RFC 3078 section 2: the strongest; stateless only if
     * the peer asked for it) */
    push_cp(P_CCP, PPP_CONF_REQ, 5, req, sizeof req);
    pull();
    f = find(P_CCP, ask == 0x40 ? PPP_CONF_ACK : PPP_CONF_NAK);   /* stateful 128 as it is */
    CHECK(f && same(f, s128, 6));
    if (f && !same(f, s128, 6))
        printf("    Nak of the Windows request: %02x %02x %02x %02x\n", f->d[6], f->d[7], f->d[8], f->d[9]);
    /* the server's request for stateless: Windows Naks it toward stateful */
    CHECK(scr_new);
    scr_new = 0;
    push_cp(P_CCP, PPP_CONF_NAK, scr.d[1], nk, sizeof nk);
    pull();
    CHECK(scr_new && same(&scr, s128, 6));
    if (scr_new && !same(&scr, s128, 6))
        printf("    the server's request after the Nak: %u octets\n", (unsigned)scr.n);
    /* both sides now agree stateful 128-bit */
    push_cp(P_CCP, PPP_CONF_REQ, 6, s128, 6);
    pull();
    f = find(P_CCP, PPP_CONF_ACK);
    CHECK(f && same(f, s128, 6));
    push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    ppp_tick(&S, 10);
    pull();
}

static void test_mppe_windows(void)
{
    static const uint32_t asks[3][2] = {
        { 0x00000060, 0x00000040 },     /* XP / 11 "require encryption" */
        { 0x00000040, 0x00000040 },     /* "require maximum strength" */
        { 0x00000061, 0x00000041 },     /* "optional encryption", MPPC asked too */
    };
    ppp_config_t cfg;
    win_dir_t wtx, wrx;     /* the client's send and receive directions */
    static uint8_t ip[200], plain[202], enc[210], pt[210];
    unsigned i, k, good, flags;
    size_t n;
    frame_t *f;

    for (k = 0; k < 3; k++) {
        for (i = 0; i < 16; i++) { inj_s[i] = (uint8_t)(i + 1); inj_r[i] = (uint8_t)(i + 101); }
        base_cfg(&cfg);
        cfg.encrypt = 1;
        inject_mppe = 1;
        up(&cfg);
        inject_mppe = 0;
        win_ccp(asks[k][0], asks[k][1]);
        CHECK_EQ_I(status()->end_reason, PPP_END_NONE);
        CHECK(status()->ccp_state == PPP_FSM_OPENED);
        CHECK_EQ_I(status()->comp_tx, PPP_COMP_MPPE);
        CHECK_EQ_I(status()->comp_rx, PPP_COMP_MPPE);
    }
    if (status()->comp_tx != PPP_COMP_MPPE)
        return;

    win_begin(&wtx, inj_r);     /* the client sends with the server's receive key */
    win_begin(&wrx, inj_s);
    for (i = 0; i < sizeof ip; i++) ip[i] = (uint8_t)(i * 7 + 3);
    ip[0] = 0x45;

    /* 700 packets each way: across the flag packets 0x0FF, 0x1FF, 0x2FF */
    good = 0; flags = 0;
    for (k = 0; k < 700; k++) {
        ip[1] = (uint8_t)k; ip[2] = (uint8_t)(k >> 8);
        CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
        ppp_tick(&S, 50);
        pull();
        f = find(P_COMP, 0);
        if (f && f->n == sizeof ip + 4) {
            unsigned c = ((unsigned)f->d[0] << 8 | f->d[1]) & 0xfff;
            if ((c & 0xff) == 0xff) flags++;
            if (win_decrypt(&wrx, f->d, f->n, pt) == 0 && pt[0] == 0 && pt[1] == 0x21 &&
                memcmp(pt + 2, ip, sizeof ip) == 0)
                good++;
        }
    }
    CHECK_EQ_I(good, 700);
    CHECK_EQ_I(flags, 2);
    good = 0;
    for (k = 0; k < 700; k++) {
        ip[1] = (uint8_t)k; ip[2] = (uint8_t)(k >> 8);
        plain[0] = 0; plain[1] = 0x21;
        memcpy(plain + 2, ip, sizeof ip);
        n = win_encrypt(&wtx, plain, sizeof plain, enc);
        ip_count = 0;
        push(P_COMP, enc, n);
        if (ip_count == 1 && ip_len == sizeof ip && memcmp(ip_got, ip, sizeof ip) == 0) good++;
    }
    CHECK_EQ_I(good, 700);
    pull();
    CHECK(find(P_CCP, RESET_REQ) == NULL);

    /* a packet from the client is lost (RFC 3078 8.2): the next is dropped,
     * one Reset-Request goes out, and the client's next packet (FLUSHED)
     * brings the server back in step. Lost across a flag packet as well. */
    for (k = 0; k < 2; k++) {
        unsigned lose = k == 0 ? 1 : 300;
        for (i = 0; i < lose; i++)
            (void)win_encrypt(&wtx, plain, sizeof plain, enc);
        ip_count = 0;
        n = win_encrypt(&wtx, plain, sizeof plain, enc);
        push(P_COMP, enc, n);
        n = win_encrypt(&wtx, plain, sizeof plain, enc);
        push(P_COMP, enc, n);
        CHECK_EQ_I(ip_count, 0);
        pull();
        CHECK_EQ_I(count(P_CCP), 1);
        f = find(P_CCP, RESET_REQ);
        CHECK(f != NULL);
        /* the client answers by flushing (and with no Reset-Ack) */
        wtx.flush_next = 1;
        n = win_encrypt(&wtx, plain, sizeof plain, enc);
        push(P_COMP, enc, n);
        CHECK_EQ_I(ip_count, 1);
        CHECK(ip_count == 1 && memcmp(ip_got, ip, sizeof ip) == 0);
        n = win_encrypt(&wtx, plain, sizeof plain, enc);
        push(P_COMP, enc, n);
        CHECK_EQ_I(ip_count, 2);
    }

    /* the client lost one of ours and asks for a reset: no Reset-Ack (it
     * would Code-Reject one), and the server's next packet resynchronises
     * it. RC4 is not started over on the old key (the same keystream again):
     * the server moves on to the next flag packet, with a new key. */
    CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
    pull();                                     /* this one is lost */
    CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
    pull();
    f = find(P_COMP, 0);
    CHECK(f && win_decrypt(&wrx, f->d, f->n, pt) == -1);
    push_cp(P_CCP, RESET_REQ, 77, NULL, 0);
    pull();
    /* a Reset-Ack goes out (the Linux kernel waits for one) until the
     * client Code-Rejects it, as Windows does; then no more of them */
    f = find(P_CCP, RESET_ACK);
    CHECK(f && f->d[1] == 77);
    if (f) {
        uint8_t rej[4];
        memcpy(rej, f->d, 4);
        push_cp(P_CCP, PPP_CODE_REJ, 3, rej, 4);
    }
    push_cp(P_CCP, RESET_REQ, 78, NULL, 0);     /* the same request again */
    pull();
    CHECK(find(P_CCP, RESET_ACK) == NULL);
    CHECK_EQ_I(status()->ccp_state, PPP_FSM_OPENED);
    CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
    ppp_tick(&S, 50);
    pull();
    f = find(P_COMP, 0);
    CHECK(f != NULL);
    if (f) {
        unsigned hdr = (unsigned)f->d[0] << 8 | f->d[1];
        CHECK(hdr & PPP_MPPE_FLUSHED);
        CHECK_EQ_I(hdr & 0xff, 0xff);           /* a flag packet: a new key */
        CHECK_EQ_I(win_decrypt(&wrx, f->d, f->n, pt), 0);
        CHECK(pt[0] == 0 && pt[1] == 0x21 && memcmp(pt + 2, ip, sizeof ip) == 0);
    }
    CHECK_EQ_I(ppp_send_ip(&S, ip, sizeof ip), 0);
    ppp_tick(&S, 50);
    pull();
    f = find(P_COMP, 0);
    CHECK(f && win_decrypt(&wrx, f->d, f->n, pt) == 0 && memcmp(pt + 2, ip, sizeof ip) == 0);
    CHECK_EQ_I(status()->end_reason, PPP_END_NONE);
    CHECK_EQ_I(status()->ip_up, 1);

    /* a client that will not do 128-bit at all (40-bit only): fail closed */
    base_cfg(&cfg);
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;
    {
        static const uint8_t l40[6] = { 18, 6, 0, 0, 0, 0x20 };
        push_cp(P_CCP, PPP_CONF_NAK, scr.d[1], l40, 6);
        pull();
        if (scr_new)
            push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
        push_cp(P_CCP, PPP_CONF_REQ, 9, l40, 6);
        pull();
        CHECK(find(P_CCP, PPP_CONF_ACK) == NULL);
        for (k = 0; k < 400 && status()->end_reason == PPP_END_NONE; k++) {
            ppp_tick(&S, 100);
            pull();
        }
        CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);
        CHECK(status()->comp_tx != PPP_COMP_MPPE);
    }
}

/* MPPE fails closed: with encryption required, no cleartext datagram is sent
 * or accepted, and if the peer will not do MPPE the link is ended. */
static void test_mppe_required(void)
{
    ppp_config_t cfg;
    unsigned i;
    uint32_t disc;

    for (i = 0; i < 16; i++) { inj_s[i] = (uint8_t)(i + 1); inj_r[i] = (uint8_t)(i + 101); }
    base_cfg(&cfg);
    cfg.comp = 0;
    cfg.encrypt = 1;
    inject_mppe = 1;
    up(&cfg);
    inject_mppe = 0;

    /* IPCP is up but MPPE is not yet: no cleartext datagram out... */
    CHECK_EQ_I(ppp_send_ip(&S, (const uint8_t *)"abcd", 4), PPP_ERR_DOWN);
    /* ...nor in */
    disc = status()->counters.rx_discards;
    ip_count = 0;
    push(PPP_PROTO_IP, (const uint8_t *)"abcd", 4);
    CHECK_EQ_I(ip_count, 0);
    CHECK(status()->counters.rx_discards > disc);

    /* the peer refuses MPPE and offers nothing of its own: the link ends
     * rather than carry traffic in the clear */
    CHECK(scr_new);
    push_cp(P_CCP, PPP_CONF_REJ, scr.d[1], scr.d + 4, scr.n - 4);
    pull();
    if (scr_new)                                   /* the server's follow-up (empty) request */
        push_cp(P_CCP, PPP_CONF_ACK, scr.d[1], scr.d + 4, scr.n - 4);
    push_cp(P_CCP, PPP_CONF_REQ, 30, NULL, 0);     /* peer requests no compression/encryption */
    pull();
    ppp_tick(&S, 10);
    pull();
    /* the link is ending/ended because encryption could not be established */
    CHECK_EQ_I(status()->end_reason, PPP_END_ENCRYPT_REQUIRED);
    CHECK(status()->phase == PPP_PHASE_TERMINATE || status()->phase == PPP_PHASE_DEAD);
    /* and still no cleartext datagram may be sent */
    CHECK_EQ_I(ppp_send_ip(&S, (const uint8_t *)"abcd", 4), PPP_ERR_DOWN);
}

int main(void)
{
    test_negotiation();
    test_mppe();
    test_mppe_required();
    test_mppe_edges();
    test_mppe_windows();
    test_deflate_data();
    test_only_ip_is_compressed();
    test_deflate_reset();
    test_reset_queue();
    test_down_up();
    test_mppc_data();
    test_mppc_reset_idle();
    test_comp_queue();
    test_mru();
    test_credit();
    test_fuzz();
    T_REPORT("test_ccp");
}
