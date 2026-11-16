/* V.42 interop: our endpoint against spandsp's V.42 as an independent peer,
 * joined by a bit pipe. spandsp times itself in transmit bits at 28800 bit/s,
 * so our clock is ticked at that rate too. Host only.
 *
 * Covered: detection phase, XID, SABME/UA, data both ways octet-exact, with
 * spandsp originating (our answer side, the main case) and answering, with
 * and without detection, with V.42 bis (spandsp's own v42bis wrapped around
 * its data path by the test wrapper), with bit errors, and release from
 * either side. */
#include <string.h>
#include "test.h"
#include "v42.h"
#include "peer_spandsp_v42.h"

static uint32_t rs = 0xBADC0DEu;
static uint32_t rnd32(void)
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

#define DMAX 40000
typedef struct {
    uint8_t src[DMAX]; size_t len, pos;
    uint8_t got[DMAX]; size_t ngot;
} flow_t;
static flow_t to_sp, from_sp;           /* named from our side */

static v42ec_t E;
static v42ec_cfg_t cfg;
static uint32_t ber;                    /* error probability times 2^32, both ways */
static unsigned long errs;
static long bits, acc;
static int sp_paused;                   /* spandsp's user stops supplying data */

static void fill(flow_t *w, size_t len, int text)
{
    static const char *words[8] = { "carrier", "the", "frame", "answer", "line", "data", "octet", "retrain" };
    size_t i = 0;
    w->len = len; w->pos = 0; w->ngot = 0;
    while (i < len) {
        if (!text) w->src[i++] = (uint8_t)(rnd32() >> 9);
        else {
            const char *s = words[(rnd32() >> 7) & 7];
            while (*s && i < len) w->src[i++] = (uint8_t)*s++;
            if (i < len) w->src[i++] = ' ';
        }
    }
}

static int sp_get(void *user, uint8_t *buf, int max)
{
    size_t n = from_sp.len - from_sp.pos;
    (void)user;
    if (sp_paused) return 0;
    if (n > (size_t)max) n = (size_t)max;
    memcpy(buf, from_sp.src + from_sp.pos, n);
    from_sp.pos += n;
    return (int)n;
}

static void sp_put(void *user, const uint8_t *buf, int len)
{
    (void)user;
    if (to_sp.ngot + (size_t)len <= DMAX) memcpy(to_sp.got + to_sp.ngot, buf, (size_t)len);
    to_sp.ngot += (size_t)len;
}

static int flip(int bit)
{
    if (ber && rnd32() < ber) { errs++; return !bit; }
    return bit;
}

static void run_bits(long n)
{
    uint8_t tmp[512]; size_t k;
    while (n-- > 0) {
        int a = v42ec_tx_bit(&E), b = sp_v42_tx_bit();
        sp_v42_rx_bit(flip(a));
        v42ec_rx_bit(&E, flip(b));
        bits++;
        acc += 10;
        while (acc >= 288) { acc -= 288; v42ec_tick(&E, 1); }      /* 28.8 bits per ms */
        if ((bits & 31) == 0) {
            if (to_sp.pos < to_sp.len)
                to_sp.pos += v42ec_write(&E, to_sp.src + to_sp.pos, to_sp.len - to_sp.pos);
            while ((k = v42ec_read(&E, tmp, sizeof tmp)) > 0) {
                if (from_sp.ngot + k <= DMAX) memcpy(from_sp.got + from_sp.ngot, tmp, k);
                from_sp.ngot += k;
            }
        }
    }
}

static long ms_now(void) { return bits * 10 / 288; }

static int run_until_connected(int max_ms)
{
    long t0 = ms_now();
    while (ms_now() - t0 < max_ms) {
        if (v42ec_state(&E) == V42EC_CONNECTED && sp_v42_connected()) return (int)(ms_now() - t0);
        run_bits(29);
    }
    return -1;
}

static int done(void)
{
    return to_sp.ngot >= to_sp.len && from_sp.ngot >= from_sp.len && v42ec_tx_idle(&E);
}

static int run_until_done(int max_ms)
{
    long t0 = ms_now();
    while (ms_now() - t0 < max_ms) {
        if (done()) return (int)(ms_now() - t0);
        run_bits(29);
    }
    return done() ? max_ms : -1;
}

static void check_flow(const flow_t *w)
{
    CHECK_EQ_I(w->ngot, w->len);
    CHECK(w->ngot == w->len && memcmp(w->got, w->src, w->len) == 0);
}

/* one complete call. we_originate: our role. p0: see peer_spandsp_v42.h.
 * our_comp: whether we offer / accept V.42 bis. release_by_us: who hangs up. */
