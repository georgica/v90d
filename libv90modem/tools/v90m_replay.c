/* v90m-replay: play a recorded call into the answering modem and report what
 * it made of it, phase by phase. The tool to run on a recording that comes
 * with a bug report (README: "Reporting a bug").
 *
 *   v90m-replay [-a] [-9] [-u BPS] [-d BPS] [-l DBM0] [-e SAMPLES] [-P CODE] RECORDING
 *
 *   -a          A-law line (mu-law otherwise)
 *   -9          V.92 offered (PCM upstream up to 48000 bit/s unless -u)
 *   -u, -d BPS  upstream / downstream rate ceilings (33600 or 48000; 56000)
 *   -l DBM0     transmit level (default -12)
 *   -e SAMPLES  the echo delay, as the recording modem was told (default: measured)
 *   -P CODE     INFO0d maximum power code, 1..31 (default 23)
 * The modem must be set up as the recording modem was: what it hears holds
 * the echo of what that modem sent. "our transmit differs from the
 * recording in 0" at the end says it was.
 *
 * RECORDING: per 10 ms block 80 codewords heard then 80 sent; or the octets
 * "V90DLIN1" and then per block 80 little-endian 16-bit linear samples heard
 * and 80 codewords sent (a codec with linear receive).
 *
 * Built from the sources with the modem's trace hooks in (-DP34D_TRACE);
 * the lines it prints come from tools/trace.c, shared with v90d's server
 * replay. V92_TRACE=FILE in the environment: a line per sample of the V.92
 * engine. bits.txt: the bits the V.90 / V.34 receiver decided. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "v90modem_priv.h"
#include "v90_pcm.h"
#include "trace.h"

typedef struct { const uint8_t *d; long n, bs, txo; int lin; } rec_t;
static uint8_t rec_sent(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + r->txo + i % 80]; }
static uint8_t rec_heard(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + i % 80]; }
static int16_t rec_heard_lin(const rec_t *r, long i)
{
    const uint8_t *q = r->d + (i / 80) * r->bs + 2 * (i % 80);
    return (int16_t)(uint16_t)(q[0] | (q[1] << 8));
}
static int silent(int alaw, uint8_t c) { return alaw ? (c & 0x7F) == 0x55 : (c & 0x7F) == 0x7F; }

/* the answer tone: the first sound sent after 200 ms of silence, more than
 * a second into the recording */
static long recorded_answer(const rec_t *r, int alaw)
{
    long i, run = 0;
    for (i = 0; i < r->n; i++) {
        if (silent(alaw, rec_sent(r, i))) run++;
        else { if (run >= 1590 && run <= 1612 && i > 8000) return i; run = 0; }
    }
    return -1;
}

static int level_rms(int dbm0)
{
    int32_t v = 15770;
    if (dbm0 > 0) dbm0 = 0;
    for (; dbm0 < 0; dbm0++) v = (v * 29205 + 16384) >> 15;
    return (int)v;
}

/* the data side: V.42, answering, from the moment the modem connects */
static v90m_ec_t *ec;
static int ec_on;
static long bits_rx, bits_tx;
static int get(void *u) { (void)u; bits_tx++; return ec_on ? v90m_ec_tx_bit(ec) : 1; }
static void put(void *u, int b) { (void)u; bits_rx++; if (ec_on) v90m_ec_rx_bit(ec, b); }
static int rp_ones(void *u) { (void)u; return 1; }
static void rp_drop(void *u, int bit) { (void)u; (void)bit; }

static int opt_alaw, opt_v92, opt_dbm0 = -12, opt_up, opt_down = 56000, opt_echo = -1, opt_code;

static void configure(v90m_cfg *c, int lin)
{
    v90m_cfg_default(c);
    c->modulations = V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | (opt_v92 ? V90M_MOD_V92 : 0);
    c->tx_level = level_rms(opt_dbm0);
    c->max_down_bps = opt_down;
    c->max_up_bps = opt_up ? opt_up : opt_v92 ? 48000 : 33600;
    c->alaw = opt_alaw;
    c->codec_linear_rx = lin;
    c->echo_delay = opt_echo;
    c->max_tx_code = opt_code;
    c->getbit = get;
    c->putbit = put;
}

