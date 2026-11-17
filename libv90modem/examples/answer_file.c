/* Example: answer a recorded call with libv90modem and print the connect
 * line. Only the public API is used.
 *
 *   answer_file [-a] [-9] [-u BPS] [-l DBM0] RECORDING
 *
 * RECORDING is a call recorded at the codec (the format of tools/README.md):
 * per 10 ms block 80 codewords heard then 80 codewords sent; or, after the
 * 8 octets "V90DLIN1", 80 little-endian 16-bit linear samples heard then 80
 * codewords sent.
 *   -a        the line is A-law (mu-law otherwise)
 *   -9        V.92 offered (PCM upstream up to 48000 bit/s, unless -u)
 *   -u BPS    upstream rate ceiling (default 33600; with -9, 48000)
 *   -l DBM0   transmit level, dBm0 (default -12)
 * The defaults are those the recordings of v90d were made with (V.90, V.34,
 * V.32bis, V.22bis, V.22, V.21; 56000 down, 33600 up).
 *
 * The modem must be set up as the recording modem was: what it hears holds
 * the echo of what that modem sent, and its echo canceller can only take the
 * echo away when it sends the same.
 *
 * What was heard is played into a fresh answering modem. The recording also
 * holds what the recording modem sent, which tells when it started its
 * answer tone; this modem is started so that its own answer tone begins at
 * that same moment, or the caller's replies would come at the wrong time. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "v90modem.h"

static const char *const conn_name[] = { "none", "V.21", "V.22", "V.22bis", "V.90", "V.34", "V.32bis", "V.92" };

typedef struct { const uint8_t *d; long n, bs, txo; int lin; } rec_t;

static uint8_t rec_sent(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + r->txo + i % 80]; }
static uint8_t rec_heard(const rec_t *r, long i) { return r->d[(i / 80) * r->bs + i % 80]; }
static int16_t rec_heard_lin(const rec_t *r, long i)
{
    const uint8_t *q = r->d + (i / 80) * r->bs + 2 * (i % 80);
    return (int16_t)(uint16_t)(q[0] | (q[1] << 8));
}

/* the two codewords of silence: mu-law 0xFF / 0x7F, A-law 0xD5 / 0x55 */
static int silent(int alaw, uint8_t c) { return alaw ? (c & 0x7F) == 0x55 : (c & 0x7F) == 0x7F; }

/* the answer tone in the recording: the first sound sent after 200 ms of
 * silence, more than a second into the recording (before it: ringing) */
static long recorded_answer(const rec_t *r, int alaw)
{
    long i, run = 0;
    for (i = 0; i < r->n; i++) {
        if (silent(alaw, rec_sent(r, i))) run++;
        else { if (run >= 1590 && run <= 1612 && i > 8000) return i; run = 0; }
    }
    return -1;
}

static int bit_one(void *u) { (void)u; return 1; }
static void bit_count(void *u, int b) { (void)b; ++*(long *)u; }

static int opt_v92, opt_dbm0 = -12, opt_up;

/* dBm0 to the transmit RMS of v90m_cfg.tx_level: 0 dBm0 is 15770 at the
 * Table 1 scale, less 1 dB a step (10^(-1/20) in Q15) */
static int level_rms(int dbm0)
{
    int32_t v = 15770;
    if (dbm0 > 0) dbm0 = 0;
    for (; dbm0 < 0; dbm0++) v = (v * 29205 + 16384) >> 15;
    return (int)v;
}

static void configure(v90m_cfg *c, int alaw, int lin, long *bits)
{
    v90m_cfg_default(c);
    c->modulations = V90M_MOD_V21 | V90M_MOD_V22 | V90M_MOD_V32BIS | V90M_MOD_V34 | V90M_MOD_V90 | (opt_v92 ? V90M_MOD_V92 : 0);
    c->tx_level = level_rms(opt_dbm0);
    c->max_down_bps = 56000;
    c->max_up_bps = opt_up ? opt_up : opt_v92 ? 48000 : 33600;
    c->alaw = alaw;
    c->codec_linear_rx = lin;
    c->getbit = bit_one;            /* nothing to say: mark */
    c->putbit = bit_count;          /* count what arrives */
    c->user = bits;
}

