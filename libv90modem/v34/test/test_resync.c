/* What the decoder does when the input is disturbed: an error burst, a lost
 * symbol, lost 4D symbols, a lost mapping frame. The numbers printed here
 * are the documentation of the resynchronisation behaviour (README.md,
 * "Resynchronisation"). */
#include "test.h"
#include "testutil.h"

#define PRE   24000UL           /* symbols before the disturbance */
#ifdef __arm__
#define POST  16000UL
#else
#define POST  40000UL           /* symbols after it */
#endif

static v34_params_t p;
static v34_tx_t tx;
static v34_rx_t rx;

/* The data is all ones (the modem's scrambler still randomises the line
 * signal). That makes the check independent of how many bits a slip
 * removes: whatever is delivered must be ones. Garbage shows as BER 0.5. */
typedef struct { unsigned long n; } ones_src_t;
static int ones_get(void *u) { ((ones_src_t *)u)->n++; return 1; }
typedef struct { unsigned long n, errs, last; } pos_sink_t;
static void pos_put(void *u, int bit)
{
    pos_sink_t *k = (pos_sink_t *)u;
    if (!(bit & 1)) {
        k->last = k->n;
        k->errs++;
    }
    k->n++;
}

static const int16_t H_STRONG[3][2] = { { -16384, 8192 }, { 3441, -8192 }, { 0, 1720 } };

typedef struct {
    unsigned long errs, bits_after;     /* bit errors, bits sent after the event */
    double recover_syms;                /* symbols from the end of the event to the last bit error */
    double q_before, q_after;           /* v34_rx_quality in units^2 */
    unsigned long shell_before, shell_after, frames_after;
    unsigned long reseeds;
} result_t;

/* kind 0: burst of `len` garbage symbols; kind 1: `len` symbols dropped */
static result_t disturb(unsigned S, unsigned R, const int16_t h[3][2], int kind, unsigned len,
                        double esn0_db)
{
    v34_cfg_t c;
    ones_src_t src;
    pos_sink_t sink;
    result_t res;
    double sigma, bits_per_sym;
    unsigned long n, frames_before, bits_at_event;

    memset(&c, 0, sizeof c);
    c.symbol_rate = (uint16_t)S; c.data_rate = (uint16_t)R; c.scrambler = V34_GPA;
    if (h) memcpy(c.h, h, sizeof c.h);
    CHECK_EQ_I(v34_params_init(&p, &c), 0);
    sigma = sqrt(p.energy_q8 / 256.0 / (2 * pow(10, esn0_db / 10)));
    bits_per_sym = (double)R / S;
    src.n = 0;
    memset(&sink, 0, sizeof sink);
    tu_seed = 0x2468ACE1u;
    v34_tx_init(&tx, &p, ones_get, &src);
    v34_rx_init(&rx, &p, pos_put, &sink);

    for (n = 0; n < PRE; n++) {
        v34_pt_t x;
        v34_tx_symbol(&tx, &x, NULL);
        v34_rx_put(&rx, tu_add_noise(x, sigma), NULL);
    }
    CHECK_EQ_I(sink.errs, 0);
    res.q_before = v34_rx_quality(&rx) / 256.0;
    res.shell_before = rx.shell_errors;
    frames_before = rx.frames;
    /* data bits the encoder had produced when the event ended */
    for (n = 0; n < len; n++) {
        v34_pt_t x, g;
        v34_tx_symbol(&tx, &x, NULL);
        if (kind == 0) {
            g.re = (int16_t)lrint((2 * tu_uniform() - 1) * p.ymax * V34_ONE);
            g.im = (int16_t)lrint((2 * tu_uniform() - 1) * p.ymax * V34_ONE);
            v34_rx_put(&rx, g, NULL);
        }
    }
    bits_at_event = src.n;
    for (n = 0; n < POST; n++) {
        v34_pt_t x;
        v34_tx_symbol(&tx, &x, NULL);
        v34_rx_put(&rx, tu_add_noise(x, sigma), NULL);
    }
    res.errs = sink.errs;
    res.bits_after = (unsigned long)(POST * bits_per_sym);
    /* a slip removes bits from the stream, so positions only mean something
     * for bursts */
    res.recover_syms = (sink.errs && sink.last > bits_at_event) ?
                       ((double)sink.last - (double)bits_at_event) / bits_per_sym : 0;
    res.q_after = v34_rx_quality(&rx) / 256.0;
    res.shell_after = rx.shell_errors - res.shell_before;
    res.frames_after = rx.frames - frames_before;
    res.reseeds = rx.reseeds;
    return res;
}

