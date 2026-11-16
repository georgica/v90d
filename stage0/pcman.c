/* Stage 0c analyser (PC side, uses double): compare a 48 kHz mono 16-bit WAV
 * capture of the FXS port against the pattern in pat.h and report whether the
 * path from RTP codeword to analogue output looks bit-exact, scaled or altered.
 *
 *   timing    impulse train: clock offset, jitter, sample slips, missing pulses
 *   levels    128-step staircase vs the G.711 table (gain, compression, merged codes)
 *   linearity random codewords: per-block least-squares fit of a linear channel
 *             on the table values; the residual is what a V.90 client would see
 *             as noise that no equaliser can remove
 *
 * usage: pcman capture.wav [-v]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g711.h"
#include "pat.h"

static int R = 6;        /* capture samples per 8 kHz sample: 6 for a 48 kHz WAV, 1 for an RTP echo dump */
#define FS (8000 * R)
#define RMAX 6
static double *y;
static long ny;
static double PI;

static int read_ulaw(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    long i, n;
    if (!f) { perror(fn); return -1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    y = malloc(sizeof(double) * (size_t)(n + 1));
    for (i = 0; i < n; i++) { int c = fgetc(f); if (c == EOF) break; y[i] = (double)ulaw_to_linear((uint8_t)c); }
    ny = i;
    fclose(f);
    return 0;
}

static int read_wav(const char *fn)
{
    FILE *f = fopen(fn, "rb");
    unsigned char h[12], c[8], fmt[16];
    unsigned ch = 0, bits = 0, rate = 0;
    long i;
    if (!f) { perror(fn); return -1; }
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV file\n"); return -1; }
    while (fread(c, 1, 8, f) == 8) {
        unsigned long len = c[4] | (c[5] << 8) | (c[6] << 16) | ((unsigned long)c[7] << 24);
        if (!memcmp(c, "fmt ", 4)) {
            if (fread(fmt, 1, 16, f) != 16) return -1;
            ch = fmt[2]; rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16); bits = fmt[14];
            fseek(f, (long)len - 16, SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            if (rate != 48000 || bits != 16 || ch < 1) { fprintf(stderr, "need 48000 Hz 16-bit PCM (got %u Hz, %u bit, %u ch)\n", rate, bits, ch); return -1; }
            ny = (long)(len / (2 * ch));
            y = malloc(sizeof(double) * (size_t)ny);
            for (i = 0; i < ny; i++) {
                unsigned char s[2];
                if (fread(s, 1, 2, f) != 2) { ny = i; break; }
                y[i] = (double)(short)(s[0] | (s[1] << 8));
                if (ch > 1) fseek(f, 2 * (long)(ch - 1), SEEK_CUR);
            }
            fclose(f);
            return 0;
        } else {
            fseek(f, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    fprintf(stderr, "no data chunk\n");
    return -1;
}

static double yat(double t) /* cubic interpolation of the capture */
{
    long i = (long)floor(t);
    double d = t - i, a, b, c, e;
    if (i < 1 || i + 2 >= ny) return 0;
    a = y[i - 1]; b = y[i]; c = y[i + 1]; e = y[i + 2];
    return b + 0.5 * d * (c - a + d * (2 * a - 5 * b + 4 * c - e + d * (3 * (b - c) + e - a)));
}

/* Hann-windowed single-bin amplitude at frequency f (Hz) over [t0, t0+n) */
static double tone_amp(double t0, long n, double f)
{
    double re = 0, im = 0, ws = 0;
    long i, i0 = (long)floor(t0);
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * PI * (i + 0.5) / n), v = (i0 + i >= 0 && i0 + i < ny) ? y[i0 + i] : 0;
        re += w * v * cos(2 * PI * f * i / FS);
        im -= w * v * sin(2 * PI * f * i / FS);
        ws += w;
    }
    return 2 * sqrt(re * re + im * im) / ws;
}

static int cmpd(const void *a, const void *b) { double d = *(const double *)a - *(const double *)b; return d < 0 ? -1 : d > 0; }

/* Solve (A + ridge) h = b, A symmetric positive definite n x n, in place. */
static int chol_solve(double *A, double *b, int n)
{
    int i, j, k;
    for (i = 0; i < n; i++) {
        for (j = 0; j <= i; j++) {
            double s = A[i * n + j];
            for (k = 0; k < j; k++) s -= A[i * n + k] * A[j * n + k];
            if (i == j) { if (s <= 0) return -1; A[i * n + i] = sqrt(s); }
            else A[i * n + j] = s / A[j * n + j];
        }
    }
    for (i = 0; i < n; i++) { double s = b[i]; for (k = 0; k < i; k++) s -= A[i * n + k] * b[k]; b[i] = s / A[i * n + i]; }
    for (i = n - 1; i >= 0; i--) { double s = b[i]; for (k = i + 1; k < n; k++) s -= A[k * n + i] * b[k]; b[i] = s / A[i * n + i]; }
    return 0;
}

#define L 56      /* fit length in 8 kHz samples, per polyphase branch */
#define BLK 800   /* symbols per fit block (100 ms) */

int main(int argc, char **argv)
{
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    double emax = 0, peak = 0, s0, rate, ppm, slope, jit = 0, med_amp;
    static double pos[PAT_IMP_COUNT], amp[PAT_IMP_COUNT], diff[PAT_IMP_COUNT], tmp[PAT_IMP_COUNT];
    static double lvl[128], dev[128], xs[PAT_PRBS_LEN], snr_db[PAT_PRBS_LEN / BLK];
    long i, k, onset = -1, nblk;
    int nslip = 0, nmiss = 0, njit = 0, polarity, u, merged = 0, nonmono = 0, nb = 0, nfit;
    double gain_num = 0, gain_den = 0, gain, maxdev = 0, rmsdev = 0, mw_amp, mw_exp, sq_k, snr_med, snr_min, floor_amp;
    int v_time, v_lvl, v_lin, minu = 0, ulo = 1, cap_ok, nbadblk = 0, echo, eb = 40;

    PI = acos(-1.0);
    if (argc < 2) { fprintf(stderr, "usage: pcman capture.wav [-v]\n"); return 2; }
    echo = strlen(argv[1]) > 5 && !strcmp(argv[1] + strlen(argv[1]) - 5, ".ulaw");
    if (echo) { R = 1; if (read_ulaw(argv[1])) return 1; printf("mode: 8 kHz mu-law echo capture (same clock as the box)\n"); }
    else if (read_wav(argv[1])) return 1;
    printf("capture: %ld samples, %.2f s\n", ny, (double)ny / FS);

    /* ---- locate the sync burst */
    eb = 40 * R;
    nblk = ny / eb;
    for (i = 0; i < nblk; i++) { double e = 0; for (k = 0; k < eb; k++) e += y[i * eb + k] * y[i * eb + k]; if (e > emax) emax = e; }
    for (i = 0; i < ny; i++) if (fabs(y[i]) > peak) peak = fabs(y[i]);
    for (i = 0; i < nblk && onset < 0; i++) {
        double e = 0;
        for (k = 0; k < eb; k++) e += y[i * eb + k] * y[i * eb + k];
        if (e > 0.1 * emax) onset = i * eb;
    }
    if (onset < 0 || peak < (echo ? 60 : 200)) { printf("VERDICT: no signal found (peak %.0f of 32768)\n", peak); return 1; }
    for (i = onset > eb ? onset - eb : 0; i < onset + eb; i++) if (fabs(y[i]) > 0.15 * peak) break;
    s0 = (double)i; /* capture index of pattern sample PAT_SYNC, +-1 ms */
    if (peak > 32000) printf("WARNING: capture clips (peak %.0f). Lower the input gain and repeat.\n", peak);
    printf("sync burst at %.3f s, capture peak %.0f (%.1f dBFS)\n", s0 / FS, peak, 20 * log10(peak / 32768));
#define CAP(n) (s0 + ((double)(n) - PAT_SYNC) * R * rate)
    rate = 1.0;

    /* ---- timing: impulse train */
    {
        double pred = CAP(PAT_IMP), win = 25 * R, period = PAT_IMP_PERIOD * R;
        int found = 0;
        for (k = 0; k < PAT_IMP_COUNT; k++) {
            long c = (long)floor(pred + 0.5), b = -1, j;
            double m = 0, a0, a1, a2, den;
            for (j = c - (long)win; j <= c + (long)win; j++)
                if (j > 0 && j < ny - 1 && fabs(y[j]) > m) { m = fabs(y[j]); b = j; }
            if (b < 0) { pos[k] = pred; amp[k] = 0; pred += period; continue; }
            a0 = fabs(y[b - 1]); a1 = fabs(y[b]); a2 = fabs(y[b + 1]);
            den = a0 - 2 * a1 + a2;
            pos[k] = b + (den != 0 ? 0.5 * (a0 - a2) / den : 0);
            amp[k] = y[b];
            pred = pos[k] + period;
            win = 10 * R; /* +-10 symbols once locked */
            found++;
        }
        for (k = 0; k < PAT_IMP_COUNT; k++) tmp[k] = fabs(amp[k]);
        qsort(tmp, PAT_IMP_COUNT, sizeof(double), cmpd);
        med_amp = tmp[PAT_IMP_COUNT / 2];
        for (k = 1; k < PAT_IMP_COUNT; k++) { diff[k - 1] = pos[k] - pos[k - 1]; tmp[k - 1] = diff[k - 1]; }
        qsort(tmp, PAT_IMP_COUNT - 1, sizeof(double), cmpd);
        slope = tmp[(PAT_IMP_COUNT - 1) / 2];
        rate = slope / period;
        ppm = (rate - 1) * 1e6;
        polarity = 0;
        for (k = 0; k < PAT_IMP_COUNT; k++) polarity += amp[k] > 0 ? 1 : -1;
        for (k = 0; k < PAT_IMP_COUNT; k++) if (fabs(amp[k]) < 0.4 * med_amp) nmiss++;
        /* A slip is a step in pulse position that persists: compare the median
         * position error of the 8 pulses before and after each point. A single
         * noisy pulse moves one value, not the median, so noise cannot fake it. */
        {
            static double res[PAT_IMP_COUNT];
            double wa[8], wb[8];
            int q;
            for (k = 0; k < PAT_IMP_COUNT; k++) res[k] = pos[k] - (pos[0] + (double)k * slope);
            for (k = 8; k + 8 <= PAT_IMP_COUNT; k++) {
                double st;
                for (q = 0; q < 8; q++) { wb[q] = res[k - 8 + q]; wa[q] = res[k + q]; }
                qsort(wa, 8, sizeof(double), cmpd); qsort(wb, 8, sizeof(double), cmpd);
                st = (wa[3] + wa[4]) / 2 - (wb[3] + wb[4]) / 2;
                if (fabs(st) > 0.6 * R) {
                    nslip++;
                    if (nslip <= 10) printf("  SLIP at impulse %ld (t=%.3f s): %+.2f samples at 8 kHz\n", k, pos[k] / FS, st / R);
                    k += 8;
                }
            }
        }
        for (k = 0; k < PAT_IMP_COUNT - 1; k++) {
            double d = diff[k] - slope;
            if (fabs(d) < 0.6 * R) { jit += d * d; njit++; }
        }
        jit = njit ? sqrt(jit / njit) / R : 0;
        if (echo && fabs(ppm) < 5) rate = 1.0; /* same clock: do not interpolate */
        printf("timing:    clock offset %+.1f ppm, pulse jitter %.4f samples rms (8 kHz), slips %d, missing/attenuated pulses %d of %d, polarity %s\n",
               ppm, jit, nslip, nmiss, PAT_IMP_COUNT, polarity > 0 ? "normal" : "INVERTED");
        (void)found;
    }

    /* ---- levels: staircase against the G.711 table.
     * Fundamental of the 8-sample square is 1.30656 x the codeword's linear value;
     * the same factor applies to every step, so only ratios matter. */
    sq_k = 0.5 * sqrt(pow(1 + cos(PI / 4) + cos(PI / 2) + cos(3 * PI / 4), 2) + pow(sin(PI / 4) + sin(PI / 2) + sin(3 * PI / 4), 2));
    floor_amp = tone_amp(CAP(PAT_LEAD - 3000), 240 * R, 1000.0 / rate);
    if (echo && floor_amp < 0.5) floor_amp = 0.5; /* silence quantises to zero; use half an LSB-ish floor instead */
    for (u = 0; u < 128; u++)
        lvl[u] = tone_amp(CAP(PAT_STAIR + (127 - u) * PAT_STEP_LEN + 80), 240 * R, 1000.0 / rate);
    for (u = 64; u < 128; u++) { double e = sq_k * ucode_linear((unsigned)u); gain_num += lvl[u] * e; gain_den += e * e; }
    gain = gain_num / gain_den;
    while (ulo < 127 && lvl[ulo] < 300 * floor_amp) ulo++; /* 50 dB above the 1 kHz noise floor */
    for (u = 0; u < 128; u++) {
        double e = gain * sq_k * ucode_linear((unsigned)u);
        dev[u] = (e > 0 && lvl[u] > 0) ? 20 * log10(lvl[u] / e) : 0;
        if (u >= ulo && u >= 1) {
            if (fabs(dev[u]) > maxdev) maxdev = fabs(dev[u]);
            rmsdev += dev[u] * dev[u]; nb++;
            if (lvl[u] <= lvl[u - 1]) nonmono++;
            else if (20 * log10(lvl[u] / lvl[u - 1]) < 0.25 * 20 * log10((double)ucode_linear((unsigned)u) / ucode_linear((unsigned)u - 1))) merged++;
        }
        if (verbose) printf("  ucode %3d linear %5ld measured %9.2f dev %+7.3f dB\n", u, (long)ucode_linear((unsigned)u), lvl[u], dev[u]);
    }
    rmsdev = sqrt(rmsdev / nb);
    /* digital milliwatt: 0 dBm0 reference. Expected fundamental from its own table values. */
    {
        double re = 0, im = 0;
        for (k = 0; k < 8; k++) { double v = ulaw_to_linear(pat_milliwatt[k]); re += v * cos(PI * k / 4); im += v * sin(PI * k / 4); }
        mw_exp = gain * sqrt(re * re + im * im) / 4;
        mw_amp = tone_amp(CAP(PAT_MW + 800), R * 6400, 1000.0 / rate);
    }
    while (minu < 127 && lvl[minu] < 3 * floor_amp) minu++;
    printf("levels:    Ucodes %d..127 vs G.711 table: max deviation %.3f dB, rms %.3f dB, merged adjacent codes %d, non-monotonic %d\n",
           ulo, maxdev, rmsdev, merged, nonmono);
    printf("           digital milliwatt %+.3f dB vs staircase fit; noise floor at 1 kHz %.1f dB below Ucode 127 (smallest resolvable Ucode about %d)\n",
           20 * log10(mw_amp / mw_exp), 20 * log10(lvl[127] / (floor_amp > 0 ? floor_amp : 1e-9)),
           minu);
    cap_ok = lvl[127] > (echo ? 2000 : 30000) * floor_amp; /* 1 kHz bin floor at least 90 dB under Ucode 127 */
    v_time = nslip == 0 && nmiss == 0 && jit < 0.05;
    v_lvl = maxdev < (echo ? 0.15 : 0.06) && merged == 0 && nonmono == 0;

    /* ---- linearity: random codewords, block least squares on table values */
    {
        uint32_t seed = PAT_PRBS_SEED;
        static double A[L * L], bv[L], h[RMAX][L], zb[BLK * RMAX];
        int nblocks = (PAT_PRBS_LEN - 2 * L) / BLK, b, p, a, c2;
        for (i = 0; i < PAT_PRBS_LEN; i++) {
            uint32_t r = pat_lcg(&seed);
            double v = (double)ucode_linear((r >> 1) % PAT_PRBS_MAXU);
            xs[i] = ((r & 1u) ? v : -v) * (polarity > 0 ? 1 : -1);
        }
        nfit = 0;
        for (b = 0; b < nblocks; b++) {
            long n0 = L + (long)b * BLK; /* first symbol of block within PRBS */
            double sy = 0, se = 0;
            for (i = 0; i < BLK * R; i++) zb[i] = yat(CAP(PAT_PRBS + n0) + (double)i * rate);
            for (p = 0; p < R; p++) {
                memset(A, 0, sizeof(A)); memset(bv, 0, sizeof(bv));
                for (i = 0; i < BLK; i++) {
                    const double *xr = &xs[n0 + i - L / 2];
                    double yy = zb[i * R + p];
                    for (a = 0; a < L; a++) {
                        bv[a] += xr[a] * yy;
                        for (c2 = 0; c2 <= a; c2++) A[a * L + c2] += xr[a] * xr[c2];
                    }
                }
                for (a = 0; a < L; a++) A[a * L + a] *= 1.0 + 1e-9;
                if (chol_solve(A, bv, L)) { se = sy = 1; break; }
                memcpy(h[p], bv, sizeof(bv));
                for (i = 0; i < BLK; i++) {
                    const double *xr = &xs[n0 + i - L / 2];
                    double est = 0, yy = zb[i * R + p];
                    for (a = 0; a < L; a++) est += h[p][a] * xr[a];
                    sy += yy * yy; se += (yy - est) * (yy - est);
                }
            }
            snr_db[nfit++] = 10 * log10(sy / (se > 0 ? se : 1e-30));
            if (verbose) printf("  prbs block %3d t=%.2f s SNR %.1f dB\n", b, CAP(PAT_PRBS + n0) / FS, snr_db[nfit - 1]);
        }
        memcpy(tmp, snr_db, sizeof(double) * (size_t)nfit);
        qsort(tmp, (size_t)nfit, sizeof(double), cmpd);
        snr_med = tmp[nfit / 2]; snr_min = tmp[0];
        printf("linearity: random-codeword fit over %d blocks of %d ms: SNR median %.1f dB, worst block %.1f dB\n",
               nfit, BLK / 8, snr_med, snr_min);
    }
    for (i = 0; i < nfit; i++) if (snr_db[i] < snr_med - 6) nbadblk++;
    if (nbadblk) printf("           %d block(s) more than 6 dB below the median: a discontinuity (slip, repeated or concealed samples)\n", nbadblk);
    v_lin = snr_med > (echo ? 27 : 38) && nbadblk == 0;

    {
        const char *v;
        int rc = 3;
        if (!cap_ok && !(v_time && v_lvl && v_lin)) { v = "INCONCLUSIVE: capture too noisy or echo too weak to judge (quiet the room / mute the phone and repeat)"; rc = 4; }
        else if (nslip || nmiss || nbadblk) v = "ALTERED IN TIME (slips, lost, repeated or concealed samples): V.90 will not hold";
        else if (!v_lvl) v = "LEVELS do not follow the G.711 table (gain stage, transcoding or companding in the path)";
        else if (!v_time) v = "pulse timing jitters without slipping: resampling or a varying delay in the path";
        else if (!v_lin) v = "levels and timing fine but residual distortion is high: nonlinear or time-varying processing";
        else if (echo) { v = "no slips, no level remapping seen via echo (cannot prove the last fraction of a dB; see RUNBOOK)"; rc = 0; }
        else { v = "consistent with a bit-exact, slip-free path"; rc = 0; }
        printf("VERDICT:   timing %s, levels %s (checked down to Ucode %d), linearity %s, capture quality %s\n           => %s\n",
               v_time ? "OK" : "BAD", v_lvl ? "OK" : "BAD", ulo, v_lin ? "OK" : "BAD", cap_ok ? "good" : "POOR", v);
        return rc;
    }
}
