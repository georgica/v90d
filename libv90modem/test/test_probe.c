/* Tests for the L1/L2 line probing generator and analyser. */
#include <string.h>
#include "test.h"
#include "../modem/probe.h"
#include "../sim/noise.h"

#define PI 3.14159265358979323846
static const int PH180[] = { 2, 11, 15, 18, 20, 21, 22, 23, 0 };   /* 300 1650 2250 2700 3000 3150 3300 3450 */

static int is180(int m) { int k; for (k = 0; PH180[k]; k++) if (PH180[k] == m) return 1; return 0; }

static int16_t buf[8000];

int main(void)
{
    probetx_t tx; proberx_t rx; rng_t rng;
    int i, m;

    CHECK(probe_has_tone(1)); CHECK(probe_has_tone(7)); CHECK(probe_has_tone(25));
    CHECK(!probe_has_tone(6)); CHECK(!probe_has_tone(8)); CHECK(!probe_has_tone(12)); CHECK(!probe_has_tone(16));
    CHECK(!probe_has_tone(0)); CHECK(!probe_has_tone(26));

    CHECK_NEAR(probe_db10(1000, 10), 200, 1);
    CHECK_NEAR(probe_db10(2, 1), 30, 1);
    CHECK_NEAR(probe_db10(1, 2), -30, 1);
    CHECK_NEAR(probe_db10((int64_t)3 << 40, 7), 10 * 10 * log10(pow(2, 40) * 3 / 7), 1);

    /* ---- generator: level, period, tone set and phases (Table 17/V.34) ---- */
    probetx_init(&tx, 2000);
    for (i = 0; i < 1600; i++) buf[i] = probetx_tick(&tx, 0);
    {
        double s = 0; int same = 1, pk = 0;
        for (i = 0; i < 1600; i++) { s += (double)buf[i] * buf[i]; if (abs(buf[i]) > pk) pk = abs(buf[i]); }
        CHECK_NEAR(sqrt(s / 1600), 2000.0, 40.0);
        for (i = 160; i < 1600; i++) if (buf[i] != buf[i - 160]) same = 0;
        CHECK(same);
        CHECK(pk < 2000 * 4);       /* crest factor below 12 dB */
        for (m = 1; m <= 26; m++) {
            double re = 0, im = 0, a;
            for (i = 0; i < 160; i++) {
                re += buf[i] * cos(2 * PI * 150.0 * m * i / 8000);
                im -= buf[i] * sin(2 * PI * 150.0 * m * i / 8000);
            }
            a = sqrt(re * re + im * im) / 80;
            if (!probe_has_tone(m)) { CHECK(a < 3.0); continue; }
            CHECK_NEAR(a, 2000.0 * sqrt(2.0 / 21.0), 12.0);
            CHECK_NEAR(re / 80 / a, is180(m) ? -1.0 : 1.0, 0.01);   /* cosine phase 0 or 180 */
        }
    }
    /* L1 is the same signal 6 dB up */
    {
        probetx_t a, b; int ok = 1;
        probetx_init(&a, 2000); probetx_init(&b, 2000);
        for (i = 0; i < 400; i++) {
            int x = probetx_tick(&a, 0), y = probetx_tick(&b, 1);
            if (abs(y - 2 * x) > 2) ok = 0;
        }
        CHECK(ok);
    }

    /* ---- analyser: levels through a tilted channel, SNR, noise floor ---- */
    {
        double nrms = 2000.0 * sqrt(1.0 / 21.0) / 31.6;   /* per-tone SNR 30 dB in a 50 Hz bin... */
        double z1 = 0;
        probetx_init(&tx, 2000); proberx_init(&rx); rng_seed(&rng, 42);
        CHECK_EQ_I(proberx_snr(&rx, 7), 0);
        CHECK_EQ_I(proberx_freq_off(&rx), -512);
        /* white noise whose power in one 50 Hz analysis bin is 30 dB below a tone:
         * sigma^2 * (50/4000) = tone power / 1000 */
        nrms = sqrt(2000.0 * 2000.0 / 21.0 / 1000.0 * 80.0);
        for (i = 0; i < 160 * 30; i++) {
            double x = probetx_tick(&tx, 0), y;
            y = x - 0.6 * z1; z1 = x;          /* high-pass tilt: |H|^2 = 1.36 - 1.2 cos w */
            proberx_tick(&rx, (int16_t)(y + nrms * rng_gauss(&rng)));
        }
        for (m = 1; m <= 25; m++) {
            double w = 2 * PI * 150.0 * m / 8000, h = sqrt(1.36 - 1.2 * cos(w));
            if (!probe_has_tone(m)) {
                CHECK_NEAR(proberx_noise(&rx, m), nrms * sqrt(50.0 / 4000.0), nrms * sqrt(50.0 / 4000.0) * 0.5);
                continue;
            }
            CHECK_NEAR(proberx_level(&rx, m), 2000.0 * sqrt(1.0 / 21.0) * h, 2000.0 * sqrt(1.0 / 21.0) * h * 0.06 + 3);
            CHECK_NEAR(proberx_snr(&rx, m) / 10.0, 30.0 + 20 * log10(h), 3.0);
        }
        CHECK_NEAR(proberx_freq_off(&rx), 0, 3);
    }
    /* clean signal: SNR saturates high */
    probetx_init(&tx, 2000); proberx_init(&rx);
    for (i = 0; i < 160 * 10; i++) proberx_tick(&rx, probetx_tick(&tx, 0));
    CHECK(proberx_snr(&rx, 7) > 450);

    /* ---- frequency offset of the 1050 Hz tone, 0.02 Hz units ---- */
    {
        static const double OFF[] = { 0.0, 0.5, -1.3, 4.0, -7.0 };
        int k;
        for (k = 0; k < 5; k++) {
            rng_seed(&rng, 7); proberx_init(&rx);
            for (i = 0; i < 160 * 25; i++) {
                double v = 0;
                for (m = 1; m <= 25; m++) if (probe_has_tone(m))
                    v += 300.0 * cos(2 * PI * (150.0 * m + OFF[k]) * i / 8000 + (is180(m) ? PI : 0));
                proberx_tick(&rx, (int16_t)(v + 30.0 * rng_gauss(&rng)));
            }
            CHECK_NEAR(proberx_freq_off(&rx) * 0.02, OFF[k], 0.12);
        }
    }
    /* no signal: offset not measurable */
    proberx_init(&rx); rng_seed(&rng, 3);
    for (i = 0; i < 160 * 10; i++) proberx_tick(&rx, (int16_t)(20.0 * rng_gauss(&rng)));
    CHECK_EQ_I(proberx_freq_off(&rx), -512);
    CHECK(proberx_snr(&rx, 7) < 60);

    T_REPORT("probe");
}