/* where a fresh modem starts its answer tone, in samples */
static long own_answer(int alaw, int lin)
{
    v90m_cfg c; v90m_t *m; long k, bits = 0; int err;
    uint8_t rx = alaw ? 0xD5 : 0xFF, tx;
    configure(&c, alaw, lin, &bits);
    if (!(m = v90m_create(&c, &err))) return -1;
    for (k = 0; k < 8000 * 5; k++) {
        v90m_process_g711(m, &rx, &tx, 1);
        if (!silent(alaw, tx)) break;
    }
    v90m_destroy(m);
    return k;
}

int main(int argc, char **argv)
{
    static uint8_t file[24 << 20];
    rec_t r; v90m_cfg c; v90m_t *m; v90m_state st; v90m_stats sx;
    long len, ans, own, start, i, bits = 0;
    int alaw = 0, err, was_connected = 0, arg = 1;
    FILE *f;

    for (; argc > arg && argv[arg][0] == '-'; arg++) {
        if (!strcmp(argv[arg], "-a")) alaw = 1;
        else if (!strcmp(argv[arg], "-9")) opt_v92 = 1;
        else if (!strcmp(argv[arg], "-l") && argc > arg + 1) opt_dbm0 = atoi(argv[++arg]);
        else if (!strcmp(argv[arg], "-u") && argc > arg + 1) opt_up = atoi(argv[++arg]);
        else break;
    }
    if (argc <= arg || !(f = fopen(argv[arg], "rb"))) { fprintf(stderr, "usage: answer_file [-a] [-9] [-u BPS] [-l DBM0] RECORDING\n"); return 2; }
    len = (long)fread(file, 1, sizeof file, f);
    fclose(f);
    memset(&r, 0, sizeof r);
    r.d = file; r.bs = 160; r.txo = 80;
    if (len >= 8 && !memcmp(file, "V90DLIN1", 8)) { r.lin = 1; r.bs = 240; r.txo = 160; r.d = file + 8; len -= 8; }
    r.n = len / r.bs * 80;

    ans = recorded_answer(&r, alaw);
    own = own_answer(alaw, r.lin);
    if (ans < 0 || own < 0) { fprintf(stderr, "no answer tone in the recording\n"); return 1; }
    start = ans - own;              /* the recording sample our modem's first sample hears */
    printf("%s: %.1f s, %s receive; answer tone at %.3f s\n", argv[arg], r.n / 8000.0,
           r.lin ? "16-bit linear" : "companded", ans / 8000.0);

    configure(&c, alaw, r.lin, &bits);
    if (!(m = v90m_create(&c, &err))) { fprintf(stderr, "v90m_create: %d\n", err); return 1; }
    for (i = start; i < r.n; i++) {
        uint8_t tx;
        if (i < 0) { uint8_t s = alaw ? 0xD5 : 0xFF; v90m_process_g711(m, &s, &tx, 1); }
        else if (r.lin) { int16_t x = rec_heard_lin(&r, i); v90m_process_lin_rx(m, &x, &tx, 1); }
        else { uint8_t x = rec_heard(&r, i); v90m_process_g711(m, &x, &tx, 1); }
        v90m_get_state(m, &st);
        if (st.connected && !was_connected) {
            was_connected = 1;
            v90m_get_stats(m, &sx);
            printf("CONNECT %d/%d %s at %.3f s%s; SNR %.1f dB, round trip %d samples\n", st.tx_bps, st.rx_bps,
                   conn_name[st.modulation], i / 8000.0, st.used_v8 ? "" : " (no V.8)", sx.snr_db10 / 10.0, sx.rtd);
        }
        if (st.end != V90M_END_NONE) break;
    }
    v90m_get_state(m, &st);
    v90m_get_stats(m, &sx);
    if (!was_connected) printf("NO CARRIER: no connection was made\n");
    else printf("at the end: %s %d/%d, %u retrains, %u renegotiations, %ld bits received%s\n", conn_name[sx.modulation],
                sx.tx_bps, sx.rx_bps, sx.retrains, sx.renegotiations, bits,
                st.end == V90M_END_NONE ? "" : st.end == V90M_END_CLEARDOWN_FAR ? "; the caller cleared down" : "; the line was lost");
    v90m_destroy(m);
    return 0;
}
