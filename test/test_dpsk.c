/* Tests for the Phase 2 tone / INFO DPSK modem. */
#include <string.h>
#include "test.h"
#include "../modem/dpsk.h"
#include "../sim/noise.h"

#define FS 8000

typedef rng_t noise_t;
static void noise_init(noise_t *n, uint32_t seed) { rng_seed(n, seed); }
/* white noise, level in dB relative to a full-scale sine */
static double noise_sample(noise_t *n, double dbfs)
{
    return rng_gauss(n) * 32768.0 / sqrt(2.0) * pow(10.0, dbfs / 20.0);
}
#define PI 3.14159265358979323846

static double goertzel(const int16_t *x, int n, double f)
{
    /* Hann-windowed single-bin power, normalised to amplitude^2 of a sine */
    double re = 0, im = 0, ws = 0; int i;
    for (i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * PI * i / n);
        re += w * x[i] * cos(2 * PI * f * i / FS);
        im += w * x[i] * sin(2 * PI * f * i / FS);
        ws += w;
    }
    return 4.0 * (re * re + im * im) / (ws * ws);
}

static double rms(const int16_t *x, int n)
{
    double s = 0; int i;
    for (i = 0; i < n; i++) s += (double)x[i] * x[i];
    return sqrt(s / n);
}

static int16_t buf[16000];

/* run a frame from tx (carrier fc, guard) into rx via gain/delay/noise/interferer;
 * returns number of frame events, payload copied to out */
static int loop_frame(int fc, int guard, const uint8_t *bits, int n, double gain, int delay,
                      double noise_dbfs, int echo, uint8_t *out, int corrupt)
{
    dpsktx_t tx, itx; dpskrx_t rx; noise_t nz;
    static int16_t dl[64];
    int i, ev, frames = 0, dpos = 0;
    uint8_t tmp[INFO_MAX_BITS];
    memcpy(tmp, bits, (size_t)n);
    if (corrupt) tmp[20] ^= 1;
    dpsktx_init(&tx, fc, 4000, guard);
    dpsktx_init(&itx, fc == DPSK_FC_A ? DPSK_FC_B : DPSK_FC_A, 8000, fc == DPSK_FC_B);
    dpskrx_init(&rx, fc);
    dpskrx_expect(&rx, INFO_PAYLOAD(n));
    noise_init(&nz, 1234);
    memset(dl, 0, sizeof dl);
    if (echo) dpsktx_on(&itx);
    for (i = 0; i < 4000; i++) {
        double v;
        if (i == 400) dpsktx_send(&tx, tmp, n);
        dl[dpos] = dpsktx_tick(&tx);
        v = gain * dl[(dpos - delay) & 63];
        dpos = (dpos + 1) & 63;
        if (echo) v += dpsktx_tick(&itx);
        if (noise_dbfs > -200) v += noise_sample(&nz, noise_dbfs);
        ev = dpskrx_tick(&rx, (int16_t)v);
        if (ev & DPSK_EV_FRAME) { frames++; memcpy(out, rx.frame, (size_t)INFO_PAYLOAD(n)); }
    }
    return frames;
}

