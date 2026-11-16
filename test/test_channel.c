/* Tests for the composed V.90 channel. */
#include "test.h"
#include "../sim/channel.h"
#include "../sim/g711.h"

static const double UNITY[1] = { 1.0 };

static channel_cfg clean_cfg(void)
{
    channel_cfg c;
    c.ds_pad_db = 0.0;
    c.ds_noise_dbfs = -300.0;
    c.ds_taps = UNITY; c.ds_ntaps = 1;
    c.us_noise_dbfs = -300.0;
    c.us_taps = UNITY; c.us_ntaps = 1;
    c.echo_gain = 0.0;
    c.echo_delay = 0;
    c.full_scale = 32124.0;
    c.seed = 1;
    return c;
}

int main(void)
{
    /* 1. downstream transparency: client sees exactly the decoded codeword */
    {
        channel_t ch;
        channel_cfg cfg = clean_cfg();
        uint8_t codes[5] = { 0x80, 0xFF, 0x00, 0xAA, 0x33 };
        int i;
        channel_init(&ch, &cfg);
        for (i = 0; i < 5; i++) {
            double cr; uint8_t sr;
            channel_tick(&ch, codes[i], 0.0, &cr, &sr);
            CHECK_NEAR(cr, (double)ulaw_to_linear(codes[i]), 1.0);
        }
    }

    /* 2. digital pad attenuates the downstream level by ~pad dB */
    {
        channel_t ch; channel_cfg cfg = clean_cfg();
        double cr; uint8_t sr;
        cfg.ds_pad_db = 6.0206;  /* factor 0.5 */
        channel_init(&ch, &cfg);
        channel_tick(&ch, 0x80, 0.0, &cr, &sr); /* +full scale 32124 */
        CHECK_NEAR(cr, 32124.0 * 0.5, 32124.0 * 0.01);
    }

    /* 3. downstream AWGN gives the requested SNR */
    {
        channel_t ch; channel_cfg cfg = clean_cfg();
        int i; double s2 = 0, n2 = 0; uint8_t sr;
        double level = (double)ulaw_to_linear(0x90);
        cfg.ds_noise_dbfs = -30.0;
        channel_init(&ch, &cfg);
        for (i = 0; i < 100000; i++) {
            double cr;
            channel_tick(&ch, 0x90, 0.0, &cr, &sr);
            s2 += level * level;
            n2 += (cr - level) * (cr - level);
        }
        {
            /* noise RMS at -30 dBFS of 32124 = 32124/sqrt2 * 10^-1.5 */
            double nrms = sqrt(n2 / 100000.0);
            double want = (32124.0 / sqrt(2.0)) * 0.0316227766;
            CHECK_NEAR(nrms, want, want * 0.05);
        }
    }

    /* 4. upstream transparency: server codeword encodes the client's analog */
    {
        channel_t ch; channel_cfg cfg = clean_cfg();
        double cr; uint8_t sr;
        double level = (double)ulaw_to_linear(0x95);
        channel_init(&ch, &cfg);
        channel_tick(&ch, 0xFF, level, &cr, &sr); /* ds silent (+0), us = level */
        CHECK_EQ_I(sr, linear_to_ulaw((int32_t)level));
    }

    /* 5. hybrid echo: with the client silent, upstream carries echo_gain * ds */
    {
        channel_t ch; channel_cfg cfg = clean_cfg();
        double cr; uint8_t sr; int i;
        cfg.echo_gain = 0.1; cfg.echo_delay = 0;
        channel_init(&ch, &cfg);
        for (i = 0; i < 4; i++) channel_tick(&ch, 0x80, 0.0, &cr, &sr); /* ds=+32124 */
        /* server upstream should decode to ~0.1 * 32124 = 3212 */
        CHECK_NEAR((double)ulaw_to_linear(sr), 3212.0, 120.0);
    }

    /* 6. echo delay places the leak the right number of samples later */
    {
        channel_t ch; channel_cfg cfg = clean_cfg();
        double cr; uint8_t sr; int i;
        double seen[12];
        cfg.echo_gain = 1.0; cfg.echo_delay = 5;
        channel_init(&ch, &cfg);
        /* one-sample downstream impulse at t=0, silence after */
        for (i = 0; i < 12; i++) {
            channel_tick(&ch, i == 0 ? 0x80 : 0xFF, 0.0, &cr, &sr);
            seen[i] = (double)ulaw_to_linear(sr);
        }
        CHECK_NEAR(seen[5], 32124.0, 200.0); /* echo lands at t=5 */
        CHECK_NEAR(seen[0], 0.0, 200.0);
        CHECK_NEAR(seen[4], 0.0, 200.0);
        CHECK_NEAR(seen[6], 0.0, 200.0);
    }

    T_REPORT("channel");
}
