/* Replays a line recording made by `v90d -w` through the server core on the
 * host and reports what the modem made of it.   replay CONF RECORDING
 * Two kinds of recording (srv/main.c): per 10 ms block 80 codewords heard and
 * 80 sent; or, from a line with linear receive, the 8 octets "V90DLIN1" and
 * then per block 80 little-endian 16-bit samples heard and 80 codewords sent.
 * V90D_MAX_TX_CODE in the environment: the daemon's -P of that call. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "v90_pcm.h"
#include "../srv/srv.h"
#include "v90modem_priv.h"
#include "trace.h"     /* libv90modem/tools: the modem's side of the report */
static void ip(void *c, const uint8_t *p, size_t n) { (void)c; (void)p; (void)n; }
static long dsec[120];
static FILE *drx, *dtx;
void dce_trace_rx(int bit) { if (!tr_on) return; if (!drx) drx = fopen("live/data_rx.txt", "w"); fputc('0' + (bit & 1), drx); }
void dce_trace_tx(int bit) { if (!tr_on) return; if (!dtx) dtx = fopen("live/data_tx.txt", "w"); fputc('0' + (bit & 1), dtx); }
static int rp_ones(void *u) { (void)u; return 1; }
static void rp_drop(void *u, int bit) { (void)u; (void)bit; }
#define SIL(x) ((x) == 0xFF || (x) == 0x7F)
/* The answer moment is not the same in a replay (the ringing has a random
 * phase), so the recording is shifted to make the answer tones coincide: the
 * answer tone is the first sound that follows exactly 200 ms of silence. */
