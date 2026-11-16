/* V.32bis receiver alone, fed by the calling test peer's transmitter playing
 * the caller's S, S-bar, TRN, R2, E, B1 and data (5.2, 5.3, 6.1). */
#include "test.h"
#include "v32_priv.h"
#include "v32call.h"
#include "noise.h"

static uint32_t tx_l, rx_l; static long nbits, nerr, nskip, nskip_bad;
static int  getbit(void *u) { (void)u; tx_l = tx_l * 1103515245u + 12345u; return (tx_l >> 16) & 1; }
static void putbit(void *u, int b)
{
    (void)u;
    /* the rest of B1 first: ones */
    if (nskip > 0) { nskip--; nskip_bad += !b; return; }
    rx_l = rx_l * 1103515245u + 12345u; nbits++; nerr += b != (int)((rx_l >> 16) & 1);
}

static v32rx_t rx;
static vc_tx tx;

/* level: line RMS; snr_db: white noise in the whole 4 kHz (<= 0: none) */
static void run(int rate, double level, double snr_db, int trn, int nsym_data)
{
    rng_t rng;
    long n, seg = 0;
    int phase = 0, e_sent = 0;
    unsigned word = 0x0991u | 1u << 10 | 1u << 6 | 1u << 9 | 1u << 5 | 1u << 3;
    static const unsigned bit[5] = { 1u << 10, 1u << 6, 1u << 9, 1u << 5, 1u << 3 };
    double sigma = snr_db > 0 ? level / pow(10, snr_db / 20) : 0;
    uint32_t t_s = 0, t_train = 0, t_dd = 0, t_rate = 0;
    int32_t mse_trn = 0;
    double te_sum = 0, te_sq = 0; long te_n = 0;

    tx_l = rx_l = 1; nbits = nerr = 0;
    nskip = (200 - 128) * (2 + rate); nskip_bad = 0;      /* 104 is unclamped 128 T after E, B1 lasts 200 T here */
    rng_seed(&rng, 9);
    vc_tx_init(&tx, 0, level, getbit, 0);
    v32rx_init(&rx, 1, putbit, 0);
    for (n = 0; n < 400000; n++) {
        double s;
        if (vc_tx_symbol_due(&tx)) {
            seg++;
            switch (phase) {
            case 0: if (seg > 100) { vc_tx_set(&tx, VC_S); phase = 1; seg = 1; } break;
            case 1: if (seg > 300) { vc_tx_set(&tx, VC_SBAR); phase = 2; seg = 1; } break;
            case 2: if (seg > 16) { vc_tx_set(&tx, VC_TRN); phase = 3; seg = 1; } break;
            case 3: if (seg > trn) { tx.rword = word; vc_tx_set(&tx, VC_RATE); phase = 4; seg = 1; } break;
            case 4: if (seg > 160 && tx.rbit == 0 && !e_sent) { tx.rword = 0xF991u | bit[rate]; e_sent = 1; seg = 1; }
                    else if (e_sent && seg > 8) { tx.rate = rate; vc_tx_set(&tx, VC_ONES); phase = 5; seg = 1; }
                    break;
            case 5: if (seg > 200) { vc_tx_set(&tx, VC_DATA); phase = 6; seg = 1; } break;
            default: break;
            }
            if (phase == 6 && seg > nsym_data) break;
        }
        s = vc_tx_tick(&tx) + sigma * rng_gauss(&rng);
        v32rx_tick(&rx, (int32_t)lrint(s));
        /* what the modem's control does: start training when S is there */
        if (rx.mode == V32_RX_TONE && rx.s_run >= 40) { v32rx_set_mode(&rx, V32_RX_S); t_s = rx.nsamp; }
        if (rx.mode == V32_RX_TRAIN && !t_train) t_train = rx.nsamp;
        if (rx.mode == V32_RX_DD4 && !t_dd) { t_dd = rx.nsamp; mse_trn = rx.mse; }
        if (rx.rate_word && !t_rate) t_rate = rx.nsamp;
        if (rx.mode == V32_RX_DATA && rx.half == 0 && rx.ndata > 500) { te_n++; te_sum += rx.te; te_sq += (double)rx.te * rx.te; }
    }
    if (getenv("V32_DEBUG")) printf("    timing error in data: mean %.0f rms %.0f, integrator %d\n", te_sum / (te_n + 1), sqrt(te_sq / (te_n + 1)), (int)rx.t_integ);
    printf("  %5d bit/s level %5.0f snr %2.0f: S at %u, S-bar at %u, R at %u (word %04x x%d), E %04x; mse after TRN %d, in data %d; "
           "timing %d; %ld bits, %ld errors\n",
           V32_BITRATE(rate), level, snr_db, (unsigned)t_s, (unsigned)t_train, (unsigned)t_rate, rx.rate_word, rx.rate_seen, rx.e_word,
           (int)mse_trn, (int)rx.mse, (int)rx.te, nbits, nerr);
    CHECK(t_s > 0 && t_train > t_s && t_dd > t_train);
    /* S began at symbol 100: detected within 40 symbols of its arrival (filters included) */
    CHECK(t_s > 100 * 10 / 3 && t_s < (snr_db > 0 ? 250 : 165) * 10 / 3);
    if (snr_db <= 0) CHECK_EQ_I(nskip_bad, 0);
    /* S-bar begins 300 symbols later; recognised on its second symbol (+ filter delays) */
    CHECK_NEAR((double)t_train, (400 + 2) * 10.0 / 3 + VC_TX_DELAY + 16, 8);
    CHECK_EQ_I(rx.rate_word, word);
    CHECK_EQ_I(rx.e_word, 0xF991u | bit[rate]);
    CHECK_EQ_I(rx.mode, V32_RX_DATA);
    CHECK_EQ_I(rx.rate, rate);
    CHECK(nbits > (long)(nsym_data - 300) * (2 + rate));
    if (snr_db <= 0) { CHECK_EQ_I(nerr, 0); CHECK(rx.mse < 60); }
}

int main(void)
{
    int rate;
    for (rate = 0; rate < V32_NRATES; rate++) run(rate, 2000, 0, 1280, 6000);
    run(4, 160, 0, 1280, 6000);          /* -40 dBm0 */
    run(4, 8000, 0, 1280, 6000);
    run(4, 2000, 0, 4000, 6000);         /* a long TRN */
    run(4, 2000, 30, 1280, 20000);
    CHECK(nerr < 20);
    run(2, 2000, 24, 1280, 20000);
    CHECK(nerr < 20);
    run(0, 2000, 15, 1280, 20000);
    CHECK(nerr < 40);
    T_REPORT("test_rx");
}