int main(void)
{
    dpsktx_t tx; dpskrx_t rx;
    int i, k;

    /* ---- transmitter: tone B at the nominal level ---- */
    dpsktx_init(&tx, DPSK_FC_B, 4000, 0);
    for (i = 0; i < 200; i++) CHECK_EQ_I(dpsktx_tick(&tx), 0);   /* silent until turned on */
    dpsktx_on(&tx);
    for (i = 0; i < 4000; i++) buf[i] = dpsktx_tick(&tx);
    CHECK_NEAR(rms(buf + 400, 3200), 4000.0, 80.0);
    CHECK_NEAR(sqrt(goertzel(buf + 400, 3200, 1200.0) / 2), 4000.0, 80.0);
    /* a steady tone has no envelope ripple */
    {
        double mx = 0, mn = 1e9;
        for (k = 400; k + 20 <= 3600; k += 20) {
            double p = rms(buf + k, 20);       /* 20 samples = 3 carrier cycles */
            if (p > mx) mx = p;
            if (p < mn) mn = p;
        }
        CHECK(mx / mn < 1.01);
    }
    dpsktx_off(&tx);
    for (i = 0; i < 400; i++) buf[i] = dpsktx_tick(&tx);
    CHECK_NEAR(rms(buf + 200, 200), 0.0, 1.0);

    /* ---- tone A: carrier 1 dB down, guard tone 7 dB down ---- */
    dpsktx_init(&tx, DPSK_FC_A, 4000, 1);
    dpsktx_on(&tx);
    for (i = 0; i < 4000; i++) buf[i] = dpsktx_tick(&tx);
    CHECK_NEAR(20 * log10(sqrt(goertzel(buf + 400, 3200, 2400.0) / 2) / 4000.0), -1.0, 0.2);
    CHECK_NEAR(20 * log10(sqrt(goertzel(buf + 400, 3200, 1800.0) / 2) / 4000.0), -7.0, 0.2);

    /* ---- INFO modulation spectrum: 600 baud DPSK of pseudo-random bits ---- */
    {
        uint8_t bits[100]; unsigned s = 7; int rep;
        double p_in = 0, p_300 = 0, p_out = 0, f;
        dpsktx_init(&tx, DPSK_FC_B, 4000, 0);
        dpsktx_on(&tx);
        for (i = 0, rep = 0; i < 16000; i++) {
            if (!dpsktx_busy(&tx) && rep < 20) {
                for (k = 0; k < 100; k++) { s = s * 1103515245u + 12345u; bits[k] = (s >> 16) & 1; }
                dpsktx_send(&tx, bits, 100); rep++;
            }
            buf[i] = dpsktx_tick(&tx);
        }
        int nin = 0, n300 = 0, nout = 0;
        for (f = -60; f <= 60; f += 2, nin++) p_in += goertzel(buf, 16000, 1200 + f);
        for (f = 286; f <= 314; f += 2, n300 += 2)
            p_300 += goertzel(buf, 16000, 1200 - f) + goertzel(buf, 16000, 1200 + f);
        for (f = 600; f <= 1000; f += 10, nout += 2)
            p_out += goertzel(buf, 16000, 1200 - f) + goertzel(buf, 16000, 1200 + f);
        p_in /= nin; p_300 /= n300; p_out /= nout;
        CHECK_NEAR(10 * log10(p_300 / p_in), -3.0, 1.5);     /* Figure 13/V.34: -2 .. -4 at 300 */
        CHECK(10 * log10(p_out / p_in) < -35.0);
        /* DPSK keeps the nominal power (within 1 dB) */
        CHECK_NEAR(20 * log10(rms(buf + 400, 12000) / 4000.0), 0.0, 1.0);
    }

    /* ---- INFO frames through the loop ---- */
    {
        info0_t a; info1d_t d; uint8_t fr[INFO_MAX_BITS], got[INFO_MAX_BITS]; int n, dly;
        memset(&a, 0, sizeof a);
        a.sr3429 = 1; a.hi3200 = 1; a.max_sr_diff = 3; a.tx_power = 9; a.max_tx_power = 12;
        n = info0d_build(&a, fr);
        for (dly = 0; dly < 14; dly++) {       /* every sample phase of the symbol clock */
            memset(got, 9, sizeof got);
            CHECK_EQ_I(loop_frame(DPSK_FC_B, 0, fr, n, 1.0, dly, -300, 0, got, 0), 1);
            CHECK(memcmp(got, fr + 12, (size_t)INFO_PAYLOAD(n)) == 0);
        }
        /* a corrupted frame is not delivered */
        CHECK_EQ_I(loop_frame(DPSK_FC_B, 0, fr, n, 1.0, 3, -300, 0, got, 1), 0);
        /* weak, noisy, under the other side's tones. (The 1200 Hz receiver is
         * the test peer's: it rejects its own 1800 Hz guard tone by only 30 dB,
         * so it gets a milder echo than the 2400 Hz receiver below.) */
        CHECK_EQ_I(loop_frame(DPSK_FC_B, 0, fr, n, 0.1, 5, -70.0, 1, got, 0), 1);
        CHECK(memcmp(got, fr + 12, (size_t)INFO_PAYLOAD(n)) == 0);

        /* the analogue side's format: 2400 Hz + guard tone, INFO0a and the long INFO1d */
        n = info0a_build(&a, fr);
        CHECK_EQ_I(loop_frame(DPSK_FC_A, 1, fr, n, 1.0, 7, -300, 0, got, 0), 1);
        CHECK(memcmp(got, fr + 12, (size_t)INFO_PAYLOAD(n)) == 0);
        CHECK_EQ_I(loop_frame(DPSK_FC_A, 1, fr, n, 0.025, 2, -70.0, 1, got, 0), 1);
        memset(&d, 0, sizeof d);
        d.md_len = 5; d.sr[3].max_rate = 12; d.freq_off = 17;
        n = info1d_build(&d, fr);
        CHECK_EQ_I(loop_frame(DPSK_FC_B, 0, fr, n, 0.3, 11, -300, 0, got, 0), 1);
        CHECK(memcmp(got, fr + 12, (size_t)INFO_PAYLOAD(n)) == 0);
    }

    /* ---- tone detection and phase reversal timing ---- */
    {
        int fc, guard;
        for (fc = DPSK_FC_B, guard = 0; fc <= DPSK_FC_A; fc += 1200, guard = 1) {
            int nrev = 0, at = -1, ev, steady_before = 0;
            dpsktx_init(&tx, fc, 4000, guard);
            dpskrx_init(&rx, fc);
            for (i = 0; i < 4000; i++) {
                if (i == 100) dpsktx_on(&tx);
                if (i == 2000) { steady_before = rx.steady; dpsktx_reverse(&tx); }
                ev = dpskrx_tick(&rx, dpsktx_tick(&tx));
                if (ev & DPSK_EV_REVERSAL) { nrev++; at = i; }
                if (i == 99) CHECK_EQ_I(rx.carrier, 0);
            }
            CHECK_EQ_I(nrev, 1);
            CHECK_NEAR(at, 2000 + DPSK_TX_DELAY + DPSK_RX_REV_DELAY, 2);
            CHECK(steady_before > 1700);
            CHECK(rx.carrier);
            CHECK(rx.steady > 1800);        /* steady again after the reversal */
            CHECK_NEAR(dpskrx_level(&rx), guard ? 4000 * 0.891 : 4000, 150);
        }
    }
    /* an abrupt reversal from an ideal oscillator, weak and noisy, any sample phase */
    {
        int off;
        for (off = 0; off < 20; off += 3) {
            noise_t nz; int nrev = 0, at = -1, ev;
            dpskrx_init(&rx, DPSK_FC_A);
            noise_init(&nz, 99);
            for (i = 0; i < 4000; i++) {
                double v = 0;
                if (i >= 100) v = 300.0 * cos(2 * PI * 2400.0 * i / FS + 0.7) * (i >= 2000 + off ? -1 : 1)
                                + 150.0 * cos(2 * PI * 1800.0 * i / FS);
                v += noise_sample(&nz, -75.0);
                ev = dpskrx_tick(&rx, (int16_t)v);
                if (ev & DPSK_EV_REVERSAL) { nrev++; at = i; }
            }
            CHECK_EQ_I(nrev, 1);
            CHECK_NEAR(at, 2000 + off + DPSK_RX_REV_DELAY, 4);
        }
    }
    /* the reversals inside INFO sequences (also after a long tone) are not tone reversals */
    {
        info0_t a; uint8_t fr[INFO_MAX_BITS]; int n, evs = 0, frames = 0, ev, rep;
        memset(&a, 0, sizeof a);
        dpsktx_init(&tx, DPSK_FC_A, 4000, 1);
        dpskrx_init(&rx, DPSK_FC_A);
        dpskrx_expect(&rx, INFO_PAYLOAD(INFO0A_BITS));
        dpsktx_on(&tx);
        for (i = 0, rep = 0; i < 12000; i++) {
            if (i >= 1000 && !dpsktx_busy(&tx) && rep < 6 && (i % 1500) == 0) {
                a.sr2743 = (uint8_t)(rep & 1); a.max_sr_diff = (uint8_t)rep; a.ack = (uint8_t)(rep > 2);
                n = info0a_build(&a, fr);
                dpsktx_send(&tx, fr, n); rep++;
            }
            ev = dpskrx_tick(&rx, dpsktx_tick(&tx));
            if (ev & DPSK_EV_FRAME) frames++;
            if (ev & DPSK_EV_REVERSAL) evs++;
        }
        CHECK_EQ_I(frames, 6);
        CHECK_EQ_I(evs, 0);
    }
    /* noise alone, or the other band's tone, is not a carrier */
    {
        noise_t nz; int evs = 0;
        dpskrx_init(&rx, DPSK_FC_A);
        dpsktx_init(&tx, DPSK_FC_B, 8000, 0);
        dpsktx_on(&tx);
        noise_init(&nz, 5);
        for (i = 0; i < 8000; i++) {
            if (i == 4000) dpsktx_reverse(&tx);
            evs |= dpskrx_tick(&rx, (int16_t)(dpsktx_tick(&tx) + noise_sample(&nz, -60.0)));
        }
        CHECK_EQ_I(evs, 0);
        CHECK_EQ_I(rx.carrier, 0);
    }

    T_REPORT("dpsk");
}