/* the recording: bs octets per block, the codewords sent from octet txo */
static int rec_lin; static long bs = 160, txo = 80;
static uint8_t rec_sent(const uint8_t *d, long i) { return d[(i / 80) * bs + txo + i % 80]; }
static uint8_t rec_heard(const uint8_t *d, long i) { return d[(i / 80) * bs + i % 80]; }
static int16_t rec_heard_lin(const uint8_t *d, long i)
{
    const uint8_t *q = d + (i / 80) * bs + 2 * (i % 80);
    return (int16_t)(uint16_t)(q[0] | (q[1] << 8));
}
static long ansam_in_recording(const uint8_t *d, long n)
{
    long i, run = 0;
    for (i = 0; i < n; i++) {
        uint8_t tx = rec_sent(d, i);
        if (SIL(tx)) run++; else { if (run >= 1590 && run <= 1612 && i > 8000) return i; run = 0; }
    }
    return -1;
}
int main(int argc, char **argv)
{
    static srv_t S; static cfg_t c; static uint8_t file[(12 << 20) + 8]; char err[200]; const uint8_t *d = file;
    long n, len, diff = 0, first = -1, rec_ans, shift = 0, pass; int last_tel = -1, i;
    FILE *f;
    if (argc < 3 || cfg_load(&c, argv[1], err, sizeof err) == CFG_ERR || !(f = fopen(argv[2], "rb"))) { fprintf(stderr, "usage: replay CONF RECORDING\n"); return 1; }
    len = (long)fread(file, 1, sizeof file, f);
    if (len >= 8 && memcmp(file, "V90DLIN1", 8) == 0) { rec_lin = 1; bs = 240; txo = 160; d = file + 8; len -= 8; }
    len = len / bs * 80;
    printf("recording: %s receive, %ld samples\n", rec_lin ? "16-bit linear" : "companded", len);
    rec_ans = ansam_in_recording(d, len);
    for (pass = 0; pass < 2; pass++) {
        long src = 0; int skipped = 0;
        srv_init(&S, &c, 0, ip, NULL);
        srv_set_rx_linear(&S, rec_lin);
        if (getenv("V90D_ECHO_DELAY")) srv_set_echo_delay(&S, atoi(getenv("V90D_ECHO_DELAY")));   /* the daemon's -d */
        if (getenv("V90D_MAX_TX_CODE")) srv_set_max_tx_code(&S, atoi(getenv("V90D_MAX_TX_CODE")));
        for (i = 0; i < 8000; i++) (void)srv_tick(&S, 0, 0xFF);
        last_tel = -1; trace_reset();
        for (n = 0; src < len; n++) {
            uint8_t rx = 0xFF, want = 0xFF, tx; int16_t rxl = 0; answer_t *a = v90m__answer(S.dce.modem);
            /* the shift is taken up at 2.5 s, while the ringing tone plays;
             * for a caller still dialling then (a slow dialler, a late
             * start), when the number is in */
            if (shift > 0 && !skipped && n >= 20000 && S.tel.state >= TEL_CONNECTING) { src += shift; skipped = 1; }
            if (n >= 20000 && n < 20000 - shift) ; else { if (rec_lin) rxl = rec_heard_lin(d, src); else rx = rec_heard(d, src); want = rec_sent(d, src); src++; }
            rec_tx = (int16_t)v90_octet_linear(0, want); rec_tx_on = pass == 1 && S.tel.state == TEL_ANSWERED && a->phase == ANS_V34 && a->connected;
            if (rec_tx92 && pass == 1) {
                /* a recorded V.92 call in data mode: the layers above cannot
                 * follow it (their data is not the recording's), so the modem
                 * alone is run from here on, with the recorded octets as the
                 * echo reference */
                if (a->phase == ANS_V92 || a->phase == ANS_P3X) { a->u.v92.cfg.getbit = rp_ones; a->u.v92.cfg.putbit = rp_drop; }
                tx = rec_lin ? answer_tick_lin(a, rxl) : answer_tick_pcm(a, rx);
            } else
            tx = rec_lin ? srv_tick_lin(&S, 1, rxl) : srv_tick(&S, 1, rx);
            a = v90m__answer(S.dce.modem);     /* the modem is made when the first call is answered */
            tr_on = pass == 1;
            if (pass == 0) {
                if (S.tel.state == TEL_ANSWERED && a->phase == ANS_V8 && a->v8.state == V8A_ANSAM) { shift = rec_ans - n; break; }
                continue;
            }
            if (tx != want && S.tel.state == TEL_ANSWERED) { if (first < 0) first = src; diff++; dsec[(src / 8000) % 120]++; }
            if ((int)S.tel.state != last_tel) { last_tel = (int)S.tel.state; printf("%7.3f s  telephony state %d\n", src / 8000.0, last_tel); }
            if (S.tel.state != TEL_ANSWERED) continue;
            trace_a(a, src);
            {
                static int ld = -1, lv = -1, lr = -1; static long nb, pr; dce_status_t st;
                dce_get_status(&S.dce, &st);
                if ((int)st.state != ld || st.v42_state != lv || a->retrains != lr) {
                    ld = (int)st.state; lv = st.v42_state; lr = a->retrains;
                    printf("%7.3f s    call state %d, V.42 state %d, retrains %d, p34 state %d\n", src / 8000.0, ld, lv, lr, (int)a->p34.state);
                }
                if (a->phase == ANS_V90 && ++nb - pr >= 16000) { pr = nb;
                    printf("%7.3f s      data: receiver mse %.1f dB, LAPM rx bad %lu, rx I %lu, tx I %lu, p34 state %d\n", src / 8000.0, 10 * log10(a->p34.rx.mse / 65536.0 + 1e-9), st.lapm_rx_bad, st.lapm_rx_i, st.lapm_tx_i, (int)a->p34.state); }
            }
            trace_b(a, src);
        }
        if (pass == 0) printf("answer tone at %.3f s in the recording; shift %ld samples\n", rec_ans / 8000.0, shift);
    }
    for (i = 0; i < 120; i++) if (dsec[i]) printf("  second %d: %ld samples differ\n", i, dsec[i]);
    printf("%ld samples; after the answer our transmit differs from the recording in %ld (first at %.3f s)\n", len, diff, first / 8000.0);
    return 0;
}
