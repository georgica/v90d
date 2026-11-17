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
static void ip(void *c, const uint8_t *p, size_t n) { (void)c; (void)p; (void)n; }
static const char *ph[] = { "V8", "V22", "V21", "FAILED", "P2", "P3", "V90", "RETRAIN", "V34", "V32", "V32_SIL", "P3X", "V92" };
static void menu(const char *n, const v8_menu_t *m)
{
    printf("  %s: call function %u, modes 0x%04x (%u octets), pcm %s0x%02x, protocol %s0x%02x, access %s0x%02x\n", n, m->call_function, m->modes,
           m->n_mod_octets, m->has_pcm ? "" : "absent ", m->pcm, m->has_prot ? "" : "absent ", m->prot, m->has_access ? "" : "absent ", m->access);
}
static long dsec[120];
static double tr_in, tr_out; static long tr_n, tr_t; static int tr_on;
void p34d_trace(const p34d_t *p, int16_t rxs, int16_t x)
{
    if (!tr_on) return;
    {
        static int had_dil, had_cpt; int i, c, k;
        if (p->state < P34_SD) had_dil = had_cpt = 0;
        if (!had_dil && p->state >= P34_SD) {
            const v90_dil_t *d = &p->dil; had_dil = 1;
            printf("      Ja: DIL N %d, Lsp %d, Ltp %d, H", d->n, d->lsp, d->ltp);
            for (i = 0; i < 8; i++) printf(" %d", d->h[i]);
            printf(", REF");
            for (i = 0; i < 8; i++) printf(" %d", d->ref[i]);
            printf("\n          SP");
            for (i = 0; i < d->lsp; i++) printf("%d", (d->sp[i >> 3] >> (i & 7)) & 1);
            printf(" TP");
            for (i = 0; i < d->ltp; i++) printf("%d", (d->tp[i >> 3] >> (i & 7)) & 1);
            printf("\n          training Ucodes");
            for (i = 0; i < d->n; i++) printf(" %d", d->train[i]);
            printf("\n");
        }
        if (!had_cpt && p->got_cpt) {
            const v90_cp_t *q = &p->cp; had_cpt = 1;
            printf("      CPt: is_cp %d drn %d silence %d Sr %d ack %d alaw %d rates 0x%04x ld %d rms_ratio %u a1 %d a2 %d b1 %d b2 %d idx %d %d %d %d %d %d codec_differs %d; upstream SNR %.1f dB\n",
                   q->is_cp, q->drn, q->silence, q->sr, q->ack, q->alaw, q->rates, q->ld, q->rms_ratio, q->a1, q->a2, q->b1, q->b2,
                   q->idx[0], q->idx[1], q->idx[2], q->idx[3], q->idx[4], q->idx[5], q->codec_differs, p->snr_db10 / 10.0);
            for (c = 0; c < 6; c++) {
                int n = 0; for (k = 0; k < 128; k++) n += (q->mask[c][k >> 3] >> (k & 7)) & 1;
                if (!n) continue;
                printf("          constellation %d: %d points:", c, n);
                for (k = 0; k < 128; k++) if ((q->mask[c][k >> 3] >> (k & 7)) & 1) printf(" %d", k);
                printf("\n");
                if (q->codec_differs) {
                    printf("          codec constellation %d:", c);
                    for (k = 0; k < 128; k++) if ((q->codec_mask[c][k >> 3] >> (k & 7)) & 1) printf(" %d", k);
                    printf("\n");
                }
            }
        }
    }
    {
        static int last_drn = -1, last_ack = -1, last_rate = -1;
        if (p->state < P34_RI) last_drn = last_ack = last_rate = -1;
        if (p->cp.is_cp && (p->cp.drn != last_drn || p->cp.ack != last_ack || p->down_rate != last_rate)) {
            int c, k, n = 0;
            last_drn = p->cp.drn; last_ack = p->cp.ack; last_rate = p->down_rate;
            for (k = 0; k < 128; k++) n += (p->cp.mask[0][k >> 3] >> (k & 7)) & 1;
            printf("      CP at %.3f s: drn %d (%d bit/s) ack %d Sr %d ld %d, constellation 0 has %d points; engine's down rate %d, state %d\n", tr_t / 8000.0, p->cp.drn,
                   (p->cp.drn + 20) * 4000 / 3, p->cp.ack, p->cp.sr, p->cp.ld, n, p->down_rate, (int)p->state);
            (void)c;
        }
    }
    tr_in += (double)rxs * rxs; tr_out += (double)x * x; tr_n++; tr_t++;
    if (tr_n == 1600) {
        printf("      p34 %6.2f s state %2d  line %7.0f rms -> after the canceller %7.0f rms (%.1f dB), delay %d, mu %d, sres_in_s %d\n", tr_t / 8000.0, (int)p->state,
               sqrt(tr_in / tr_n), sqrt(tr_out / tr_n), 10 * log10((tr_out + 1) / (tr_in + 1)), p->ecd.delay, p->ec_mu, sres_in_s(&p->sres));
        printf("          receiver: mode %d, mse %.1f dB, gain %d, symbols %d, timing %d/%d, freq %d, got_cpt %d, tx signal %d\n", (int)p->rxmode, 10 * log10(p->rx.mse / 65536.0 + 1e-9),
               (int)p->rx.gain, (int)p->rx.nsym, p->rx.timing_on, p->rx.timing_fast, (int)p->rx.freq, p->got_cpt, (int)v90tx_signal(&p->tx));
        tr_in = tr_out = 0; tr_n = 0;
    }
}
/* V92_TRACE=file in the environment: one line per sample (time, state, line,
 * after the canceller, our octet's signal) and per decided training symbol */