int main(int argc, char **argv)
{
    static uint8_t file[(24 << 20) + 8];
    static long dsec[120];
    rec_t r; v90m_cfg c; v90m_ec_cfg ecc; v90m_t *m; answer_t *a;
    long len, ans, k0, i, diff = 0, first = -1;
    int arg, err, lv = -1, lr = -1;
    FILE *f;

    for (arg = 1; arg < argc && argv[arg][0] == '-' && argv[arg][1]; arg++) {
        const char *o = argv[arg];
        if (!strcmp(o, "-a")) opt_alaw = 1;
        else if (!strcmp(o, "-9")) opt_v92 = 1;
        else if (arg + 1 < argc && !strcmp(o, "-u")) opt_up = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-d")) opt_down = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-l")) opt_dbm0 = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-e")) opt_echo = atoi(argv[++arg]);
        else if (arg + 1 < argc && !strcmp(o, "-P")) opt_code = atoi(argv[++arg]);
        else break;
    }
    if (arg >= argc || !(f = fopen(argv[arg], "rb"))) {
        fprintf(stderr, "usage: v90m-replay [-a] [-9] [-u BPS] [-d BPS] [-l DBM0] [-e SAMPLES] [-P CODE] RECORDING\n");
        return 2;
    }
    len = (long)fread(file, 1, sizeof file, f);
    fclose(f);
    memset(&r, 0, sizeof r);
    r.d = file; r.bs = 160; r.txo = 80;
    if (len >= 8 && !memcmp(file, "V90DLIN1", 8)) { r.lin = 1; r.bs = 240; r.txo = 160; r.d = file + 8; len -= 8; }
    r.n = len / r.bs * 80;
    printf("recording: %s receive, %ld samples\n", r.lin ? "16-bit linear" : "companded", r.n);
    trace_bits_path = "bits.txt";

    configure(&c, r.lin);
    if (!(m = v90m_create(&c, &err))) { fprintf(stderr, "v90m_create: %d\n", err); return 1; }
    a = v90m__answer(m);
    /* where this modem starts its answer tone: run it on silence until it
     * sends its first sound, which is what recorded_answer() finds */
    for (k0 = 0; k0 < 8000 * 5; k0++) {
        uint8_t s = opt_alaw ? 0xD5 : 0xFF, t;
        v90m_process_g711(m, &s, &t, 1);
        if (!silent(opt_alaw, t)) break;
    }
    ans = recorded_answer(&r, opt_alaw);
    if (ans < 0) { fprintf(stderr, "no answer tone in the recording\n"); return 1; }
    printf("answer tone at %.3f s in the recording; the modem starts %.3f s before it\n", ans / 8000.0, k0 / 8000.0);

    v90m_ec_cfg_default(&ecc, 0);
    ec = v90m_ec_create(&ecc, &err);
    v90m_reset(m, &c);
    trace_reset();
    tr_on = 1;
    for (i = ans - k0; i < r.n; i++) {
        uint8_t want = 0xFF, tx; v90m_state st;
        if (i < 0) { uint8_t s = opt_alaw ? 0xD5 : 0xFF; v90m_process_g711(m, &s, &tx, 1); continue; }
        want = rec_sent(&r, i);
        /* the echo cancellers take what was really sent as their reference
         * once data moves (V.34, V.92): this modem's data are not the
         * recording's */
        rec_tx = (int16_t)v90_octet_linear(opt_alaw, want);
        rec_tx_on = a->phase == ANS_V34 && a->connected;
        if (rec_tx92 && (a->phase == ANS_V92 || a->phase == ANS_P3X)) { a->u.v92.cfg.getbit = rp_ones; a->u.v92.cfg.putbit = rp_drop; }
        if (r.lin) { int16_t x = rec_heard_lin(&r, i); v90m_process_lin_rx(m, &x, &tx, 1); }
        else { uint8_t x = rec_heard(&r, i); v90m_process_g711(m, &x, &tx, 1); }
        if (tx != want) { if (first < 0) first = i; diff++; dsec[(i / 8000) % 120]++; }

        trace_a(a, i);
        v90m_get_state(m, &st);
        if (st.connected && !ec_on) {
            v90m_ec_reset(ec, &ecc);
            ec_on = 1;
        }
        if (ec_on && i % 8 == 0) v90m_ec_tick(ec, 1);
        {
            int s = ec_on ? v90m_ec_state(ec) : -1;
            if (s != lv || a->retrains != lr) {
                lv = s; lr = a->retrains;
                printf("%7.3f s    V.42 state %d, retrains %d, p34 state %d\n", i / 8000.0, lv, lr, (int)a->p34.state);
            }
            if (ec_on && a->phase == ANS_V90 && i % 16000 == 0) {
                v90m_ec_status es; v90m_ec_get_status(ec, &es);
                printf("%7.3f s      data: receiver mse %.1f dB, LAPM rx bad %lu, rx I %lu, tx I %lu, p34 state %d\n", i / 8000.0,
                       10 * log10(a->p34.rx.mse / 65536.0 + 1e-9), es.rx_frames_bad, es.rx_i, es.tx_i, (int)a->p34.state);
            }
        }
        trace_b(a, i);
        if (st.end != V90M_END_NONE) { printf("%7.3f s  the modem stopped (%d)\n", i / 8000.0, (int)st.end); break; }
    }
    {
        v90m_stats sx; v90m_get_stats(m, &sx);
        printf("modulation %d, %d bit/s down, %d up, SNR %.1f dB, round trip %d, echo delay %d, %u retrains, %u renegotiations; %ld bits received\n",
               (int)sx.modulation, sx.tx_bps, sx.rx_bps, sx.snr_db10 / 10.0, sx.rtd, sx.echo_delay, sx.retrains, sx.renegotiations, bits_rx);
    }
    for (i = 0; i < 120; i++) if (dsec[i]) printf("  second %ld: %ld samples differ\n", i, dsec[i]);
    printf("%ld samples; after the answer our transmit differs from the recording in %ld (first at %.3f s)\n", r.n, diff, first / 8000.0);
    v90m_ec_destroy(ec);
    v90m_destroy(m);
    return 0;
}