static void session(const char *name, int we_originate, int detect, int our_comp,
                    int p0, int p1, int p2, uint32_t ber_data, int release_by_us)
{
    v42ec_status_t s;
    int t_conn, t_data, t;

    bits = acc = 0; errs = 0; ber = 0; sp_paused = 0;
    fill(&to_sp, 0, 0); fill(&from_sp, 0, 0);
    v42ec_cfg_default(&cfg, we_originate);
    cfg.detect = detect;
    cfg.lapm.comp_tx = cfg.lapm.comp_rx = our_comp;
    cfg.lapm.comp_p1 = p1 ? p1 : 512; cfg.lapm.comp_p2 = p2 ? p2 : 6;
    v42ec_init(&E, &cfg);
    sp_v42_start(!we_originate, detect, p0, p1, p2, sp_get, sp_put, NULL);

    t_conn = run_until_connected(5000);
    CHECK(t_conn >= 0);
    CHECK_EQ_I(sp_v42_seen_connected(), 1);
    v42ec_get_status(&E, &s);
    CHECK_EQ_I(s.k_tx, 15); CHECK_EQ_I(s.n401_tx, 128);
    if (p0 > 0 && our_comp) {
        /* P0 bit 1 is initiator to responder, bit 2 the reverse (V.42 bis 5.1) */
        CHECK_EQ_I(s.comp_tx, we_originate ? (p0 & 1) != 0 : (p0 & 2) != 0);
        CHECK_EQ_I(s.comp_rx, we_originate ? (p0 & 2) != 0 : (p0 & 1) != 0);
        CHECK_EQ_I(s.comp_n2, p1); CHECK_EQ_I(s.comp_n7, p2);
    } else if (!our_comp) {
        CHECK_EQ_I(s.comp_tx, 0); CHECK_EQ_I(s.comp_rx, 0);
    }

    ber = ber_data;
    fill(&to_sp, 30000, 1); fill(&from_sp, 30000, ber_data ? 0 : 1);
    t_data = run_until_done(ber_data ? 400000 : 60000);
    CHECK(t_data >= 0);
    check_flow(&to_sp); check_flow(&from_sp);
    CHECK_EQ_I(v42ec_state(&E), V42EC_CONNECTED);
    CHECK(sp_v42_connected());
    ber = 0;
    v42ec_get_status(&E, &s);
    printf("  %-34s connect %4d ms, 30000 octets each way in %6d ms; line errors %lu;"
           " ours: I sent %lu (%lu again), REJ sent %lu got %lu, T401 %lu, F=1 unasked %lu\n",
           name, t_conn, t_data, errs, s.tx_i, s.tx_i_retx, s.tx_rej, s.rx_rej,
           s.t401_expiries, s.violations);

    /* idle for a while (longer than T401): must stay up */
    run_bits(29L * 3000);
    CHECK_EQ_I(v42ec_state(&E), V42EC_CONNECTED);
    CHECK(sp_v42_connected());

    if (release_by_us) v42ec_release(&E); else sp_v42_stop();
    for (t = 0; t < 3000 && !(v42ec_state(&E) == V42EC_DISCONNECTED && !sp_v42_connected()
                              && sp_v42_seen_disconnected()); t++)
        run_bits(29);
    CHECK_EQ_I(v42ec_state(&E), V42EC_DISCONNECTED);
    v42ec_get_status(&E, &s);
    CHECK_EQ_I(s.reason, release_by_us ? V42LAPM_R_LOCAL : V42LAPM_R_PEER_DISC);
    CHECK(!sp_v42_connected());
    CHECK(sp_v42_seen_disconnected() >= 1);
}

/* spandsp as answerer left as it is: its XID response always claims V.42 bis
 * P0=1, P1=512, P2=6, whatever the command asked for (it builds the response
 * from its own fixed configuration). When we asked for no compression that
 * response is illegal (a responder cannot turn on what was not requested),
 * and we must refuse the connection rather than run with the two ends
 * disagreeing about the data format. */
static void test_spandsp_xid_response(void)
{
    v42ec_status_t s; int t;
    bits = acc = 0; errs = 0; ber = 0; sp_paused = 0;
    fill(&to_sp, 0, 0); fill(&from_sp, 0, 0);
    v42ec_cfg_default(&cfg, 1);
    cfg.lapm.comp_tx = cfg.lapm.comp_rx = 0;
    v42ec_init(&E, &cfg);
    sp_v42_start(0, 1, 0, 0, 0, sp_get, sp_put, NULL);
    for (t = 0; t < 5000 && v42ec_state(&E) != V42EC_DISCONNECTED; t++) run_bits(29);
    v42ec_get_status(&E, &s);
    CHECK_EQ_I(s.state, V42EC_DISCONNECTED);
    CHECK_EQ_I(s.reason, V42LAPM_R_NEGOTIATION);
    printf("  originate, no V.42bis asked, spandsp unmodified: its XID response claims"
           " V.42bis anyway; refused (reason %d) after %ld ms\n", s.reason, ms_now());
}

int main(void)
{
    /* our answer side against spandsp originating: the case that matters */
    session("answer, plain", 0, 1, 0, 0, 0, 0, 0, 1);
    session("answer, plain, peer releases", 0, 1, 0, 0, 0, 0, 0, 0);
    session("answer, no detection phase", 0, 0, 0, 0, 0, 0, 0, 1);
    session("answer, V.42bis from peer only", 0, 1, 1, 1, 512, 6, 0, 1);
    session("answer, V.42bis both ways", 0, 1, 1, 3, 512, 6, 0, 0);
    session("answer, V.42bis 2048/32", 0, 1, 1, 3, 2048, 32, 0, 1);
    session("answer, plain, BER 1e-4", 0, 1, 0, 0, 0, 0, 429497u, 1);
    session("answer, V.42bis, BER 1e-4", 0, 1, 1, 3, 512, 6, 429497u, 1);
    /* and originating against spandsp answering */
    session("originate, plain", 1, 1, 0, -1, 0, 0, 0, 1);
    session("originate, peer releases", 1, 1, 0, -1, 0, 0, 0, 0);
    session("originate, no detection phase", 1, 0, 0, -1, 0, 0, 0, 1);
    session("originate, V.42bis both ways", 1, 1, 1, 3, 512, 6, 0, 1);
    session("originate, V.42bis to peer only", 1, 1, 1, 1, 512, 6, 0, 0);
    session("originate, plain, BER 1e-4", 1, 1, 0, -1, 0, 0, 429497u, 0);
    session("originate, V.42bis, BER 1e-4", 1, 1, 1, 3, 512, 6, 429497u, 1);
    test_spandsp_xid_response();
    T_REPORT("interop_v42_spandsp");
}
