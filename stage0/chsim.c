/* Stage 0c tool validation only (PC side, uses double): turn a mu-law file into
 * a simulated 48 kHz sound card capture, with selectable impairments, so the
 * analyser can be shown to tell a clean path from an altered one.
 *
 * usage: chsim in.ulaw out.wav [ppm=N] [noise=dB] [gain=dB] [alaw] [slip=N] [dup=N] [invert] [lead=ms]
 *   ppm    sound card clock offset relative to the box
 *   noise  white noise level in dB relative to full scale (e.g. -70)
 *   gain   digital gain applied to linear samples, then re-encoded to mu-law
 *   alaw   mu-law -> A-law -> mu-law transcode
 *   echo=dB  write an 8 kHz mu-law file instead: the pattern as it would return in
 *            upstream RTP through a hybrid with this echo gain (out name must end .ulaw)
 *   slip   drop one sample at 8 kHz index N;  dup: repeat one sample at index N
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "g711.h"

#define MAXN (1 << 20)
#define HALF 24 /* interpolation half-width in 8 kHz samples */
static double x[MAXN];
static uint8_t raw[MAXN];

static double nrand(uint32_t *s)
{
    double a = 0;
    int i;
    for (i = 0; i < 12; i++) { *s = *s * 1664525u + 1013904223u; a += (double)(*s >> 8) / 16777216.0; }
    return a - 6.0;
}

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

int main(int argc, char **argv)
{
    double ppm = 0, noise_db = -200, gain_db = 0, lead_ms = 137.3, pi = acos(-1.0), echo_db = 1;
    long slip = -1, dup = -1, n, i, m, nout;
    int alaw = 0, invert = 0, a;
    uint32_t seed = 99;
    FILE *f;

    if (argc < 3) { fprintf(stderr, "usage: chsim in.ulaw out.wav [opts]\n"); return 2; }
    for (a = 3; a < argc; a++) {
        if (!strncmp(argv[a], "ppm=", 4)) ppm = atof(argv[a] + 4);
        else if (!strncmp(argv[a], "noise=", 6)) noise_db = atof(argv[a] + 6);
        else if (!strncmp(argv[a], "gain=", 5)) gain_db = atof(argv[a] + 5);
        else if (!strncmp(argv[a], "echo=", 5)) echo_db = atof(argv[a] + 5);
        else if (!strncmp(argv[a], "slip=", 5)) slip = atol(argv[a] + 5);
        else if (!strncmp(argv[a], "dup=", 4)) dup = atol(argv[a] + 4);
        else if (!strncmp(argv[a], "lead=", 5)) lead_ms = atof(argv[a] + 5);
        else if (!strcmp(argv[a], "alaw")) alaw = 1;
        else if (!strcmp(argv[a], "invert")) invert = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[a]); return 2; }
    }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    n = (long)fread(raw, 1, MAXN - 8, f);
    fclose(f);

    for (i = 0, m = 0; i < n; i++) {
        int32_t v = ulaw_to_linear(raw[i]);
        if (gain_db != 0) v = ulaw_to_linear(linear_to_ulaw((int32_t)lrint(v * pow(10, gain_db / 20))));
        if (alaw) v = ulaw_to_linear(linear_to_ulaw(alaw_to_linear(linear_to_alaw(v))));
        if (i == slip) continue;
        x[m++] = v;
        if (i == dup) x[m++] = v;
    }
    n = m;

    if (echo_db <= 0) { /* hybrid echo, sampled by the box codec at the same 8 kHz clock */
        double g = pow(10, echo_db / 20);
        f = fopen(argv[2], "wb");
        if (!f) { perror(argv[2]); return 1; }
        for (m = 0; m < n + 400; m++) {
            long k = m - 163; /* constant round-trip delay */
            double v = 0;
            if (k >= 5 && k < n) v = g * (0.55 * x[k - 2] - 0.35 * x[k - 3] + 0.2 * x[k - 4] - 0.1 * x[k - 5]);
            v += 32768.0 * pow(10, noise_db / 20) * nrand(&seed);
            fputc(linear_to_ulaw((int32_t)lrint(v)), f);
        }
        fclose(f);
        return 0;
    }

    nout = (long)((n / 8000.0 + lead_ms / 1000.0 + 0.2) * 48000.0);
    f = fopen(argv[2], "wb");
    if (!f) { perror(argv[2]); return 1; }
    fwrite("RIFF", 1, 4, f); put32(f, 36 + (uint32_t)nout * 2); fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16); put16(f, 1); put16(f, 1); put32(f, 48000); put32(f, 96000); put16(f, 2); put16(f, 16);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)nout * 2);
    for (m = 0; m < nout; m++) {
        /* capture sample m is taken at box time t (in 8 kHz samples) */
        double t = (m / 48000.0 - lead_ms / 1000.0) * 8000.0 / (1.0 + ppm * 1e-6), y = 0, c = 0.92;
        long k0 = (long)floor(t), k;
        for (k = k0 - HALF + 1; k <= k0 + HALF; k++) {
            double d = t - k, w, sc;
            if (k < 0 || k >= n) continue;
            w = 0.5 + 0.5 * cos(pi * d / HALF);
            sc = fabs(d) < 1e-9 ? 1.0 : sin(pi * c * d) / (pi * c * d);
            y += x[k] * c * sc * w;
        }
        y = y * 0.5 + 32768.0 * pow(10, noise_db / 20) * nrand(&seed);
        if (invert) y = -y;
        if (y > 32767) y = 32767;
        if (y < -32768) y = -32768;
        put16(f, (unsigned)((int)lrint(y) & 0xFFFF));
    }
    fclose(f);
    return 0;
}
