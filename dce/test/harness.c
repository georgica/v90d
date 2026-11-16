/* Test side only: the simulated line and the traffic generator shared by the
 * call tests. See harness.h. */
#include <string.h>
#include <math.h>
#include "harness.h"
#include "gen.h"
#include "channel.h"
#include "g711.h"
#include "pcmenc.h"
#include "v90_pcm.h"

#define OUR_IP   HARNESS_OUR_IP
#define PEER_IP  HARNESS_PEER_IP
#define DNS1     HARNESS_DNS1

/* ------------------------------------------------------------ the line */

const line_t line_clean = { -55.0, 0, 0, 0, 0.2 };
const line_t line_no_echo = { -55.0, 0, 0, 0, 0.0 };

dce_t     D;
caller_t  C;
static channel_t CH;
static line_t    L;
long      now;          /* samples since the call began */
static uint8_t   s_oct;   /* the octet the DCE is sending */
static double    c_tx;    /* the caller's line signal */
int       harness_linear, harness_alaw;
int       bursts_on;
static uint32_t  burst_rng;
long      n_bursts;

static int16_t clamp16(long v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static int burst_noise(void)
{
    burst_rng = burst_rng * 1664525u + 1013904223u;
    return (int)((burst_rng >> 16) % (unsigned)(2 * L.burst_amp + 1)) - L.burst_amp;
}

/* One 8 kHz sample for everybody. The DCE sits at the G.711 interface:
 * its octet goes into the channel as it is (and straight to a V.90 caller,
 * see p34a.h), and it gets the octet the network A/D made of the line. With
 * harness_linear the same call is run through dce_tick() and the conversion
 * is done here, as an owner with a linear codec would. */
void step(void)
{
    static const long SPMS = FS / 1000;
    double crx; uint8_t srv; int16_t srx;
    long crx_i;
    int burst = 0;

    /* a V.92 caller brings its own line (sim/v92call): octets both ways */
    if (C.cfg.v92) {
        static uint8_t c_oct;
        if (now == 0) c_oct = harness_alaw ? 0xD5 : 0xFF;
        dce_tick_pcm(&D, c_oct, &s_oct);
        c_oct = caller_tick_v92(&C, s_oct);
        now++;
        return;
    }

    /* the simulated network is mu-law; an A-law codec is put on it by
     * converting at both of its ports */
    channel_tick(&CH, harness_alaw ? linear_to_ulaw(v90_octet_linear(1, s_oct)) : s_oct, c_tx, &crx, &srv);
    srx = (int16_t)ulaw_to_linear(srv);
    crx_i = lround(crx);
    if (bursts_on && L.burst_every_ms > 0) {
        long ph = (now / SPMS) % L.burst_every_ms;
        if (ph < L.burst_ms) {
            if (ph == 0 && now % SPMS == 0) n_bursts++;
            srx = clamp16(srx + burst_noise());
            crx_i += burst_noise();
            burst = 1;
        }
    }
    if (harness_linear) {
        int16_t t;
        dce_tick(&D, srx, &t);
        s_oct = linear_to_ulaw(t);
    } else {
        dce_tick_pcm(&D, harness_alaw ? pcm_encode(1, srx) : burst ? linear_to_ulaw(srx) : srv, &s_oct);
    }
    c_tx = caller_tick_pcm(&C, clamp16(crx_i), s_oct);
    now++;
}

static dce_status_t ST;
dce_status_t *status(void) { dce_get_status(&D, &ST); return &ST; }

/* run until pred() holds; returns the milliseconds it took, -1 on timeout */
int run_until(int (*pred)(void), int limit_ms)
{
    long n, lim = (long)limit_ms * (FS / 1000);
    for (n = 0; n < lim; n++) {
        if (n % 8 == 0 && pred()) return (int)(n / 8);
        step();
    }
    return pred() ? limit_ms : -1;
}

void run_ms(int ms)
{
    long n, lim = (long)ms * (FS / 1000);
    for (n = 0; n < lim; n++) step();
}

int p_ip_up(void) { return status()->ip_up && C.ppp.ip_up; }
int p_ended(void) { return status()->state == DCE_ENDED; }

/* ------------------------------------------------------------- traffic */
flow_t F[2];
long   n_busy[2];

static const int sizes[] = { 1, 20, 40, 64, 100, 256, 576, 1000, 1500, 333 };
#define N_SIZES ((int)(sizeof sizes / sizeof sizes[0]))

static const char text[] =
    "GET /pub/v90d/index.html HTTP/1.0\r\nHost: dialup.example\r\nAccept: text/html, text/plain\r\n\r\n"
    "The quick brown fox jumps over the lazy dog while the modem sings its two tone song. ";

/* packet idx of a direction: always the same octets for the same triple */
static size_t gen(int dir, int kind, int idx, uint8_t *b)
{
    size_t len = (size_t)sizes[idx % N_SIZES], i;
    uint32_t x = 0x9e3779b9u * (uint32_t)(idx + 1) + (dir ? 0x5bd1e995u : 0x1234567u);

    if (kind == HTML) {
        gen_t g;
        g.s = x | 1u;
        len = idx % 8 == 7 ? 576 : idx % 8 == 3 ? 1000 + (size_t)(idx % 10) * 37 : 1500;
        gen_packet(&g, GEN_HTML, (uint32_t)idx * 1460u, b, len);
        /* told apart by arrived() even when packets are missing */
        b[4] = (uint8_t)(idx >> 8); b[5] = (uint8_t)idx; b[6] = (uint8_t)dir;
        return len;
    }
    if (kind == TEXT) {
        size_t off = (size_t)(idx * 7 + dir * 3) % (sizeof text - 1);
        for (i = 0; i < len; i++) b[i] = (uint8_t)text[(off + i) % (sizeof text - 1)];
        if (len >= 4) { b[0] = (uint8_t)idx; b[1] = (uint8_t)(idx >> 8); b[2] = (uint8_t)dir; }
    } else {
        for (i = 0; i < len; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            b[i] = (uint8_t)(x >> 11);
        }
    }
    return len;
}

/* With traffic_lossy (a line without error control that damages frames)
 * packets may be missing: the arrival is then also compared with the next
 * few packets of the sequence. No two of ten consecutive packets have the
 * same length, so a damaged packet is never taken for a later one. */
int traffic_lossy;
int traffic_idle_ms = 3000;

static void arrived(int dir, const uint8_t *pkt, size_t len)
{
    static uint8_t want[1600];
    flow_t *f = &F[dir];
    int skip, max_skip = traffic_lossy > 1 ? traffic_lossy : traffic_lossy ? N_SIZES - 1 : 0, ok = 0;

    for (skip = 0; skip <= max_skip && f->n_rx + f->n_lost + skip < f->want; skip++) {
        size_t wl = gen(dir, f->kind, f->base + f->n_rx + f->n_lost + skip, want);
        if (wl == len && memcmp(want, pkt, len) == 0) { ok = 1; break; }
    }
    if (ok) f->n_lost += skip; else f->n_bad++;
    f->n_rx++;
    f->bytes_rx += (long)len;
    f->t_last = now;
}

static void srv_ip(void *ctx, const uint8_t *pkt, size_t len) { (void)ctx; arrived(UP, pkt, len); }
static void cli_ip(void *ctx, const uint8_t *pkt, size_t len) { (void)ctx; arrived(DN, pkt, len); }

static int next_base;

/* Send n_dn packets down and n_up packets up as fast as the two ends take
 * them, until all have arrived or the limit passes. Fills rate[] with the
 * payload throughput of each direction in octets per second. Returns 1 when
 * everything arrived. */
int traffic(int kind, int n_dn, int n_up, int limit_ms, long rate[2])
{
    static uint8_t pkt[1600];
    long t0 = now, lim = now + (long)limit_ms * (FS / 1000), busy_at = now;
    int dir, seen = 0;

    memset(F, 0, sizeof F);
    F[DN].kind = F[UP].kind = kind;
    F[DN].base = F[UP].base = next_base;
    F[DN].want = n_dn; F[UP].want = n_up;
    next_base += (n_dn > n_up ? n_dn : n_up);

    while (now < lim && (F[DN].n_rx + F[DN].n_lost < n_dn || F[UP].n_rx + F[UP].n_lost < n_up)) {
        /* lossy: the last packets may be among the lost; stop once
         * everything has been sent and nothing has arrived for 3 s */
        if (F[DN].n_tx + F[UP].n_tx + F[DN].n_rx + F[UP].n_rx != seen || F[DN].n_tx < n_dn || F[UP].n_tx < n_up) {
            seen = F[DN].n_tx + F[UP].n_tx + F[DN].n_rx + F[UP].n_rx;
            busy_at = now;
        } else if (traffic_lossy && now - busy_at > (long)traffic_idle_ms * (FS / 1000)) {
            F[DN].n_lost = n_dn - F[DN].n_rx; F[UP].n_lost = n_up - F[UP].n_rx;
            break;
        }
        if (now % 8 == 0) {
            /* offer packets until the sender says "busy" */
            while (F[DN].n_tx < n_dn) {
                size_t len = gen(DN, kind, F[DN].base + F[DN].n_tx, pkt);
                int rc = dce_send_ip(&D, pkt, len);
                if (rc == PPP_ERR_FULL) { n_busy[DN]++; break; }
                if (rc != 0) return 0;
                F[DN].n_tx++;
            }
            while (F[UP].n_tx < n_up) {
                size_t len = gen(UP, kind, F[UP].base + F[UP].n_tx, pkt);
                int rc = caller_send_ip(&C, pkt, len);
                if (rc == -2) { n_busy[UP]++; break; }
                if (rc != 0) return 0;
                F[UP].n_tx++;
            }
        }
        step();
    }
    for (dir = 0; dir < 2; dir++) {
        long dt = F[dir].t_last - t0;
        rate[dir] = dt > 0 ? F[dir].bytes_rx * FS / dt : 0;
    }
    return F[DN].n_rx + F[DN].n_lost == n_dn && F[UP].n_rx + F[UP].n_lost == n_up;
}

/* ------------------------------------------------------------ call setup */
static const ppp_user_t users[] = { { "alice", "wonderland" }, { "bob", "hunter2" } };

void server_cfg(dce_cfg_t *c)
{
    dce_cfg_default(c);
    c->ppp.auth = PPP_AUTH_CHAP | PPP_AUTH_PAP;
    c->ppp.hostname = "v90d-test";
    c->ppp.users = users;
    c->ppp.n_users = 2;
    c->ppp.our_ip = OUR_IP;
    c->ppp.peer_ip = PEER_IP;
    c->ppp.dns1 = DNS1;
    c->ppp.seed = 0xfeedbeef;
    c->ppp.ip_rx = srv_ip;
}

void client_cfg(caller_cfg_t *c)
{
    caller_cfg_default(c);
    c->ip_rx = cli_ip;
}

void call_begin(const dce_cfg_t *sc, const caller_cfg_t *cc, const line_t *line)
{
    static const double one[1] = { 1.0 };
    channel_cfg cfg;

    L = *line;
    memset(&cfg, 0, sizeof cfg);
    cfg.ds_noise_dbfs = cfg.us_noise_dbfs = L.noise_dbfs;
    cfg.ds_taps = one; cfg.ds_ntaps = 1; cfg.us_taps = one; cfg.us_ntaps = 1;
    cfg.echo_gain = L.echo_gain; cfg.echo_delay = 21; cfg.full_scale = 32124.0; cfg.seed = 17;
    channel_init(&CH, &cfg);
    now = 0; s_oct = 0xFF; c_tx = 0; bursts_on = 0; burst_rng = 12345; n_bursts = 0;
    memset(F, 0, sizeof F);
    n_busy[0] = n_busy[1] = 0;
    next_base = 0;                  /* every call sees the same packets */
    dce_init(&D, sc);
    caller_init(&C, cc);
}

