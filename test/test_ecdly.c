/* Tests for the transmit history and the echo delay measurement, and for
 * the single tone watch. */
#include <string.h>
#include "test.h"
#include "../modem/ecdly.h"
#include "../modem/tonedet.h"
#include "../sim/noise.h"

/* A line whose echo starts `delay` samples after we send: a short decaying
 * path of gain g, and behind it a slow tail (one pole, 25 ms) of gain tail.
 * Runs the measurement over a white two-level signal; returns the delay
 * found, -1 if the measurement did not complete in ECDLY_SAMPLES. */
static int measure(int delay, double g, double tail, int diff, int *done_at)
{
    static ecdly_t d; static double line[8192];
    rng_t rng; double slow = 0;
    int k, i, done = 0;

    ecdly_init(&d, 0);
    memset(line, 0, sizeof line);
    rng_seed(&rng, 5);
    *done_at = 0;
    for (k = -3000; k <= ECDLY_SAMPLES; k++) {
        int16_t tx = rng_uniform(&rng) < 0.5 ? -3000 : 3000;
        double e = 0;
        line[(k + 3000) & 8191] = tx;
        for (i = 0; i < 12; i++) e += g * exp(-i / 3.0) * line[(k + 3000 - delay - i) & 8191];
        slow += (line[(k + 3000 - delay) & 8191] - slow) / 200.0;
        e += tail * slow;
        ecdly_put(&d, tx);
        (void)ecdly_ref(&d, 0);
        ecdly_next(&d);
        if (k >= 1 && ecdly_measure(&d, k, (int16_t)floor(e + 0.5), diff)) { done++; *done_at = k; }
    }
    return done == 1 ? d.delay : -1;
}

static int tone_blocks(uint32_t dph, double hz, double amp, double noise, int samples)
{
    tonedet_t t; rng_t rng; int i, first = -1;
    tonedet_init(&t, dph);
    rng_seed(&rng, 9);
    for (i = 0; i < samples; i++) {
        double v = amp * sin(2 * 3.14159265358979 * hz * i / 8000.0) + noise * (rng_uniform(&rng) - 0.5) * 3.46;
        if (tonedet_tick(&t, (int16_t)floor(v + 0.5)) && first < 0) first = i;
    }
    return first;
}

int main(void)
{
    int at, r;

    /* the history: what was put `delay` + back samples ago */
    {
        static ecdly_t d; int i;
        ecdly_init(&d, 7);
        CHECK_EQ_I(d.delay, 7);
        for (i = 0; i < 5000; i++) {
            ecdly_put(&d, (int16_t)i);
            if (i >= 4000) { CHECK_EQ_I(ecdly_ref(&d, 0), (int16_t)(i - 7)); }
            if (i == 4999) { CHECK_EQ_I(ecdly_ref(&d, 96), (int16_t)(i - 7 - 96)); CHECK_EQ_I(ecdly_ref(&d, 4000), (int16_t)(i - 4007)); }
            ecdly_next(&d);
        }
        ecdly_init(&d, -1);
        CHECK_EQ_I(d.delay, 0);                 /* not known: none until measured */
    }

    /* the measurement: a little before the start of the echo, both ways */
    r = measure(60, 0.2, 0, 0, &at);
    printf("  echo at 60: found %d (plain), done at sample %d of %d\n", r, at, ECDLY_SAMPLES);
    CHECK(r >= 60 - 12 && r <= 60);
    CHECK(at > ECDLY_MAXLAG + ECDLY_WIN && at <= ECDLY_SAMPLES);
    r = measure(60, 0.2, 0, 1, &at);
    CHECK(r >= 60 - 12 && r <= 60);
    r = measure(0, 0.2, 0, 1, &at);
    CHECK_EQ_I(r, 0);
    r = measure(1200, 0.1, 0, 1, &at);
    printf("  echo at 1200: found %d\n", r);
    CHECK(r >= 1200 - 12 && r <= 1200);

    /* a weak start of the echo under a slow tail many times its size: the
     * change from sample to sample finds the start */
    r = measure(270, 0.02, 4.0, 1, &at);
    printf("  echo at 270 under a slow tail: found %d (differences)\n", r);
    CHECK(r >= 270 - 12 && r <= 270 + 4);

    /* the tone watch: Tone A after more than 50 ms, at the latest 70 */
    r = tone_blocks(TONEDET_2400, 2400, 2000, 0, 1600);
    printf("  2400 Hz: on after %d samples\n", r);
    CHECK(r >= 400 && r < 560);
    r = tone_blocks(TONEDET_1200, 1200, 2000, 0, 1600);
    CHECK(r >= 400 && r < 560);
    /* with noise a quarter of its power beside it */
    CHECK(tone_blocks(TONEDET_2400, 2400, 2000, 700, 1600) >= 400);
    /* not: the other tone, noise alone, a tone too weak to count, silence */
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 1200, 2000, 0, 4000), -1);
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 1800, 2000, 0, 4000), -1);
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 2400, 0, 2000, 4000), -1);
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 2400, 10, 0, 4000), -1);
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 2400, 2000, 3000, 4000), -1);     /* less than half of the power */
    /* and 40 ms of it are not enough */
    CHECK_EQ_I(tone_blocks(TONEDET_2400, 2400, 2000, 0, 320), -1);

    T_REPORT("ecdly");
}
