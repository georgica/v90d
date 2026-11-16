/* Tests for the V.21 FSK modulator. */
#include "test.h"
#include "../modem/v21.h"
#include "../sim/g711.h"

#define FS 8000
#define MAXS 20000

static int16_t samp[MAXS];
static uint8_t bits[600];

/* Goertzel power at frequency f over n samples (double, test-side only). */
static double goertzel(const int16_t *x, int n, double f, int fs)
{
    double w = 2.0 * 3.14159265358979323846 * f / fs;
    double c = 2.0 * cos(w), s1 = 0, s2 = 0, s0;
    int i;
    for (i = 0; i < n; i++) { s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

int main(void)
{
    v21mod_t m;
    int i, ns;

    /* sample count ~ nbits * fs / baud */
    v21mod_init(&m, V21H_MARK, V21H_SPACE, FS, 300, 8192);
    for (i = 0; i < 300; i++) bits[i] = 1;
    ns = v21mod_run(&m, bits, 300, samp, MAXS);
    CHECK_NEAR((double)ns, 300.0 * FS / 300.0, 3.0); /* ~8000 samples */

    /* all-ones on V.21(H) -> energy concentrated at the mark freq 1650 */
    {
        double pm = goertzel(samp, ns, V21H_MARK, FS);
        double ps = goertzel(samp, ns, V21H_SPACE, FS);
        CHECK(pm > ps * 50.0);
    }

    /* all-zeros -> space freq 1850 dominates */
    v21mod_init(&m, V21H_MARK, V21H_SPACE, FS, 300, 8192);
    for (i = 0; i < 300; i++) bits[i] = 0;
    ns = v21mod_run(&m, bits, 300, samp, MAXS);
    {
        double pm = goertzel(samp, ns, V21H_MARK, FS);
        double ps = goertzel(samp, ns, V21H_SPACE, FS);
        CHECK(ps > pm * 50.0);
    }

    /* low channel: all-ones -> 980 Hz dominates over 1180 Hz */
    v21mod_init(&m, V21L_MARK, V21L_SPACE, FS, 300, 8192);
    for (i = 0; i < 300; i++) bits[i] = 1;
    ns = v21mod_run(&m, bits, 300, samp, MAXS);
    {
        double pm = goertzel(samp, ns, V21L_MARK, FS);
        double ps = goertzel(samp, ns, V21L_SPACE, FS);
        CHECK(pm > ps * 50.0);
    }

    /* amplitude ~ requested (peak of a mark tone near amp) */
    {
        int16_t pk = 0;
        for (i = 0; i < ns; i++) { int16_t a = samp[i] < 0 ? -samp[i] : samp[i]; if (a > pk) pk = a; }
        CHECK_NEAR((double)pk, 8192.0, 8192.0 * 0.05);
    }

    /* continuous phase: no sample-to-sample jump larger than the max slope of a
     * 1850 Hz tone at amp (|dx| <= amp * 2*pi*f/fs, with margin) */
    v21mod_init(&m, V21H_MARK, V21H_SPACE, FS, 300, 8192);
    for (i = 0; i < 20; i++) bits[i] = (uint8_t)(i & 1); /* alternating, forces transitions */
    ns = v21mod_run(&m, bits, 20, samp, MAXS);
    {
        double maxslope = 8192.0 * (2.0 * 3.14159265 * 1850.0 / FS) * 1.25;
        int bad = 0;
        for (i = 1; i < ns; i++) { int d = samp[i] - samp[i-1]; if (d < 0) d = -d; if (d > maxslope) bad = 1; }
        CHECK(bad == 0);
    }

    /* ---- modulator -> demodulator loopback ---- */
    {
        static uint8_t tx[160], rx[400];
        static int16_t wav[MAXS];
        v21mod_t mod; v21demod_t dem;
        uint32_t s = 7;
        int n, nbits, best_err, off;

        for (i = 0; i < 16; i++) tx[i] = 1;                 /* mark preamble */
        for (i = 16; i < 144; i++) { s = s * 1103515245u + 12345u; tx[i] = (s >> 20) & 1u; }
        nbits = 144;

        v21mod_init(&mod, V21H_MARK, V21H_SPACE, FS, 300, 8192);
        ns = v21mod_run(&mod, tx, nbits, wav, MAXS);
        v21demod_init(&dem, V21H_MARK, V21H_SPACE, FS, 300);
        n = v21demod_run(&dem, wav, ns, rx, 400);
        CHECK_NEAR((double)n, (double)nbits, 2.0);

        /* align within a few bits of latency; payload must recover exactly */
        best_err = 1 << 30;
        for (off = 0; off <= 4; off++) {
            int err = 0, j;
            for (j = 20; j < nbits - 2; j++)
                if (off + j < n && rx[off + j] != tx[j]) err++;
            if (err < best_err) best_err = err;
        }
        CHECK_EQ_I(best_err, 0);
    }

    /* ---- loopback through the mu-law PCM channel (quantization only) ---- */
    {
        static uint8_t tx[160], rx[400];
        static int16_t wav[MAXS];
        v21mod_t mod; v21demod_t dem;
        uint32_t s = 99;
        int n, nbits, best_err, off, j;

        for (i = 0; i < 16; i++) tx[i] = 1;
        for (i = 16; i < 144; i++) { s = s * 1103515245u + 12345u; tx[i] = (s >> 20) & 1u; }
        nbits = 144;

        v21mod_init(&mod, V21H_MARK, V21H_SPACE, FS, 300, 8192);
        ns = v21mod_run(&mod, tx, nbits, wav, MAXS);
        /* pass each sample through mu-law encode/decode (the PCM highway) */
        for (j = 0; j < ns; j++) wav[j] = (int16_t)ulaw_to_linear(linear_to_ulaw(wav[j]));

        v21demod_init(&dem, V21H_MARK, V21H_SPACE, FS, 300);
        n = v21demod_run(&dem, wav, ns, rx, 400);
        best_err = 1 << 30;
        for (off = 0; off <= 4; off++) {
            int err = 0;
            for (j = 20; j < nbits - 2; j++)
                if (off + j < n && rx[off + j] != tx[j]) err++;
            if (err < best_err) best_err = err;
        }
        CHECK_EQ_I(best_err, 0);
    }

    T_REPORT("v21");
}