int main(void)
{
    result_t r;

    /* ---- error bursts: the decoder recovers by itself. The damage ends
     * within the Viterbi path memory, the mapping frame in progress, the
     * differential decoder (one 4D symbol) and the descrambler (23 bits). */
    printf("  error burst (garbage symbols), noise about 5 dB better than needed:\n");
    printf("    config                     burst  bit errors  recovered after (symbols past the burst)\n");
    {
        static const struct { unsigned S, R; int pre; unsigned len; } cs[] = {
            { 3200, 19200, 0, 1 }, { 3200, 19200, 0, 20 }, { 3200, 19200, 0, 400 },
            { 3200, 4800, 0, 20 }, { 3000, 28800, 0, 20 }, { 3200, 19200, 1, 20 },
            { 3200, 19200, 1, 400 } };
        for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            /* white noise on x is amplified by the decoder's whitening filter:
             * give the precoded cases 10 dB more */
            double snr = cs[i].R == 28800 ? 36.0 : cs[i].R == 4800 ? 14.0 : cs[i].pre ? 36.0 : 26.0;
            r = disturb(cs[i].S, cs[i].R, cs[i].pre ? H_STRONG : NULL, 0, cs[i].len, snr);
            printf("    %u/%-5u %-15s %5u  %10lu  %.0f\n", cs[i].S, cs[i].R,
                   cs[i].pre ? "strong precoder" : "no precoder", cs[i].len, r.errs, r.recover_syms);
            CHECK(r.recover_syms < 120);
            CHECK(r.errs < (cs[i].len + 120) * 12);
            CHECK(r.q_after < 4 * r.q_before + 0.01);
        }
    }

    /* ---- lost symbols: no recovery. What the monitors show. ---- */
    printf("  lost symbols (timing slip), no precoder, 26 dB Es/N0; quality = v34_rx_quality()/256\n");
    printf("    config       lost  BER after  quality before -> after (units^2)  bad shell frames after\n");
    {
        static const struct { unsigned S, R, len; } cs[] = {
            { 3200, 19200, 1 }, { 3200, 19200, 2 }, { 3200, 19200, 8 }, { 3200, 19200, 128 },
            { 3000, 19200, 1 }, { 3000, 19200, 2 }, { 3000, 19200, 8 }, { 3200, 4800, 1 }, { 3200, 4800, 2 } };
        for (unsigned i = 0; i < sizeof cs / sizeof cs[0]; i++) {
            double snr = cs[i].R == 4800 ? 14.0 : 26.0, ber;
            r = disturb(cs[i].S, cs[i].R, NULL, 1, cs[i].len, snr);
            ber = (double)r.errs / (double)r.bits_after;
            printf("    %u/%-5u  %4u  %.3f      %.3f -> %.3f                      %lu of %lu\n",
                   cs[i].S, cs[i].R, cs[i].len, ber, r.q_before, r.q_after, r.shell_after, r.frames_after);
            CHECK_EQ_I(r.shell_before, 0);
            if (cs[i].len & 1) {
                /* an odd slip breaks the pairing into 4D symbols: the
                 * trellis decoder cannot find a code sequence and the
                 * quality figure jumps */
                CHECK(ber > 0.3);
                CHECK(r.q_after > 0.35);
                CHECK(r.q_after > 2.5 * r.q_before);
            } else if (cs[i].len == 2 && cs[i].R == 19200) {
                /* an even slip keeps the code sequence valid; the data is
                 * garbage but only the shell mapper check notices */
                CHECK(ber > 0.3);
                CHECK(r.shell_after * 20 > r.frames_after);
            } else if (cs[i].len == 128 && cs[i].S == 3200) {
                /* exactly one data frame lost: still aligned with data
                 * frames, only the bit inversion pattern is one frame off,
                 * which costs an error event at 6 of the 14 half frames */
                CHECK(ber > 0.001 && ber < 0.1);
            }
        }
    }
    T_REPORT("test_resync");
}