static FILE *v92f; static int v92f_tried;
static FILE *v92_file(void)
{
    if (!v92f_tried) { const char *n = getenv("V92_TRACE"); v92f_tried = 1; if (n) v92f = fopen(n, "w"); }
    return v92f;
}
/* the V.92 engine (v92/v92_p34d.c): the same view of its echo canceller, and
 * what its PCM upstream receiver makes of the caller */
void v92d_trace(const v92d_t *p, int16_t rxs, int16_t x)
{
    static double in, out; static long n;
    if (!tr_on) return;
    if (v92_file()) fprintf(v92f, "X %.5f st %d rx %d x %d sig %d p6 %d %d err %d\n", (tr_t + 1) / 8000.0, (int)p->state, rxs, x, (int)v90tx_signal(&p->tx), p->p6.run, p->p6.armed, p->error);
    in += (double)rxs * rxs; out += (double)x * x; n++; tr_t++;
    if (n == 1600) {
        printf("      v92 %6.2f s state %2d (error %d)  line %7.0f rms -> after the canceller %7.0f rms (%.1f dB), echo delay %d, mu %d\n", tr_t / 8000.0, (int)p->state, p->error,
               sqrt(in / n), sqrt(out / n), 10 * log10((out + 1) / (in + 1)), p->ecd.delay, p->ec_mu);
        printf("          receiver: quality %lu, bad frames %lu, up %d bit/s, down %d bit/s, renegotiations %d, rate drops %d, B1u level %lu, quiet blocks %d\n",
               (unsigned long)v92_dec_quality(&p->dec), (unsigned long)p->dec.bad_frames, p->up_rate, p->down_rate, p->renegs, p->rate_drops, (unsigned long)p->sig_ms, p->cd_quiet);
        in = out = 0; n = 0;
    }
}
void v92d_trace_sym(const v92d_t *p, int bits)
{
    if (!tr_on || !v92_file()) return;
    fprintf(v92f, "S %.5f st %d m %ld lev %d y %ld bits %d mse %lu cpk %d four %d\n", tr_t / 8000.0, (int)p->state, (long)p->trn.dec_m, p->trn.dec_level,
            (long)p->trn.dec_y, bits, (unsigned long)p->trn.mse, p->trn.cpk, p->four);
}
void v92d_trace_data(const v92d_t *p, int16_t sample)
{
    static long hdr = -1;
    if (!tr_on || !v92_file()) return;
    if (hdr != (long)p->m_b1u) { int j, k; const v92_map_t *mp = &p->cpd.map; const v92_filt_t *f = &p->cpd.filt; hdr = (long)p->m_b1u;
        fprintf(v92f, "C K %d trellis %d drn %d gain4 %u lz1 %d lp1 %d lz2 %d lp2 %d dly %d m_b1u %ld cpk %d\nM", mp->K, mp->trellis, p->cpd.drn, (unsigned)f->gain4, f->lz1, f->lp1, f->lz2, f->lp2, p->dsn.dly, (long)p->m_b1u, p->trn.cpk);
        for (j = 0; j < 12; j++) fprintf(v92f, " %d", mp->M[j]);
        fprintf(v92f, "\nP");
        for (k = 0; k < v92_lc(mp, 0); k++) fprintf(v92f, " %d", (int)v92_point(mp, 0, k));
        fprintf(v92f, "\nF");
        for (k = 0; k < f->lz1 + f->lp1 + f->lz2 + f->lp2; k++) fprintf(v92f, " %d", f->coef[k]);
        fprintf(v92f, "\nH");
        for (k = 0; k < V92_CH_NTAP; k++) fprintf(v92f, " %ld", (long)p->dsn.cfg.h[k]);
        fprintf(v92f, "\n");
    }
    fprintf(v92f, "D %ld %d\n", (long)p->data_k, sample);
}
static FILE *bitf;
void p34d_trace_bit(const p34d_t *p, int bit)
{
    static int last = -1;
    if (!tr_on) return;
    if (!bitf) bitf = fopen("live/bits.txt", "w");
    if ((int)p->state != last) { last = (int)p->state; fprintf(bitf, "\nstate %d at %.3f:\n", last, tr_t / 8000.0); }
    fputc('0' + bit, bitf);
}
static FILE *drx, *dtx;
void dce_trace_rx(int bit) { if (!tr_on) return; if (!drx) drx = fopen("live/data_rx.txt", "w"); fputc('0' + (bit & 1), drx); }
void dce_trace_tx(int bit) { if (!tr_on) return; if (!dtx) dtx = fopen("live/data_tx.txt", "w"); fputc('0' + (bit & 1), dtx); }
static int16_t rec_tx; static int rec_tx_on;
int16_t v34dx_trace_tx(int16_t y) { return rec_tx_on ? rec_tx : y; }
static int rec_tx92;
static int rp_ones(void *u) { (void)u; return 1; }
static void rp_drop(void *u, int bit) { (void)u; (void)bit; }
int16_t v92d_trace_tx(const v92d_t *p, int16_t y) { if (p->state == V92D_DATA) rec_tx92 = 1; return rec_tx92 && tr_on ? rec_tx : y; }
static const char *p2n[] = { "INFO0", "WAIT_REV1", "REPLY1", "WAIT_REV2", "RECOVER", "PROBE_RX", "WAIT_REV3", "REPLY3", "L1", "L2", "INFO1D", "WAIT_INFO1A", "DONE", "FAILED" };
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
    long n, len, diff = 0, first = -1, rec_ans, shift = 0, pass; int last_ph = -1, last_tel = -1, last_v8 = -1, i, last_p2 = -1, last_p34 = -1;
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
        last_ph = last_tel = last_v8 = last_p2 = -1;
        for (n = 0; src < len; n++) {
            uint8_t rx = 0xFF, want = 0xFF, tx; int16_t rxl = 0; answer_t *a = &S.dce.modem;
            /* the shift is taken up at 2.5 s, while the ringing tone plays;
             * for a caller still dialling then (a slow dialler, a late
             * start), when the number is in */
            if (shift > 0 && !skipped && n >= 20000 && S.tel.state >= TEL_CONNECTING) { src += shift; skipped = 1; }
            if (n >= 20000 && n < 20000 - shift) ; else { if (rec_lin) rxl = rec_heard_lin(d, src); else rx = rec_heard(d, src); want = rec_sent(d, src); src++; }
            rec_tx = (int16_t)v90_octet_linear(0, want); rec_tx_on = pass == 1 && S.tel.state == TEL_ANSWERED && S.dce.modem.phase == ANS_V34 && S.dce.modem.connected;
            if (rec_tx92 && pass == 1) {
                /* a recorded V.92 call in data mode: the layers above cannot
                 * follow it (their data is not the recording's), so the modem
                 * alone is run from here on, with the recorded octets as the
                 * echo reference */
                if (a->phase == ANS_V92 || a->phase == ANS_P3X) { a->u.v92.cfg.getbit = rp_ones; a->u.v92.cfg.putbit = rp_drop; }
                tx = rec_lin ? answer_tick_lin(a, rxl) : answer_tick_pcm(a, rx);
            } else
            tx = rec_lin ? srv_tick_lin(&S, 1, rxl) : srv_tick(&S, 1, rx);
            tr_on = pass == 1;
            if (pass == 0) {
                if (S.tel.state == TEL_ANSWERED && a->phase == ANS_V8 && a->v8.state == V8A_ANSAM) { shift = rec_ans - n; break; }
                continue;
            }
            if (tx != want && S.tel.state == TEL_ANSWERED) { if (first < 0) first = src; diff++; dsec[(src / 8000) % 120]++; }
            if ((int)S.tel.state != last_tel) { last_tel = (int)S.tel.state; printf("%7.3f s  telephony state %d\n", src / 8000.0, last_tel); }
            if (S.tel.state != TEL_ANSWERED) continue;
            if ((int)a->phase != last_ph) { last_ph = (int)a->phase; printf("%7.3f s  modem phase %s (V.34 engine state %d error %d)\n", src / 8000.0, ph[last_ph], (int)a->v34.dx.state, a->v34.dx.error); }
            if (a->phase == ANS_V8 && (int)a->v8.state != last_v8) {
                last_v8 = (int)a->v8.state; printf("%7.3f s    V.8 state %d\n", src / 8000.0, last_v8);
                if (last_v8 == V8A_JM) { menu("CM heard", &a->v8.cm); menu("JM sent ", &a->v8.jm); }
                if (last_v8 == V8A_DONE) printf("  V.8 result %d, selected %d\n", a->v8.result, a->v8.selected);
            }
            if (a->phase == ANS_P2 && (int)a->p2.state != last_p2) { last_p2 = (int)a->p2.state; printf("%7.3f s    Phase 2 %s (error %d)\n", src / 8000.0, p2n[last_p2], a->p2.error); }
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
            if (a->phase == ANS_V34) {
                static int ls = -1, lm = -1; const v34dx_t *x = &a->v34.dx;
                if ((int)x->state != ls) { ls = (int)x->state; printf("%7.3f s    V.34 state %d (error %d), rates tx %d rx %d\n", src / 8000.0, ls, (int)x->error, x->tx_rate, x->rx_rate); }
                if (x->got_far_mp != lm) { const v34_mp_t *m = &x->far_mp; lm = x->got_far_mp;
                    if (lm) printf("%7.3f s    caller's MP: type %d, max ca %d ac %d, aux %d, trellis %d, nonlinear %d, shaping %d, rates %04x, asym %d, h (%d,%d) (%d,%d) (%d,%d)\n"
                        "              our cfg: symbol rate %d, tx carrier %d/%d, rx carrier %d/%d, tx pre-emphasis %d\n", src / 8000.0,
                        m->type, m->max_ca, m->max_ac, m->aux, m->trellis, m->nonlinear, m->shaping, m->rates, m->asym,
                        m->h[0][0], m->h[0][1], m->h[1][0], m->h[1][1], m->h[2][0], m->h[2][1],
                        x->cfg.sym_rate, x->cfg.tx_car_num, x->cfg.tx_car_den, x->cfg.rx_car_num, x->cfg.rx_car_den, x->cfg.tx_preemph); }
            }
            {   /* how the V.92 engine ended, when the answerer has just left it */
                static int was;
                int is = a->phase == ANS_P3X || a->phase == ANS_V92;
                if (was && !is) {
                    const v92d_t *m = &a->u.v92;
                    printf("%7.3f s    INFO1a: sections %d, Ltot %d, Lmax %d, MD %d, UINFO %d\n", src / 8000.0, m->cfg.sections, m->cfg.ltot, m->cfg.lmax, m->cfg.md_len, m->cfg.uinfo);
                    printf("%7.3f s    V.92 engine left in state %d, error %d, after %ld samples in it; Ja %d CPt %d E1u %d SUVu %d CPu %d ack %d E2u %d, design %d (step state %d, %d steps), "
                           "noise %lu q %lu echo %lu, estimate residual %lu, eq mse %lu, cursor %d, peak1 %ld, Jp frac %u, DIL N %d, Ja rates %05lx, B1u bad %ld left %ld\n", src / 8000.0,
                           (int)m->state, m->error, (long)m->t, m->got_ja, m->got_cpt, m->got_e1u, m->got_suvu, m->got_cpu, m->got_ack, m->got_e2u, m->dsn_state, m->dsn.st, m->dsn.steps,
                           (unsigned long)m->noise, (unsigned long)m->q_quiet, (unsigned long)m->echo_rms, (unsigned long)m->trn.res, (unsigned long)m->trn.mse, m->trn.cpk, (long)m->peak1,
                           (unsigned)m->jp.frac, m->ja.dil.n, (unsigned long)m->ja.rates, (long)m->b1_bad, (long)m->b1_left);
                }
                was = is;
            }
            if (a->phase == ANS_P3X || a->phase == ANS_V92) {
                /* when each of the caller's Phase 4 signals was taken as received */
                static int lg = -1; const v92d_t *m = &a->u.v92;
                int g = m->got_ja | m->got_cpt << 1 | m->got_e1u << 2 | m->got_suvu << 3 | m->got_cpu << 4 | m->got_ack << 5 | m->got_e2u << 6 | m->dsn_state << 7;
                { static int lsig = -1; int sg = (int)v90tx_signal(&m->tx); if (m->state >= 7 && sg != lsig) { lsig = sg; printf("%7.3f s    V.92 sending signal %d (CPd is %d bits)\n", src / 8000.0, sg, m->ncpd); } }
                if (g != lg) { lg = g; printf("%7.3f s    V.92 heard: Ja %d CPt %d E1u %d SUVu %d CPu %d ack %d E2u %d; design %d (%d steps); sending signal %d\n", src / 8000.0,
                    m->got_ja, m->got_cpt, m->got_e1u, m->got_suvu, m->got_cpu, m->got_ack, m->got_e2u, m->dsn_state, m->dsn.steps, (int)v90tx_signal(&m->tx)); }
            }
            if (a->phase == ANS_P3X || a->phase == ANS_V92) {
                static int ld = -1; const v92d_t *m = &a->u.v92;
                if (m->dsn_state != ld) { ld = m->dsn_state;
                    if (ld == 2) printf("%7.3f s    CPd designed: %d bit/s (drn %d), trellis %d, LC %d (smallest point %d, largest %d), M0 %d, LZ2 %d LP1 %d, 4G %u; %d steps; "
                        "noise %lu, quantiser quiet %lu / training %lu, echo %lu (given %lu), estimate residual %lu, rho %.2f, ISI %.1f dB, x rms max %lu, power %.2f\n", src / 8000.0,
                        V92_UP_BPS(m->cpd.drn), m->cpd.drn, m->cpd.map.trellis, m->cpd.map.lc[0], (int)v92_point(&m->cpd.map, 0, 0), (int)v92_point(&m->cpd.map, 0, m->cpd.map.lc[0] - 1),
                        m->cpd.map.M[0], m->cpd.filt.lz2, m->cpd.filt.lp1, (unsigned)m->cpd.filt.gain4, m->dsn.steps, (unsigned long)m->dsn.cfg.noise, (unsigned long)m->dsn.cfg.q_quiet,
                        (unsigned long)m->dsn.cfg.q_trn, (unsigned long)m->dsn.cfg.echo_rms, (unsigned long)m->echo_rms, (unsigned long)m->dsn.cfg.res, m->dsn.rho_q8 / 256.0,
                        10 * log10(m->dsn.isi_q28 / 268435456.0 + 1e-12), (unsigned long)m->dsn.xrms_max, m->dsn.power_q14 / 16384.0);
                }
            }
            if (a->phase == ANS_P3X || a->phase == ANS_V92) {
                static int ls = -1, lu = -1;
                if ((int)a->u.v92.state != ls || a->u.v92.up_rate != lu) { ls = (int)a->u.v92.state; lu = a->u.v92.up_rate;
                    printf("%7.3f s    V.92 state %d (error %d), %d bit/s up, %d down, PCM upstream %s; peak1 %ld, Jp frac %u\n", src / 8000.0, ls, a->u.v92.error, lu, a->u.v92.down_rate, a->pcm_up ? "offered" : "withdrawn", (long)a->u.v92.peak1, (unsigned)a->u.v92.jp.frac); }
            }
            if (a->phase == ANS_P3 && (int)a->p34.state != last_p34) { last_p34 = (int)a->p34.state; printf("%7.3f s    Phase 3/4 state %d (error %d)\n", src / 8000.0, last_p34, a->p34.error); }
        }
        if (pass == 0) printf("answer tone at %.3f s in the recording; shift %ld samples\n", rec_ans / 8000.0, shift);
    }
    for (i = 0; i < 120; i++) if (dsec[i]) printf("  second %d: %ld samples differ\n", i, dsec[i]);
    printf("%ld samples; after the answer our transmit differs from the recording in %ld (first at %.3f s)\n", len, diff, first / 8000.0);
    return 0;
}
